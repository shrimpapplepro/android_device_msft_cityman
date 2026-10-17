/*
 * Copyright (C) 2015 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "lights"
#define DEBUG 0

#include <cutils/log.h>

#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <hardware/lights.h>

/* Android 11 lights.h dropped LIGHT_ID_FLASHLIGHT; torch is still
 * schematic TORCH_EN (GPIO 12) via set_light_flashlight. */
#ifndef LIGHT_ID_FLASHLIGHT
#define LIGHT_ID_FLASHLIGHT "flashlight"
#endif

/*
 * PMI8994 RGB: arch/arm/boot/dts/qcom/msm-pmi8994.dtsi qcom,leds@d000
 *   linux,name = "red" / "green" / "blue"
 *   WOA HWN0 HID QCOM24A3 (qchwnled8994.inf), ACPI RGB1 SPMI 0xD000
 *
 * Duke AMOLED backlight: panels/dsi-panel-duke-wqhd-dualdsi-cmd.dtsi
 *   qcom,mdss-dsi-bl-pmic-control-type = "bl_ctrl_dcs"
 *   mdss_fb.c led_classdev.name = "lcd-backlight"
 *
 * qpnp-wled @d800 linux,name = "wled" is not the panel path (DCS, not WLED).
 * leds-qpnp RGB is PWM without qcom,use-blink; mmo_defconfig has
 * CONFIG_LEDS_TRIGGER_TIMER=n — blink is brightness on/off, not on_off_ms.
 *
 * LOS 18.1 runtime is lights/ HIDL 2.0 (same sysfs, software blink).
 */

static pthread_once_t g_init = PTHREAD_ONCE_INIT;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static const char LCD_FILE[] = "/sys/class/leds/lcd-backlight/brightness";
static const char RED_LED_FILE[] = "/sys/class/leds/red/brightness";
static const char GREEN_LED_FILE[] = "/sys/class/leds/green/brightness";
static const char BLUE_LED_FILE[] = "/sys/class/leds/blue/brightness";
static const char RED_TRIGGER_FILE[] = "/sys/class/leds/red/trigger";
static const char GREEN_TRIGGER_FILE[] = "/sys/class/leds/green/trigger";
static const char BLUE_TRIGGER_FILE[] = "/sys/class/leds/blue/trigger";
static const char FLASH_TORCH_FILE[] =
    "/sys/class/leds/led:flash_torch/brightness";

struct led_config {
  unsigned int colorRGB;
  int onMS, offMS;
};

static struct led_config g_leds[3];
static int g_cur_led = -1;

static pthread_t g_blink_thread;
static int g_blink_running;
static int g_blink_thread_started;
static struct led_config g_blink_led;

static int write_int(char const *path, int value) {
  int fd = open(path, O_RDWR);
  if (fd < 0) {
    if (errno == ENOENT) {
      return 0; /* Hardware LED node not present; silent success */
    }
    ALOGE("write_int failed to open %s (%s)", path, strerror(errno));
    return -errno;
  }

  char buffer[32];
  int bytes = snprintf(buffer, sizeof(buffer), "%d\n", value);
  int amt = write(fd, buffer, bytes);
  int saved = errno;
  close(fd);
  if (amt == -1) {
    ALOGE("write_int failed to write %s (%s)", path, strerror(saved));
    return -saved;
  }
  return 0;
}

static int write_str(char const *path, char const *str) {
  int fd = open(path, O_RDWR);
  if (fd < 0) {
    if (errno == ENOENT) {
      return 0;
    }
    return -errno;
  }

  size_t len = strlen(str);
  int amt = write(fd, str, len);
  int saved = errno;
  close(fd);
  if (amt < 0 || (size_t)amt != len)
    return amt < 0 ? -saved : -EIO;
  return 0;
}

static int rgb_to_brightness(struct light_state_t const *state) {
  int color = state->color & 0x00ffffff;

  return ((77 * ((color >> 16) & 0x00ff)) + (150 * ((color >> 8) & 0x00ff)) +
          (29 * (color & 0x00ff))) >>
         8;
}

static int write_rgb(int red, int green, int blue) {
  int err = 0;
  int e;

  e = write_int(RED_LED_FILE, red);
  if (e)
    err = e;
  e = write_int(GREEN_LED_FILE, green);
  if (e)
    err = e;
  e = write_int(BLUE_LED_FILE, blue);
  if (e)
    err = e;
  return err;
}

static void rgb_claim_triggers(void) {
  /* Drop DT default-triggers (battery-charging / battery-full /
   * boot-indication). */
  write_str(RED_TRIGGER_FILE, "none");
  write_str(GREEN_TRIGGER_FILE, "none");
  write_str(BLUE_TRIGGER_FILE, "none");
}

static void sleep_blink_ms(int ms) {
  while (ms > 0) {
    int chunk = ms > 50 ? 50 : ms;
    usleep(chunk * 1000);
    ms -= chunk;
    pthread_mutex_lock(&g_lock);
    int run = g_blink_running;
    pthread_mutex_unlock(&g_lock);
    if (!run)
      return;
  }
}

static void *rgb_blink_thread(void *arg __unused) {
  for (;;) {
    int red, green, blue, onMS, offMS;

    pthread_mutex_lock(&g_lock);
    if (!g_blink_running) {
      pthread_mutex_unlock(&g_lock);
      break;
    }
    red = (g_blink_led.colorRGB >> 16) & 0xFF;
    green = (g_blink_led.colorRGB >> 8) & 0xFF;
    blue = g_blink_led.colorRGB & 0xFF;
    onMS = g_blink_led.onMS;
    offMS = g_blink_led.offMS;
    pthread_mutex_unlock(&g_lock);

    write_rgb(red, green, blue);
    sleep_blink_ms(onMS);

    pthread_mutex_lock(&g_lock);
    if (!g_blink_running) {
      pthread_mutex_unlock(&g_lock);
      break;
    }
    pthread_mutex_unlock(&g_lock);

    write_rgb(0, 0, 0);
    sleep_blink_ms(offMS);
  }
  return NULL;
}

static void stop_blink_locked(void) {
  if (!g_blink_thread_started)
    return;

  g_blink_running = 0;
  pthread_mutex_unlock(&g_lock);
  pthread_join(g_blink_thread, NULL);
  pthread_mutex_lock(&g_lock);
  g_blink_thread_started = 0;
}

void init_globals(void) {
  pthread_mutex_init(&g_lock, NULL);
  rgb_claim_triggers();
  write_rgb(0, 0, 0);
}

static int set_light_backlight(struct light_device_t *dev __unused,
                               struct light_state_t const *state) {
  int brightness = rgb_to_brightness(state);
  if (brightness > 0 && brightness < 2)
    brightness = 2;
  int err;

  pthread_mutex_lock(&g_lock);
  err = write_int(LCD_FILE, brightness);
  pthread_mutex_unlock(&g_lock);
  return err;
}

static int write_torch(int brightness) {
  return write_int(FLASH_TORCH_FILE, brightness);
}

static int set_light_flashlight(struct light_device_t *dev __unused,
                                struct light_state_t const *state) {
  int brightness = rgb_to_brightness(state);
  int err;

  pthread_mutex_lock(&g_lock);
  err = write_torch(brightness);
  pthread_mutex_unlock(&g_lock);
  return err;
}

static int write_leds_locked(struct led_config *led) {
  static const struct led_config led_off = {0, 0, 0};
  int red, green, blue;
  int err;

  if (led == NULL)
    led = (struct led_config *)&led_off;

  stop_blink_locked();
  rgb_claim_triggers();

  red = (led->colorRGB >> 16) & 0xFF;
  green = (led->colorRGB >> 8) & 0xFF;
  blue = led->colorRGB & 0xFF;

  if (led->colorRGB && led->onMS > 0 && led->offMS > 0) {
    g_blink_led = *led;
    g_blink_running = 1;
    if (pthread_create(&g_blink_thread, NULL, rgb_blink_thread, NULL) != 0) {
      g_blink_running = 0;
      ALOGE("rgb blink thread create failed, solid color");
      return write_rgb(red, green, blue);
    }
    g_blink_thread_started = 1;
    return 0;
  }

  err = write_rgb(red, green, blue);
  return err;
}

static int set_light_locked(struct light_state_t const *state, int type) {
  struct led_config *led;
  int err = 0;

  if (type < 0 || (unsigned int)type >= sizeof(g_leds) / sizeof(g_leds[0]))
    return -EINVAL;

  led = &g_leds[type];

  switch (state->flashMode) {
  case LIGHT_FLASH_TIMED:
  case LIGHT_FLASH_HARDWARE:
    led->onMS = state->flashOnMS;
    led->offMS = state->flashOffMS;
    break;
  case LIGHT_FLASH_NONE:
  default:
    led->onMS = 0;
    led->offMS = 0;
    break;
  }

#if DEBUG
  ALOGD("set_light_locked: mode %d, color=%08X, onMS=%d, offMS=%d, type=%d",
        state->flashMode, state->color, led->onMS, led->offMS, type);
#endif

  led->colorRGB = state->color & 0x00ffffff;

  if (led->colorRGB > 0) {
    if (type >= g_cur_led) {
      err = write_leds_locked(led);
      g_cur_led = type;
    }
  } else {
    if (type == g_cur_led) {
      int i;

      for (i = type - 1; i >= 0; i--) {
        if (g_leds[i].colorRGB > 0) {
          err = write_leds_locked(&g_leds[i]);
          goto switched;
        }
      }

      err = write_leds_locked(NULL);
    switched:
      g_cur_led = i;
    }
  }
  return err;
}

static int set_light_battery(struct light_device_t *dev __unused,
                             struct light_state_t const *state) {
  int err;

  pthread_mutex_lock(&g_lock);
  err = set_light_locked(state, 0);
  pthread_mutex_unlock(&g_lock);
  return err;
}

static int set_light_notifications(struct light_device_t *dev __unused,
                                   struct light_state_t const *state) {
  int err;

  pthread_mutex_lock(&g_lock);
  err = set_light_locked(state, 1);
  pthread_mutex_unlock(&g_lock);
  return err;
}

static int set_light_attention(struct light_device_t *dev __unused,
                               struct light_state_t const *state) {
  struct light_state_t fixed;
  int err;

  pthread_mutex_lock(&g_lock);

  memcpy(&fixed, state, sizeof(fixed));
  switch (fixed.flashMode) {
  case LIGHT_FLASH_NONE:
    fixed.color = 0;
    break;
  case LIGHT_FLASH_HARDWARE:
    if (fixed.flashOnMS > 0 && fixed.flashOffMS == 0)
      fixed.flashMode = LIGHT_FLASH_NONE;
    break;
  }

  err = set_light_locked(&fixed, 2);
  pthread_mutex_unlock(&g_lock);
  return err;
}

static int set_light_buttons(struct light_device_t *dev __unused,
                             struct light_state_t const *state __unused) {
  return 0;
}

static int close_lights(struct light_device_t *dev) {
  if (dev)
    free(dev);
  return 0;
}

static int open_lights(const struct hw_module_t *module, char const *name,
                       struct hw_device_t **device) {
  int (*set_light)(struct light_device_t *dev,
                   struct light_state_t const *state);

  if (!strcmp(LIGHT_ID_BACKLIGHT, name))
    set_light = set_light_backlight;
  else if (!strcmp(LIGHT_ID_FLASHLIGHT, name))
    set_light = set_light_flashlight;
  else if (!strcmp(LIGHT_ID_BUTTONS, name))
    set_light = set_light_buttons;
  else if (!strcmp(LIGHT_ID_NOTIFICATIONS, name))
    set_light = set_light_notifications;
  else if (!strcmp(LIGHT_ID_ATTENTION, name))
    set_light = set_light_attention;
  else if (!strcmp(LIGHT_ID_BATTERY, name))
    set_light = set_light_battery;
  else
    return -EINVAL;

  pthread_once(&g_init, init_globals);

  struct light_device_t *dev = malloc(sizeof(struct light_device_t));
  if (!dev)
    return -ENOMEM;

  memset(dev, 0, sizeof(*dev));

  dev->common.tag = HARDWARE_DEVICE_TAG;
  dev->common.version = 0;
  dev->common.module = (struct hw_module_t *)module;
  dev->common.close = (int (*)(struct hw_device_t *))close_lights;
  dev->set_light = set_light;

  *device = (struct hw_device_t *)dev;
  return 0;
}

static struct hw_module_methods_t lights_module_methods = {
    .open = open_lights,
};

struct hw_module_t HAL_MODULE_INFO_SYM = {
    .tag = HARDWARE_MODULE_TAG,
    .version_major = 1,
    .version_minor = 0,
    .id = LIGHTS_HARDWARE_MODULE_ID,
    .name = "Cityman lights",
    .author = "Google, Inc.",
    .methods = &lights_module_methods,
};
