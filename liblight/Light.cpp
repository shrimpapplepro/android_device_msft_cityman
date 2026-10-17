/*
 * Copyright (C) 2017 The LineageOS Project
 * Copyright (C) 2018 The Android Open Source Project
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

#define LOG_TAG "LightService"

#include "Light.h"

#include <log/log.h>

#include <algorithm>
#include <cmath>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <vector>

namespace android {
namespace hardware {
namespace light {
namespace V2_0 {
namespace implementation {

/*
 * Same nodes as liblight/lights.c.
 *
 * PMI8994 RGB: leds.dtsi linux,name = "red" / "green" / "blue"
 *   PWM, no qcom,use-blink. mmo_defconfig CONFIG_LEDS_TRIGGER_TIMER=n —
 *   TIMED flash is brightness on/off, not on_off_ms, not solid-on.
 *
 * Duke AMOLED backlight: mdss_fb.c led_classdev.name = "lcd-backlight"
 *   (DCS, not qpnp-wled @d800 "wled").
 *
 * Torch (GPIO 12, qpnp-flash-led) is not exposed here: light@2.0 Type has no
 * FLASHLIGHT member. It stays on the legacy LIGHT_ID_FLASHLIGHT node in
 * liblight/lights.c, and CameraManager torch goes through the camera HAL
 * set_torch_mode (QCamera2/util/QCameraTorch.cpp). Both write only
 * led:flash_torch; led:torch_0 is the red indicator.
 */

static const char kLcdFile[] = "/sys/class/leds/lcd-backlight/brightness";
static const char kRedLedFile[] = "/sys/class/leds/red/brightness";
static const char kGreenLedFile[] = "/sys/class/leds/green/brightness";
static const char kBlueLedFile[] = "/sys/class/leds/blue/brightness";
static const char kRedTriggerFile[] = "/sys/class/leds/red/trigger";
static const char kGreenTriggerFile[] = "/sys/class/leds/green/trigger";
static const char kBlueTriggerFile[] = "/sys/class/leds/blue/trigger";

static constexpr int kLedBattery = 0;
static constexpr int kLedNotifications = 1;
static constexpr int kLedAttention = 2;
static constexpr int kLedCount = 3;

static bool nodeWritable(const char* path) {
    int fd = TEMP_FAILURE_RETRY(open(path, O_RDWR));
    if (fd < 0)
        return false;
    close(fd);
    return true;
}

static int writeInt(const char* path, int value) {
    int fd = TEMP_FAILURE_RETRY(open(path, O_RDWR));
    if (fd < 0) {
        ALOGE("writeInt failed to open %s (%s)", path, strerror(errno));
        return -errno;
    }

    char buffer[32];
    int bytes = snprintf(buffer, sizeof(buffer), "%d\n", value);
    int amt = TEMP_FAILURE_RETRY(write(fd, buffer, bytes));
    int saved = errno;
    close(fd);
    if (amt == -1) {
        ALOGE("writeInt failed to write %s (%s)", path, strerror(saved));
        return -saved;
    }
    return 0;
}

static int writeStr(const char* path, const char* str) {
    int fd = TEMP_FAILURE_RETRY(open(path, O_RDWR));
    if (fd < 0)
        return -errno;

    size_t len = strlen(str);
    int amt = TEMP_FAILURE_RETRY(write(fd, str, len));
    int saved = errno;
    close(fd);
    if (amt < 0 || static_cast<size_t>(amt) != len)
        return amt < 0 ? -saved : -EIO;
    return 0;
}

static int rgbToBrightness(const LightState& state) {
    int color = state.color & 0x00ffffff;
    return ((77 * ((color >> 16) & 0x00ff)) + (150 * ((color >> 8) & 0x00ff)) +
            (29 * (color & 0x00ff))) >>
           8;
}

static Status errToStatus(int err) {
    return err == 0 ? Status::SUCCESS : Status::UNKNOWN;
}

Light::Light() {
    mBacklightOk = nodeWritable(kLcdFile);
    if (!mBacklightOk)
        ALOGE("%s missing or not writable (%s)", kLcdFile, strerror(errno));

    mRgbOk = nodeWritable(kRedLedFile) && nodeWritable(kGreenLedFile) &&
             nodeWritable(kBlueLedFile);
    if (!mRgbOk)
        ALOGE("RGB brightness nodes missing or not writable");
    else {
        rgbClaimTriggers();
        writeRgb(0, 0, 0);
    }
}

Light::~Light() {
    mLock.lock();
    stopBlinkLocked();
    mLock.unlock();
}

Return<Status> Light::setLight(Type type, const LightState& state) {
    std::lock_guard<std::mutex> lock(mLock);

    switch (type) {
        case Type::BACKLIGHT:
            return setBacklight(state);
        case Type::BATTERY:
            return setRgbLight(state, kLedBattery);
        case Type::NOTIFICATIONS:
            return setRgbLight(state, kLedNotifications);
        case Type::ATTENTION:
            return setAttention(state);
        case Type::BUTTONS:
            return Status::SUCCESS;
        default:
            return Status::LIGHT_NOT_SUPPORTED;
    }
}

Return<void> Light::getSupportedTypes(getSupportedTypes_cb _hidl_cb) {
    std::vector<Type> types;
    if (mBacklightOk)
        types.push_back(Type::BACKLIGHT);
    if (mRgbOk) {
        types.push_back(Type::BATTERY);
        types.push_back(Type::NOTIFICATIONS);
        types.push_back(Type::ATTENTION);
    }
    _hidl_cb(types);
    return Void();
}

Status Light::setBacklight(const LightState& state) {
    if (!mBacklightOk)
        return Status::UNKNOWN;
    int brightness = rgbToBrightness(state);
    if (brightness > 0 && brightness < 2)
        brightness = 2;
    return errToStatus(writeInt(kLcdFile, brightness));
}

Status Light::setRgbLight(const LightState& state, int type) {
    if (!mRgbOk)
        return Status::SUCCESS;
    return setLightLocked(state, type);
}

Status Light::setAttention(const LightState& state) {
    LightState fixed = state;
    switch (fixed.flashMode) {
        case Flash::NONE:
            fixed.color = 0;
            break;
        case Flash::HARDWARE:
            if (fixed.flashOnMs > 0 && fixed.flashOffMs == 0)
                fixed.flashMode = Flash::NONE;
            break;
        default:
            break;
    }
    return setRgbLight(fixed, kLedAttention);
}

Status Light::setLightLocked(const LightState& state, int type) {
    if (type < 0 || type >= kLedCount)
        return Status::LIGHT_NOT_SUPPORTED;

    LedConfig* led = &mLeds[type];

    switch (state.flashMode) {
        case Flash::TIMED:
        case Flash::HARDWARE:
            led->onMs = state.flashOnMs;
            led->offMs = state.flashOffMs;
            break;
        case Flash::NONE:
        default:
            led->onMs = 0;
            led->offMs = 0;
            break;
    }

    led->colorRgb = state.color & 0x00ffffff;

    int err = 0;
    if (led->colorRgb > 0) {
        if (type >= mCurLed) {
            err = writeLedsLocked(led);
            mCurLed = type;
        }
    } else if (type == mCurLed) {
        int next = -1;
        for (int i = type - 1; i >= 0; i--) {
            if (mLeds[i].colorRgb > 0) {
                next = i;
                break;
            }
        }
        if (next >= 0)
            err = writeLedsLocked(&mLeds[next]);
        else
            err = writeLedsLocked(nullptr);
        mCurLed = next;
    }
    return errToStatus(err);
}

int Light::writeLedsLocked(const LedConfig* led) {
    static const LedConfig kLedOff = {};
    if (led == nullptr)
        led = &kLedOff;

    stopBlinkLocked();
    rgbClaimTriggers();

    int red = (led->colorRgb >> 16) & 0xFF;
    int green = (led->colorRgb >> 8) & 0xFF;
    int blue = led->colorRgb & 0xFF;

    if (led->colorRgb && led->onMs > 0 && led->offMs > 0) {
        mBlinkLed = *led;
        mBlinkRunning = true;
        int err = pthread_create(&mBlinkThread, nullptr, &Light::blinkThreadEntry, this);
        if (err != 0) {
            mBlinkRunning = false;
            ALOGE("rgb blink thread create failed (%s), solid color", strerror(err));
            return writeRgb(red, green, blue);
        }
        mBlinkThreadStarted = true;
        return 0;
    }

    return writeRgb(red, green, blue);
}

int Light::writeRgb(int red, int green, int blue) {
    int err = 0;
    int e;

    e = writeInt(kRedLedFile, red);
    if (e)
        err = e;
    e = writeInt(kGreenLedFile, green);
    if (e)
        err = e;
    e = writeInt(kBlueLedFile, blue);
    if (e)
        err = e;
    return err;
}

void Light::rgbClaimTriggers() {
    writeStr(kRedTriggerFile, "none");
    writeStr(kGreenTriggerFile, "none");
    writeStr(kBlueTriggerFile, "none");
}

void Light::stopBlinkLocked() {
    if (!mBlinkThreadStarted)
        return;

    mBlinkRunning = false;
    mLock.unlock();
    pthread_join(mBlinkThread, nullptr);
    mLock.lock();
    mBlinkThreadStarted = false;
}

void Light::sleepBlinkMs(int ms) {
    while (ms > 0) {
        int chunk = ms > 50 ? 50 : ms;
        usleep(chunk * 1000);
        ms -= chunk;
        mLock.lock();
        int run = mBlinkRunning;
        mLock.unlock();
        if (!run)
            return;
    }
}

void* Light::blinkThreadEntry(void* arg) {
    static_cast<Light*>(arg)->blinkLoop();
    return nullptr;
}

void Light::blinkLoop() {
    for (;;) {
        int red, green, blue, onMs, offMs;

        mLock.lock();
        if (!mBlinkRunning) {
            mLock.unlock();
            break;
        }
        red = (mBlinkLed.colorRgb >> 16) & 0xFF;
        green = (mBlinkLed.colorRgb >> 8) & 0xFF;
        blue = mBlinkLed.colorRgb & 0xFF;
        onMs = mBlinkLed.onMs;
        offMs = mBlinkLed.offMs;
        mLock.unlock();

        writeRgb(red, green, blue);
        sleepBlinkMs(onMs);

        mLock.lock();
        if (!mBlinkRunning) {
            mLock.unlock();
            break;
        }
        mLock.unlock();

        writeRgb(0, 0, 0);
        sleepBlinkMs(offMs);
    }
}

}  // namespace implementation
}  // namespace V2_0
}  // namespace light
}  // namespace hardware
}  // namespace android
