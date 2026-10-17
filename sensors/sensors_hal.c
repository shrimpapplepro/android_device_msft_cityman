/*
 * cityman (Lumia 950 RM-1104) legacy sensors HAL.
 *
 *   /dev/i2c-4  AK09912 @ 0x0c, ZPA2326 @ 0x5c, ICM-20648 @ 0x68
 *   /dev/i2c-7  QPDS-T900 / APDS-9930 @ 0x39
 *   GPIO 42 / 75  hall (front / back cover)
 *
 * 0x68 is ICM-20648-class (WHO 0xAB), not stock MPU6500. It boots
 * asleep (PWR_MGMT_1=0x41). LSM6 / BMI / MPU probes remain as fallback.
 *
 * VINTF lists android.hardware.sensors@1.0 as passthrough, so
 * SensorService loads android.hardware.sensors@1.0-impl in-process
 * and that impl opens this module as sensors.cityman.
 *
 * Mag/gyro leftover from the first bring-up: chip axes were published
 * raw. Speaker hard-iron is ~300 uT, so AOSP fusion rejects mag
 * (|B| > 100 uT) and yaw walks at the gyro rest bias (~1 deg/s).
 * This HAL subtracts a still-detected gyro bias and a magnetometer
 * calibration that matches production stacks (PX4 / AKM-style):
 * least-squares sphere hard-iron, then a symmetric 3x3 soft-iron
 * ellipsoid. Mainline octagon DT has no mount-matrix; a full search
 * of the 48 axis maps used in Qualcomm/InvenSense board files does
 * not improve IGRF dip, so the chip frame is published as Android
 * device frame (identity), same as Linux IIO with no matrix.
 * Mag accuracy follows AOSP SensorEvent: field quality only, not
 * whether the phone is moving.
 */

#define LOG_TAG "CitymanSensors"

#include <errno.h>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <log/log.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <hardware/sensors.h>

#define I2C4_PATH "/dev/i2c-4"
#define I2C7_PATH "/dev/i2c-7"

#define AK_ADDR 0x0c
#define ZPA_ADDR 0x5c
#define IMU_ADDR 0x68
#define APDS_ADDR 0x39

#define HANDLE_MAG 1
#define HANDLE_PROX 2
#define HANDLE_LIGHT 3
#define HANDLE_PRESS 4
#define HANDLE_HALL_FRONT 5
#define HANDLE_HALL_BACK 6
#define HANDLE_ACCEL 7
#define HANDLE_GYRO 8
#define HANDLE_TEMP 9
#define HANDLE_ORIENTATION 10 // Ajout du handle d'orientation
#define N_SENSORS 12

#define IMU_NONE 0
#define IMU_LSM6 1
#define IMU_BMI 2
#define IMU_MPU 3
#define IMU_ICM 4

#define ICM_REG_WHO 0x00
#define ICM_REG_LP_CONFIG 0x05
#define ICM_REG_PWR_MGMT_1 0x06
#define ICM_REG_PWR_MGMT_2 0x07
#define ICM_REG_ACCEL_XOUT_H 0x2d
#define ICM_REG_GYRO_XOUT_H 0x33
#define ICM_REG_GYRO_CONFIG_1 0x01
#define ICM_REG_ACCEL_CONFIG 0x14
#define ICM_REG_BANK_SEL 0x7f
#define ICM_BANK0 0x00
#define ICM_BANK2 0x20
#define ICM_WHO_CITYMAN 0xab

#define HALL_FRONT_GPIO 42
#define HALL_BACK_GPIO 75
#define HALL_FRONT_PATH "/sys/class/gpio/gpio42/value"
#define HALL_BACK_PATH "/sys/class/gpio/gpio75/value"

#define QMAX 256

#define AK_WIA1 0x00
#define AK_ST1 0x10
#define AK_HXL 0x11
#define AK_CNTL2 0x31
#define AK_CNTL3 0x32
#define AK_WIA1_VAL 0x48
#define AK_WIA2_VAL 0x04
#define AK_CNTL2_POWER_DOWN 0x00
#define AK_CNTL2_CONT_20HZ 0x04
#define AK_CNTL2_CONT_50HZ 0x06
#define AK_CNTL2_FUSE_ROM 0x1F
#define AK_ASAX 0x60
#define AK_SCALE_UT 0.15f
#define CAL_PATH "/data/misc/cityman-sensors/cal.txt"
#define CAL_PATH_ALT "/data/system/cityman-sensors-cal.txt"
#define MAG_SPAN_MIN 35.0f
#define MAG_SPAN_WIDE 45.0f
#define MAG_RADIUS_MIN 20.0f
#define MAG_RADIUS_MAX 80.0f
#define MAG_EARTH_LOW 25.0f
#define MAG_EARTH_HIGH 65.0f
#define MAG_FIT_N 256
#define MAG_FIT_MIN 100
#define MAG_FIT_RMS_MAX 12.0f
#define MAG_DIP_MIN 0.40f
#define MAG_DIP_MAX 0.98f
#define MAG_STILL_COMMIT 20
#define GYRO_STILL_MAX 0.16f
#define GYRO_BIAS_MAX 0.25f
#define GYRO_STILL_SAMPLES 200
#define GYRO_REST_DEADBAND 0.025f
#define GYRO_MOTION_COOLDOWN 150

#define ZPA_WHOAMI 0x0f
#define ZPA_CTRL0 0x20
#define ZPA_CTRL2 0x22
#define ZPA_CTRL3 0x23
#define ZPA_STATUS 0x27
#define ZPA_PRESS_XL 0x28
#define ZPA_TEMP_L 0x2b
#define ZPA_ENABLE 0x02
#define ZPA_TEMP_SCALE 0.00649f
#define ZPA_TEMP_OFFSET 176.83f
#define ZPA_ONE_SHOT 0x01
#define ZPA_SWRESET 0x04
#define ZPA_ODR_23HZ 0x70
#define ZPA_WHOAMI_VAL 0xb9
#define ZPA_HPA_DIV 6400.0f

#define APDS_CMD 0x80
#define APDS_AUTO 0xa0
#define APDS_ENABLE 0x00
#define APDS_ATIME 0x01
#define APDS_PTIME 0x02
#define APDS_WTIME 0x03
#define APDS_PPULSE 0x0e
#define APDS_CONTROL 0x0f
#define APDS_ID 0x12
#define APDS_CDATAL 0x14
#define APDS_PDATAL 0x18
#define APDS_ENABLE_PON 0x01
#define APDS_ENABLE_AEN 0x02
#define APDS_ENABLE_PEN 0x04
#define APDS_ID_VAL 0x39
#define APDS_PROX_THRESH_NEAR 160
#define APDS_PROX_THRESH_FAR  100

static const struct sensor_t k_list[] = {
    {
        .name = "AK09912 Magnetometer",
        .vendor = "Asahi Kasei",
        .version = 1,
        .handle = HANDLE_MAG,
        .type = SENSOR_TYPE_MAGNETIC_FIELD,
        .maxRange = 4900.0f,
        .resolution = 0.15f,
        .power = 1.1f,
        .minDelay = 10000,
        .fifoReservedEventCount = 0,
        .fifoMaxEventCount = 0,
        .stringType = SENSOR_STRING_TYPE_MAGNETIC_FIELD,
        .requiredPermission = "",
        .maxDelay = 200000,
        .flags = SENSOR_FLAG_CONTINUOUS_MODE,
    },
    {
        .name = "QPDS-T900 Proximity",
        .vendor = "Avago",
        .version = 1,
        .handle = HANDLE_PROX,
        .type = SENSOR_TYPE_PROXIMITY,
        .maxRange = 5.0f,
        .resolution = 5.0f,
        .power = 0.2f,
        .minDelay = 0,
        .fifoReservedEventCount = 0,
        .fifoMaxEventCount = 0,
        .stringType = SENSOR_STRING_TYPE_PROXIMITY,
        .requiredPermission = "",
        .maxDelay = 0,
        .flags = SENSOR_FLAG_ON_CHANGE_MODE | SENSOR_FLAG_WAKE_UP,
    },
    {
        .name = "QPDS-T900 Light",
        .vendor = "Avago",
        .version = 1,
        .handle = HANDLE_LIGHT,
        .type = SENSOR_TYPE_LIGHT,
        .maxRange = 60000.0f,
        .resolution = 1.0f,
        .power = 0.2f,
        .minDelay = 0,
        .fifoReservedEventCount = 0,
        .fifoMaxEventCount = 0,
        .stringType = SENSOR_STRING_TYPE_LIGHT,
        .requiredPermission = "",
        .maxDelay = 0,
        .flags = SENSOR_FLAG_ON_CHANGE_MODE,
    },
    {
        .name = "ZPA2326 Pressure",
        .vendor = "Murata",
        .version = 1,
        .handle = HANDLE_PRESS,
        .type = SENSOR_TYPE_PRESSURE,
        .maxRange = 1100.0f,
        .resolution = 0.0015625f,
        .power = 0.1f,
        .minDelay = 200000,
        .fifoReservedEventCount = 0,
        .fifoMaxEventCount = 0,
        .stringType = SENSOR_STRING_TYPE_PRESSURE,
        .requiredPermission = "",
        .maxDelay = 1000000,
        .flags = SENSOR_FLAG_CONTINUOUS_MODE,
    },
    {
        .name = "Hall front / lid",
        .vendor = "GPIO",
        .version = 1,
        .handle = HANDLE_HALL_FRONT,
        .type = SENSOR_TYPE_ORIENTATION,
        .maxRange = 360.0f,
        .resolution = 180.0f,
        .power = 0.001f,
        .minDelay = 0,
        .fifoReservedEventCount = 0,
        .fifoMaxEventCount = 0,
        .stringType = SENSOR_STRING_TYPE_ORIENTATION,
        .requiredPermission = "",
        .maxDelay = 0,
        .flags = SENSOR_FLAG_ON_CHANGE_MODE,
    },
    {
        .name = "Hall back cover",
        .vendor = "GPIO",
        .version = 1,
        .handle = HANDLE_HALL_BACK,
        .type = SENSOR_TYPE_ORIENTATION,
        .maxRange = 360.0f,
        .resolution = 180.0f,
        .power = 0.001f,
        .minDelay = 0,
        .fifoReservedEventCount = 0,
        .fifoMaxEventCount = 0,
        .stringType = SENSOR_STRING_TYPE_ORIENTATION,
        .requiredPermission = "",
        .maxDelay = 0,
        .flags = SENSOR_FLAG_ON_CHANGE_MODE,
    },
    {
        .name = "ZPA2326 Temperature",
        .vendor = "Murata",
        .version = 1,
        .handle = HANDLE_TEMP,
        .type = SENSOR_TYPE_AMBIENT_TEMPERATURE,
        .maxRange = 85.0f,
        .resolution = 0.00649f,
        .power = 0.1f,
        .minDelay = 200000,
        .fifoReservedEventCount = 0,
        .fifoMaxEventCount = 0,
        .stringType = SENSOR_STRING_TYPE_AMBIENT_TEMPERATURE,
        .requiredPermission = "",
        .maxDelay = 1000000,
        .flags = SENSOR_FLAG_CONTINUOUS_MODE,
    },
    {
        .name = "ICM-20648 Accelerometer",
        .vendor = "InvenSense",
        .version = 1,
        .handle = HANDLE_ACCEL,
        .type = SENSOR_TYPE_ACCELEROMETER,
        .maxRange = 78.4532f,
        .resolution = 0.002395f,
        .power = 0.25f,
        .minDelay = 10000,
        .fifoReservedEventCount = 0,
        .fifoMaxEventCount = 0,
        .stringType = SENSOR_STRING_TYPE_ACCELEROMETER,
        .requiredPermission = "",
        .maxDelay = 200000,
        .flags = SENSOR_FLAG_CONTINUOUS_MODE,
    },
    {
        .name = "ICM-20648 Gyroscope",
        .vendor = "InvenSense",
        .version = 1,
        .handle = HANDLE_GYRO,
        .type = SENSOR_TYPE_GYROSCOPE,
        .maxRange = 34.9065f,
        .resolution = 0.001068f,
        .power = 0.61f,
        .minDelay = 10000,
        .fifoReservedEventCount = 0,
        .fifoMaxEventCount = 0,
        .stringType = SENSOR_STRING_TYPE_GYROSCOPE,
        .requiredPermission = "",
        .maxDelay = 200000,
        .flags = SENSOR_FLAG_CONTINUOUS_MODE,
    },
    {
        .name = "Device Orientation Sensor",
        .vendor = "Cityman Virtual",
        .version = 1,
        .handle = HANDLE_ORIENTATION,
        .type = SENSOR_TYPE_DEVICE_ORIENTATION,
        .maxRange = 3.0f,
        .resolution = 1.0f,
        .power = 0.0f,
        .minDelay = 0,
        .fifoReservedEventCount = 0,
        .fifoMaxEventCount = 0,
        .stringType = SENSOR_STRING_TYPE_DEVICE_ORIENTATION,
        .requiredPermission = "",
        .maxDelay = 0,
        .flags = SENSOR_FLAG_ON_CHANGE_MODE,
    }};

static const struct sensor_t k_accel = {
    .name = "ICM-206xx Accelerometer",
    .vendor = "TDK InvenSense",
    .version = 1,
    .handle = HANDLE_ACCEL,
    .type = SENSOR_TYPE_ACCELEROMETER,
    .maxRange = 39.2266f,
    .resolution = 0.0012f,
    .power = 0.5f,
    .minDelay = 20000,
    .fifoReservedEventCount = 0,
    .fifoMaxEventCount = 0,
    .stringType = SENSOR_STRING_TYPE_ACCELEROMETER,
    .requiredPermission = "",
    .maxDelay = 200000,
    .flags = SENSOR_FLAG_CONTINUOUS_MODE,
};

static const struct sensor_t k_gyro = {
    .name = "ICM-206xx Gyroscope",
    .vendor = "TDK InvenSense",
    .version = 1,
    .handle = HANDLE_GYRO,
    .type = SENSOR_TYPE_GYROSCOPE,
    .maxRange = 34.9066f,
    .resolution = 0.001f,
    .power = 0.5f,
    .minDelay = 20000,
    .fifoReservedEventCount = 0,
    .fifoMaxEventCount = 0,
    .stringType = SENSOR_STRING_TYPE_GYROSCOPE,
    .requiredPermission = "",
    .maxDelay = 200000,
    .flags = SENSOR_FLAG_CONTINUOUS_MODE,
};

static int g_i2c4 = -1;
static int g_i2c7 = -1;
static int g_enabled;
static int64_t g_period_ns[N_SENSORS + 1];
static int64_t g_next_ns[N_SENSORS + 1];
static int g_have_prox;
static int g_have_light;
static int g_have_hall_front;
static int g_have_hall_back;
static float g_last_prox;
static float g_last_light;
static float g_last_hall_front;
static float g_last_hall_back;
static int g_zpa_busy;
static int g_zpa_saw_busy;
static int64_t g_zpa_start_ns;
static int g_imu_kind;
static int g_imu_probed;
static float g_ak_scale[3] = {1.0f, 1.0f, 1.0f};
static float g_mag_off[3];
static float g_mag_S[9] = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f,
                           0.0f, 0.0f, 0.0f, 1.0f};
static int g_mag_calibrated;
static float g_fit_m[MAG_FIT_N][3];
static int g_fit_n;
static int g_fit_i;
static float g_mag_fit_r;
static float g_mag_fit_rms = 99.0f;
static int g_last_mag_status = -1;
static int g_mag_still_n;
static int g_gyro_cool;
static float g_last_mag[3];
static int g_have_last_mag;
static float g_gyro_bias[3];
static int g_gyro_calibrated;
static float g_last_accel[3];
static int g_have_accel;
static float g_last_gyro_raw[3];
static int g_have_gyro;
static float g_still_sum[3];
static int g_still_n;
static int g_cal_loaded;
static int g_cal_dirty;
static int64_t g_last_save_ns;
static struct sensor_t
    g_list[24]; // Déclaré comme un vrai tableau de 24 éléments
static int g_list_n;

static sensors_event_t g_q[QMAX];
static int g_qhead;
static int g_qcount;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv;
static int g_cv_ready;
static int g_opened;

static int open_buses(void);

static int64_t clock_ns(clockid_t id) {
  struct timespec ts;
  clock_gettime(id, &ts);
  return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static int64_t now_ns(void) { return clock_ns(CLOCK_BOOTTIME); }

static int64_t mono_ns(void) { return clock_ns(CLOCK_MONOTONIC); }

static float vlen3(float x, float y, float z) {
  return sqrtf(x * x + y * y + z * z);
}

static float mag_dip_abs(float mx, float my, float mz) {
  float an, mn, d;
  if (!g_have_accel)
    return -1.0f;
  an = vlen3(g_last_accel[0], g_last_accel[1], g_last_accel[2]);
  mn = vlen3(mx, my, mz);
  if (an < 8.5f || an > 11.0f || mn < 1.0f)
    return -1.0f;
  d = mx * g_last_accel[0] + my * g_last_accel[1] + mz * g_last_accel[2];
  return fabsf(d / (mn * an));
}

static int mag_status_from_field(float mx, float my, float mz) {
  float n = vlen3(mx, my, mz);
  int st;
  /* AOSP SensorEvent.accuracy for TYPE_MAGNETIC_FIELD is field
   * quality, not a motion flag. Compass UIs treat MEDIUM as red.
   */
  if (!g_mag_calibrated)
    st = SENSOR_STATUS_ACCURACY_LOW;
  else if (n >= MAG_EARTH_LOW && n <= MAG_EARTH_HIGH)
    st = SENSOR_STATUS_ACCURACY_HIGH;
  else if (n >= MAG_RADIUS_MIN && n <= MAG_RADIUS_MAX)
    st = SENSOR_STATUS_ACCURACY_MEDIUM;
  else
    st = SENSOR_STATUS_ACCURACY_LOW;
  if (st != g_last_mag_status) {
    ALOGI("mag status %d |B|=%.1f dip=%.2f cal=%d r=%.1f rms=%.1f", st, n,
          mag_dip_abs(mx, my, mz), g_mag_calibrated, g_mag_fit_r,
          g_mag_fit_rms);
    g_last_mag_status = st;
  }
  return st;
}

static void cal_save(void) {
  /* Caller holds g_lock. Do not fopen here: SensorService poll
   * thread would block I2C and can SIGBUS if the mapping is busy.
   */
  g_cal_dirty = 1;
}

static void cal_flush(void) {
  FILE *fp;
  float mag_off[3], mag_S[9], gyro_bias[3];
  int mag_c, gyro_c;
  int64_t now;

  pthread_mutex_lock(&g_lock);
  now = now_ns();
  if (!g_cal_dirty || (g_last_save_ns && now - g_last_save_ns < 2000000000LL)) {
    pthread_mutex_unlock(&g_lock);
    return;
  }
  g_cal_dirty = 0;
  g_last_save_ns = now;
  mag_off[0] = g_mag_off[0];
  mag_off[1] = g_mag_off[1];
  mag_off[2] = g_mag_off[2];
  memcpy(mag_S, g_mag_S, sizeof(mag_S));
  mag_c = g_mag_calibrated;
  gyro_bias[0] = g_gyro_bias[0];
  gyro_bias[1] = g_gyro_bias[1];
  gyro_bias[2] = g_gyro_bias[2];
  gyro_c = g_gyro_calibrated;
  pthread_mutex_unlock(&g_lock);

  fp = fopen(CAL_PATH, "w");
  if (!fp)
    fp = fopen(CAL_PATH_ALT, "w");
  if (!fp)
    return;
  fprintf(fp, "v4\n");
  fprintf(fp, "mag %.4f %.4f %.4f %d\n", mag_off[0], mag_off[1], mag_off[2],
          mag_c);
  fprintf(fp, "magS %.5f %.5f %.5f %.5f %.5f %.5f %.5f %.5f %.5f\n", mag_S[0],
          mag_S[1], mag_S[2], mag_S[3], mag_S[4], mag_S[5], mag_S[6], mag_S[7],
          mag_S[8]);
  fprintf(fp, "gyro %.6f %.6f %.6f %d\n", gyro_bias[0], gyro_bias[1],
          gyro_bias[2], gyro_c);
  fclose(fp);
}

static void mag_S_identity(void) {
  memset(g_mag_S, 0, sizeof(g_mag_S));
  g_mag_S[0] = g_mag_S[4] = g_mag_S[8] = 1.0f;
}

static void mag_S_diag(float sx, float sy, float sz) {
  mag_S_identity();
  g_mag_S[0] = sx;
  g_mag_S[4] = sy;
  g_mag_S[8] = sz;
}

static void cal_load(void) {
  FILE *fp;
  char line[192];
  float ox, oy, oz, bx, by, bz, sx, sy, sz;
  float S[9];
  int mq, gq;
  int ver = 0;
  if (g_cal_loaded)
    return;
  g_cal_loaded = 1;
  mag_S_identity();
  fp = fopen(CAL_PATH, "r");
  if (!fp)
    fp = fopen(CAL_PATH_ALT, "r");
  if (!fp)
    return;
  if (!fgets(line, sizeof(line), fp)) {
    fclose(fp);
    return;
  }
  /* v1 AABB discard mag. v2 sphere. v3 diagonal scale. v4 3x3 soft-iron. */
  if (strncmp(line, "v2", 2) == 0)
    ver = 2;
  else if (strncmp(line, "v3", 2) == 0)
    ver = 3;
  else if (strncmp(line, "v4", 2) == 0)
    ver = 4;
  while (fgets(line, sizeof(line), fp)) {
    if (ver >= 2 && sscanf(line, "mag %f %f %f %d", &ox, &oy, &oz, &mq) == 4) {
      if (vlen3(ox, oy, oz) < 2000.0f) {
        g_mag_off[0] = ox;
        g_mag_off[1] = oy;
        g_mag_off[2] = oz;
        g_mag_calibrated = mq ? 1 : 0;
      }
    } else if (ver == 3 &&
               sscanf(line, "magscale %f %f %f", &sx, &sy, &sz) == 3) {
      if (sx > 0.5f && sx < 2.0f && sy > 0.5f && sy < 2.0f && sz > 0.5f &&
          sz < 2.0f)
        mag_S_diag(sx, sy, sz);
    } else if (ver >= 4 &&
               sscanf(line, "magS %f %f %f %f %f %f %f %f %f", &S[0], &S[1],
                      &S[2], &S[3], &S[4], &S[5], &S[6], &S[7], &S[8]) == 9) {
      if (fabsf(S[0]) > 0.3f && fabsf(S[0]) < 3.0f && fabsf(S[4]) > 0.3f &&
          fabsf(S[4]) < 3.0f && fabsf(S[8]) > 0.3f && fabsf(S[8]) < 3.0f)
        memcpy(g_mag_S, S, sizeof(g_mag_S));
    } else if (sscanf(line, "gyro %f %f %f %d", &bx, &by, &bz, &gq) == 4) {
      if (vlen3(bx, by, bz) < GYRO_BIAS_MAX) {
        g_gyro_bias[0] = bx;
        g_gyro_bias[1] = by;
        g_gyro_bias[2] = bz;
        g_gyro_calibrated = gq ? 1 : 0;
      }
    }
  }
  fclose(fp);
  if (g_mag_calibrated)
    g_mag_fit_rms = 10.0f;
  ALOGI("cal load mag=%d off=%.1f %.1f %.1f S=%.2f %.2f %.2f / %.2f %.2f %.2f "
        "/ %.2f %.2f %.2f gyro=%d v=%d",
        g_mag_calibrated, g_mag_off[0], g_mag_off[1], g_mag_off[2], g_mag_S[0],
        g_mag_S[1], g_mag_S[2], g_mag_S[3], g_mag_S[4], g_mag_S[5], g_mag_S[6],
        g_mag_S[7], g_mag_S[8], g_gyro_calibrated, ver);
}

static int solve4(double A[4][4], double b[4], double x[4]) {
  int i, j, k, p;
  double t, maxv;
  for (i = 0; i < 4; i++) {
    p = i;
    maxv = fabs(A[i][i]);
    for (k = i + 1; k < 4; k++) {
      t = fabs(A[k][i]);
      if (t > maxv) {
        maxv = t;
        p = k;
      }
    }
    if (maxv < 1e-9)
      return -1;
    if (p != i) {
      for (j = 0; j < 4; j++) {
        t = A[i][j];
        A[i][j] = A[p][j];
        A[p][j] = t;
      }
      t = b[i];
      b[i] = b[p];
      b[p] = t;
    }
    t = A[i][i];
    for (j = i; j < 4; j++)
      A[i][j] /= t;
    b[i] /= t;
    for (k = 0; k < 4; k++) {
      if (k == i)
        continue;
      t = A[k][i];
      for (j = i; j < 4; j++)
        A[k][j] -= t * A[i][j];
      b[k] -= t * b[i];
    }
  }
  for (i = 0; i < 4; i++)
    x[i] = b[i];
  return 0;
}

static int mag_sphere_fit(float *cx, float *cy, float *cz, float *R,
                          float *rms_out) {
  double A[4][4];
  double b[4], x[4], row[4];
  double mx, my, mz, y, r2, rms, d, rr;
  int i, j, k, n;
  n = g_fit_n;
  if (n < MAG_FIT_MIN)
    return -1;
  memset(A, 0, sizeof(A));
  memset(b, 0, sizeof(b));
  for (i = 0; i < n; i++) {
    mx = g_fit_m[i][0];
    my = g_fit_m[i][1];
    mz = g_fit_m[i][2];
    row[0] = 2.0 * mx;
    row[1] = 2.0 * my;
    row[2] = 2.0 * mz;
    row[3] = -1.0;
    y = mx * mx + my * my + mz * mz;
    for (j = 0; j < 4; j++) {
      for (k = 0; k < 4; k++)
        A[j][k] += row[j] * row[k];
      b[j] += row[j] * y;
    }
  }
  if (solve4(A, b, x) != 0)
    return -1;
  r2 = x[0] * x[0] + x[1] * x[1] + x[2] * x[2] - x[3];
  if (r2 < (double)(MAG_RADIUS_MIN * MAG_RADIUS_MIN) ||
      r2 > (double)(MAG_RADIUS_MAX * MAG_RADIUS_MAX))
    return -1;
  rr = sqrt(r2);
  rms = 0.0;
  for (i = 0; i < n; i++) {
    mx = g_fit_m[i][0] - x[0];
    my = g_fit_m[i][1] - x[1];
    mz = g_fit_m[i][2] - x[2];
    d = sqrt(mx * mx + my * my + mz * mz) - rr;
    rms += d * d;
  }
  rms = sqrt(rms / (double)n);
  if (rms > (double)MAG_FIT_RMS_MAX)
    return -1;
  *cx = (float)x[0];
  *cy = (float)x[1];
  *cz = (float)x[2];
  *R = (float)rr;
  *rms_out = (float)rms;
  return 0;
}

static int mag_buffer_span(float *sx, float *sy, float *sz) {
  float mn[3], mx[3];
  int i, j;
  if (g_fit_n < MAG_FIT_MIN)
    return -1;
  mn[0] = mx[0] = g_fit_m[0][0];
  mn[1] = mx[1] = g_fit_m[0][1];
  mn[2] = mx[2] = g_fit_m[0][2];
  for (i = 1; i < g_fit_n; i++) {
    for (j = 0; j < 3; j++) {
      if (g_fit_m[i][j] < mn[j])
        mn[j] = g_fit_m[i][j];
      if (g_fit_m[i][j] > mx[j])
        mx[j] = g_fit_m[i][j];
    }
  }
  *sx = mx[0] - mn[0];
  *sy = mx[1] - mn[1];
  *sz = mx[2] - mn[2];
  return 0;
}

static int solve_n(int n, double *A, double *b, double *x) {
  int i, j, k, p;
  double t, maxv;
  for (i = 0; i < n; i++) {
    p = i;
    maxv = fabs(A[i * n + i]);
    for (k = i + 1; k < n; k++) {
      t = fabs(A[k * n + i]);
      if (t > maxv) {
        maxv = t;
        p = k;
      }
    }
    if (maxv < 1e-12)
      return -1;
    if (p != i) {
      for (j = 0; j < n; j++) {
        t = A[i * n + j];
        A[i * n + j] = A[p * n + j];
        A[p * n + j] = t;
      }
      t = b[i];
      b[i] = b[p];
      b[p] = t;
    }
    t = A[i * n + i];
    for (j = i; j < n; j++)
      A[i * n + j] /= t;
    b[i] /= t;
    for (k = 0; k < n; k++) {
      if (k == i)
        continue;
      t = A[k * n + i];
      for (j = i; j < n; j++)
        A[k * n + j] -= t * A[i * n + j];
      b[k] -= t * b[i];
    }
  }
  for (i = 0; i < n; i++)
    x[i] = b[i];
  return 0;
}

static int chol3(const double A[3][3], double L[3][3]) {
  double d;
  if (A[0][0] <= 1e-12)
    return -1;
  L[0][0] = sqrt(A[0][0]);
  L[0][1] = L[0][2] = 0.0;
  L[1][0] = A[1][0] / L[0][0];
  L[2][0] = A[2][0] / L[0][0];
  d = A[1][1] - L[1][0] * L[1][0];
  if (d <= 1e-12)
    return -1;
  L[1][1] = sqrt(d);
  L[1][2] = 0.0;
  L[2][1] = (A[2][1] - L[2][0] * L[1][0]) / L[1][1];
  d = A[2][2] - L[2][0] * L[2][0] - L[2][1] * L[2][1];
  if (d <= 1e-12)
    return -1;
  L[2][2] = sqrt(d);
  return 0;
}

static void mag_apply_S(float dx, float dy, float dz, const float *S, float *ox,
                        float *oy, float *oz) {
  *ox = S[0] * dx + S[1] * dy + S[2] * dz;
  *oy = S[3] * dx + S[4] * dy + S[5] * dz;
  *oz = S[6] * dx + S[7] * dy + S[8] * dz;
}

static float mag_S_rms(float cx, float cy, float cz, const float *S, float R) {
  double rms = 0.0, d;
  float ox, oy, oz;
  int i;
  if (g_fit_n <= 0 || R < 1.0f)
    return 99.0f;
  for (i = 0; i < g_fit_n; i++) {
    mag_apply_S(g_fit_m[i][0] - cx, g_fit_m[i][1] - cy, g_fit_m[i][2] - cz, S,
                &ox, &oy, &oz);
    d = sqrt((double)ox * ox + (double)oy * oy + (double)oz * oz) - (double)R;
    rms += d * d;
  }
  return (float)sqrt(rms / (double)g_fit_n);
}

/* PX4-style symmetric soft-iron: (m-c)^T A (m-c) = R^2, S = L^T from
 * Cholesky A = L L^T. Maps the speaker ellipsoid onto a sphere.
 */
static int mag_ellipsoid_scale(float cx, float cy, float cz, float R,
                               float S[9]) {
  double AtA[36], Atb[6], p[6], row[6];
  double A[3][3], L[3][3], vx, vy, vz, y;
  int i, j, k;
  if (g_fit_n < MAG_FIT_MIN || R < MAG_RADIUS_MIN)
    return -1;
  memset(AtA, 0, sizeof(AtA));
  memset(Atb, 0, sizeof(Atb));
  y = (double)R * (double)R;
  for (i = 0; i < g_fit_n; i++) {
    vx = g_fit_m[i][0] - cx;
    vy = g_fit_m[i][1] - cy;
    vz = g_fit_m[i][2] - cz;
    row[0] = vx * vx;
    row[1] = vy * vy;
    row[2] = vz * vz;
    row[3] = 2.0 * vx * vy;
    row[4] = 2.0 * vx * vz;
    row[5] = 2.0 * vy * vz;
    for (j = 0; j < 6; j++) {
      for (k = 0; k < 6; k++)
        AtA[j * 6 + k] += row[j] * row[k];
      Atb[j] += row[j] * y;
    }
  }
  if (solve_n(6, AtA, Atb, p) != 0)
    return -1;
  A[0][0] = p[0];
  A[1][1] = p[1];
  A[2][2] = p[2];
  A[0][1] = A[1][0] = p[3];
  A[0][2] = A[2][0] = p[4];
  A[1][2] = A[2][1] = p[5];
  if (chol3(A, L) != 0)
    return -1;
  /* S = L^T so |S (m-c)| = R */
  S[0] = (float)L[0][0];
  S[1] = (float)L[1][0];
  S[2] = (float)L[2][0];
  S[3] = (float)L[0][1];
  S[4] = (float)L[1][1];
  S[5] = (float)L[2][1];
  S[6] = (float)L[0][2];
  S[7] = (float)L[1][2];
  S[8] = (float)L[2][2];
  if (fabsf(S[0]) < 0.4f || fabsf(S[0]) > 2.5f || fabsf(S[4]) < 0.4f ||
      fabsf(S[4]) > 2.5f || fabsf(S[8]) < 0.4f || fabsf(S[8]) > 2.5f)
    return -1;
  return 0;
}

static void mag_try_commit(void) {
  float cx, cy, cz, radius, rms, spx, spy, spz, used_rms;
  float S[9];
  int ok, wide;
  if (mag_buffer_span(&spx, &spy, &spz) != 0)
    return;
  ok = (spx > MAG_SPAN_MIN) + (spy > MAG_SPAN_MIN) + (spz > MAG_SPAN_MIN);
  wide = (spx > MAG_SPAN_WIDE) + (spy > MAG_SPAN_WIDE) + (spz > MAG_SPAN_WIDE);
  if (g_mag_calibrated) {
    if (ok != 3)
      return;
  } else if (!(ok == 3 || wide >= 2)) {
    return;
  }
  if (mag_sphere_fit(&cx, &cy, &cz, &radius, &rms) != 0)
    return;
  memset(S, 0, sizeof(S));
  S[0] = S[4] = S[8] = 1.0f;
  if (mag_ellipsoid_scale(cx, cy, cz, radius, S) != 0) {
    S[0] = S[4] = S[8] = 1.0f;
    S[1] = S[2] = S[3] = S[5] = S[6] = S[7] = 0.0f;
  }
  used_rms = mag_S_rms(cx, cy, cz, S, radius);
  if (used_rms > MAG_FIT_RMS_MAX)
    return;
  if (g_mag_calibrated && used_rms > g_mag_fit_rms - 0.3f) {
    ALOGI("mag keep cal rms=%.1f new=%.1f span=%.1f %.1f %.1f", g_mag_fit_rms,
          used_rms, spx, spy, spz);
    return;
  }
  g_mag_off[0] = cx;
  g_mag_off[1] = cy;
  g_mag_off[2] = cz;
  memcpy(g_mag_S, S, sizeof(g_mag_S));
  g_mag_fit_r = radius;
  g_mag_fit_rms = used_rms;
  g_mag_calibrated = 1;
  ALOGI("mag commit off=%.1f %.1f %.1f S=%.3f %.3f %.3f / %.3f %.3f %.3f / "
        "%.3f %.3f %.3f r=%.1f rms=%.1f n=%d",
        cx, cy, cz, S[0], S[1], S[2], S[3], S[4], S[5], S[6], S[7], S[8],
        radius, used_rms, g_fit_n);
  g_last_save_ns = 0;
  cal_save();
}

static void mag_update_cal(float x, float y, float z, int moving) {
  static int logged_n;
  if (moving) {
    g_fit_m[g_fit_i][0] = x;
    g_fit_m[g_fit_i][1] = y;
    g_fit_m[g_fit_i][2] = z;
    g_fit_i = (g_fit_i + 1) % MAG_FIT_N;
    if (g_fit_n < MAG_FIT_N)
      g_fit_n++;
    g_mag_still_n = 0;
    if (g_fit_n != logged_n && (g_fit_n % 50) == 0) {
      ALOGI("mag cal samples=%d", g_fit_n);
      logged_n = g_fit_n;
    }
    return;
  }
  if (g_fit_n < MAG_FIT_MIN)
    return;
  g_mag_still_n++;
  if (g_mag_still_n == MAG_STILL_COMMIT)
    mag_try_commit();
}

static void gyro_update_bias(float gx, float gy, float gz) {
  float an, gn, nb[3];
  int i;
  if (!g_have_accel)
    return;
  an = vlen3(g_last_accel[0], g_last_accel[1], g_last_accel[2]);
  gn = vlen3(gx, gy, gz);
  if (an < 8.5f || an > 11.0f || gn > 0.50f) {
    g_still_n = 0;
    g_still_sum[0] = g_still_sum[1] = g_still_sum[2] = 0;
    g_gyro_cool = GYRO_MOTION_COOLDOWN;
    return;
  }
  if (g_gyro_cool > 0) {
    g_gyro_cool--;
    g_still_n = 0;
    g_still_sum[0] = g_still_sum[1] = g_still_sum[2] = 0;
    return;
  }
  if (gn > GYRO_STILL_MAX)
    return;
  g_still_sum[0] += gx;
  g_still_sum[1] += gy;
  g_still_sum[2] += gz;
  g_still_n++;
  if (g_still_n < GYRO_STILL_SAMPLES)
    return;
  for (i = 0; i < 3; i++)
    nb[i] = g_still_sum[i] / (float)g_still_n;
  g_still_n = 0;
  g_still_sum[0] = g_still_sum[1] = g_still_sum[2] = 0;
  if (vlen3(nb[0], nb[1], nb[2]) > GYRO_BIAS_MAX)
    return;
  if (g_gyro_calibrated) {
    for (i = 0; i < 3; i++)
      g_gyro_bias[i] = 0.90f * g_gyro_bias[i] + 0.10f * nb[i];
  } else {
    for (i = 0; i < 3; i++)
      g_gyro_bias[i] = nb[i];
    ALOGI("gyro rest bias %.4f %.4f %.4f rad/s", g_gyro_bias[0], g_gyro_bias[1],
          g_gyro_bias[2]);
    g_last_save_ns = 0;
  }
  g_gyro_calibrated = 1;
  cal_save();
}

static int handle_index(int handle) {
  if (handle >= HANDLE_MAG && handle <= HANDLE_TEMP)
    return handle;
  return -1;
}

static int i2c_xfer(int fd, uint8_t addr, uint16_t flags, uint8_t *buf,
                    uint16_t len) {
  struct i2c_msg msg = {
      .addr = addr,
      .flags = flags,
      .len = len,
      .buf = buf,
  };
  struct i2c_rdwr_ioctl_data xfer = {
      .msgs = &msg,
      .nmsgs = 1,
  };
  if (ioctl(fd, I2C_RDWR, &xfer) < 0)
    return -errno;
  return 0;
}

static int i2c_write_reg(int fd, uint8_t addr, uint8_t reg, uint8_t val) {
  uint8_t buf[2] = {reg, val};
  return i2c_xfer(fd, addr, 0, buf, 2);
}

static int i2c_read_regs(int fd, uint8_t addr, uint8_t reg, uint8_t *buf,
                         uint16_t len) {
  struct i2c_msg msgs[2];
  struct i2c_rdwr_ioctl_data xfer;
  msgs[0].addr = addr;
  msgs[0].flags = 0;
  msgs[0].len = 1;
  msgs[0].buf = &reg;
  msgs[1].addr = addr;
  msgs[1].flags = I2C_M_RD;
  msgs[1].len = len;
  msgs[1].buf = buf;
  xfer.msgs = msgs;
  xfer.nmsgs = 2;
  if (ioctl(fd, I2C_RDWR, &xfer) < 0)
    return -errno;
  return 0;
}

static void enqueue_locked(const sensors_event_t *ev) {
  int idx;
  if (g_qcount == QMAX) {
    g_qhead = (g_qhead + 1) % QMAX;
    g_qcount--;
  }
  idx = (g_qhead + g_qcount) % QMAX;
  g_q[idx] = *ev;
  g_qcount++;
  pthread_cond_signal(&g_cv);
}

static void drop_handle_locked(int handle) {
  sensors_event_t tmp[QMAX];
  int n = 0;
  int i;
  for (i = 0; i < g_qcount; i++) {
    sensors_event_t *ev = &g_q[(g_qhead + i) % QMAX];
    int keep = 1;
    if (ev->type == SENSOR_TYPE_META_DATA) {
      if (ev->meta_data.sensor == handle)
        keep = 0;
    } else if (ev->sensor == handle) {
      keep = 0;
    }
    if (keep)
      tmp[n++] = *ev;
  }
  memcpy(g_q, tmp, (size_t)n * sizeof(tmp[0]));
  g_qhead = 0;
  g_qcount = n;
}

static void fill_common(sensors_event_t *ev, int handle, int type) {
  memset(ev, 0, sizeof(*ev));
  ev->version = sizeof(sensors_event_t);
  ev->sensor = handle;
  ev->type = type;
  ev->timestamp = now_ns();
}

static int ak_init(void) {
  uint8_t id[2] = {0, 0};
  uint8_t asa[3] = {128, 128, 128};
  if (i2c_read_regs(g_i2c4, AK_ADDR, AK_WIA1, id, 2) != 0)
    return -1;
  if (id[0] != AK_WIA1_VAL || id[1] != AK_WIA2_VAL) {
    ALOGE("AK09912 WIA %02x %02x (expected 48 04)", id[0], id[1]);
    return -1;
  }
  i2c_write_reg(g_i2c4, AK_ADDR, AK_CNTL3, 0x01);
  usleep(1000);
  i2c_write_reg(g_i2c4, AK_ADDR, AK_CNTL2, AK_CNTL2_POWER_DOWN);
  usleep(100);
  i2c_write_reg(g_i2c4, AK_ADDR, AK_CNTL2, AK_CNTL2_FUSE_ROM);
  usleep(100);
  if (i2c_read_regs(g_i2c4, AK_ADDR, AK_ASAX, asa, 3) == 0) {
    g_ak_scale[0] = (asa[0] + 128.0f) / 256.0f;
    g_ak_scale[1] = (asa[1] + 128.0f) / 256.0f;
    g_ak_scale[2] = (asa[2] + 128.0f) / 256.0f;
    ALOGI("AK09912 WIA ok ASA %u %u %u scale=%.3f %.3f %.3f", asa[0], asa[1],
          asa[2], g_ak_scale[0], g_ak_scale[1], g_ak_scale[2]);
  } else {
    ALOGI("AK09912 WIA ok (ASA unread, scale 1)");
  }
  i2c_write_reg(g_i2c4, AK_ADDR, AK_CNTL2, AK_CNTL2_POWER_DOWN);
  cal_load();
  return 0;
}

static int ak_read(sensors_event_t *ev) {
  uint8_t st1 = 0;
  uint8_t raw[8];
  int16_t x, y, z;
  float mx, my, mz, dmag, gn;
  int moving;
  if (i2c_read_regs(g_i2c4, AK_ADDR, AK_ST1, &st1, 1) != 0)
    return -1;
  if ((st1 & 0x01) == 0)
    return 0;
  if (i2c_read_regs(g_i2c4, AK_ADDR, AK_HXL, raw, 8) != 0)
    return -1;
  if (raw[7] & 0x08)
    return 0;
  x = (int16_t)(raw[0] | (raw[1] << 8));
  y = (int16_t)(raw[2] | (raw[3] << 8));
  z = (int16_t)(raw[4] | (raw[5] << 8));
  mx = x * AK_SCALE_UT * g_ak_scale[0];
  my = y * AK_SCALE_UT * g_ak_scale[1];
  mz = z * AK_SCALE_UT * g_ak_scale[2];
  dmag = 0.0f;
  if (g_have_last_mag)
    dmag = vlen3(mx - g_last_mag[0], my - g_last_mag[1], mz - g_last_mag[2]);
  g_last_mag[0] = mx;
  g_last_mag[1] = my;
  g_last_mag[2] = mz;
  g_have_last_mag = 1;
  gn = g_have_gyro
           ? vlen3(g_last_gyro_raw[0], g_last_gyro_raw[1], g_last_gyro_raw[2])
           : 0.0f;
  moving = (dmag > 8.0f) || (gn > 0.40f);
  mag_update_cal(mx, my, mz, moving);
  if (g_mag_calibrated)
    mag_apply_S(mx - g_mag_off[0], my - g_mag_off[1], mz - g_mag_off[2],
                g_mag_S, &mx, &my, &mz);
  fill_common(ev, HANDLE_MAG, SENSOR_TYPE_MAGNETIC_FIELD);
  ev->magnetic.x = mx;
  ev->magnetic.y = my;
  ev->magnetic.z = mz;
  ev->magnetic.status = mag_status_from_field(mx, my, mz);
  return 1;
}

static int zpa_prepare(void) {
  /* CTRL_REG3 is illegal until ENABLE + Tpup (~1–2 ms). One-shot
   * then drops the chip back to low power, so this must run every
   * sample, not only at open().
   */
  if (i2c_write_reg(g_i2c4, ZPA_ADDR, ZPA_CTRL0, ZPA_ENABLE) != 0)
    return -1;
  usleep(2000);
  if (i2c_write_reg(g_i2c4, ZPA_ADDR, ZPA_CTRL3, ZPA_ODR_23HZ) != 0)
    return -1;
  return 0;
}

static int zpa_init(void) {
  uint8_t who = 0;
  if (i2c_read_regs(g_i2c4, ZPA_ADDR, (uint8_t)(ZPA_WHOAMI | 0x80), &who, 1) !=
      0)
    return -1;
  if (who != ZPA_WHOAMI_VAL)
    ALOGW("ZPA2326 WHOAMI %02x (Linux expects B9)", who);
  else
    ALOGI("ZPA2326 WHOAMI ok");
  /* Reset is only legal after ENABLE. */
  if (i2c_write_reg(g_i2c4, ZPA_ADDR, ZPA_CTRL0, ZPA_ENABLE) != 0)
    return -1;
  usleep(2000);
  i2c_write_reg(g_i2c4, ZPA_ADDR, ZPA_CTRL2, ZPA_SWRESET);
  usleep(2000);
  return zpa_prepare();
}

static int zpa_read_sample(int32_t *press_out, uint16_t *temp_out,
                           uint8_t *ctrl0_out, uint8_t *status_out) {
  uint8_t raw[5] = {0, 0, 0, 0, 0};
  uint8_t ctrl0 = 0;
  uint8_t status = 0;

  if (i2c_read_regs(g_i2c4, ZPA_ADDR, (uint8_t)(ZPA_CTRL0 | 0x80), &ctrl0, 1) !=
      0)
    return -1;
  i2c_read_regs(g_i2c4, ZPA_ADDR, (uint8_t)(ZPA_STATUS | 0x80), &status, 1);
  if (i2c_read_regs(g_i2c4, ZPA_ADDR, (uint8_t)(ZPA_PRESS_XL | 0x80), raw, 3) !=
      0)
    return -1;
  if (i2c_read_regs(g_i2c4, ZPA_ADDR, (uint8_t)(ZPA_TEMP_L | 0x80), raw + 3,
                    2) != 0)
    return -1;
  *press_out = raw[0] | (raw[1] << 8) | (raw[2] << 16);
  *temp_out = (uint16_t)(raw[3] | (raw[4] << 8));
  *ctrl0_out = ctrl0;
  *status_out = status;
  return 0;
}

static float zpa_temp_c(uint16_t raw) {
  return (float)raw * ZPA_TEMP_SCALE - ZPA_TEMP_OFFSET;
}

static int zpa_temp_sane(float c) { return (c > -20.0f && c < 85.0f); }

static void zpa_finish_locked(int64_t t, int32_t press, uint16_t traw) {
  sensors_event_t ev;
  float c;

  if ((g_enabled & (1 << HANDLE_PRESS)) && press != 0) {
    fill_common(&ev, HANDLE_PRESS, SENSOR_TYPE_PRESSURE);
    ev.pressure = press / ZPA_HPA_DIV;
    ev.data[0] = ev.pressure;
    enqueue_locked(&ev);
  }
  if (g_enabled & (1 << HANDLE_TEMP)) {
    c = zpa_temp_c(traw);
    if (zpa_temp_sane(c)) {
      fill_common(&ev, HANDLE_TEMP, SENSOR_TYPE_AMBIENT_TEMPERATURE);
      ev.temperature = c;
      ev.data[0] = c;
      enqueue_locked(&ev);
    }
  }
  g_next_ns[HANDLE_PRESS] = t + g_period_ns[HANDLE_PRESS];
  g_next_ns[HANDLE_TEMP] = t + g_period_ns[HANDLE_TEMP];
  g_zpa_busy = 0;
  g_zpa_saw_busy = 0;
}

/*
 * One-shot takes 40–90 ms. Do not spin here: Android 11 fusion
 * drops gyro samples more than 50 ms apart, which leaves gravity /
 * rotation vector / game RV at quaternion 0.
 */
static void zpa_sample_locked(int64_t t) {
  int want =
      ((g_enabled & (1 << HANDLE_PRESS)) && t >= g_next_ns[HANDLE_PRESS]) ||
      ((g_enabled & (1 << HANDLE_TEMP)) && t >= g_next_ns[HANDLE_TEMP]);
  int32_t press = 0;
  uint16_t traw = 0;
  uint8_t ctrl0 = 0, status = 0;

  if (!g_zpa_busy) {
    if (!want)
      return;
    if (zpa_prepare() != 0)
      return;
    if (i2c_write_reg(g_i2c4, ZPA_ADDR, ZPA_CTRL0, ZPA_ENABLE | ZPA_ONE_SHOT) !=
        0)
      return;
    g_zpa_busy = 1;
    g_zpa_saw_busy = 0;
    g_zpa_start_ns = t;
    return;
  }
  if (zpa_read_sample(&press, &traw, &ctrl0, &status) != 0) {
    g_zpa_busy = 0;
    return;
  }
  if (ctrl0 & ZPA_ONE_SHOT) {
    g_zpa_saw_busy = 1;
    if (t - g_zpa_start_ns > 250000000LL) {
      ALOGW("ZPA2326 oneshot timeout");
      g_zpa_busy = 0;
      g_next_ns[HANDLE_PRESS] = t + g_period_ns[HANDLE_PRESS];
      g_next_ns[HANDLE_TEMP] = t + g_period_ns[HANDLE_TEMP];
    }
    return;
  }
  if (!g_zpa_saw_busy && t - g_zpa_start_ns < 20000000LL)
    return;
  ALOGD("ZPA oneshot raw=%d hPa=%.2f temp=%d C=%.1f CTRL0=%02x STATUS=%02x",
        press, press / ZPA_HPA_DIV, traw, zpa_temp_c(traw), ctrl0, status);
  zpa_finish_locked(t, press, traw);
}

static int hall_gpio_raw(const char *path) {
  char buf[8];
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  ssize_t n;
  if (fd < 0)
    return -1;
  n = read(fd, buf, sizeof(buf) - 1);
  close(fd);
  if (n <= 0)
    return -1;
  buf[n] = 0;
  return (buf[0] == '0') ? 0 : 1;
}

/*
 * Idle (no magnet) is 1 on this phone. Treat 1 as open / 180 deg,
 * 0 as magnet present / closed. Matches a typical hall + pull-up.
 */
static int hall_read(int handle, sensors_event_t *ev) {
  int raw;
  float value;
  int *have;
  float *last;
  const char *path;

  if (handle == HANDLE_HALL_FRONT) {
    path = HALL_FRONT_PATH;
    have = &g_have_hall_front;
    last = &g_last_hall_front;
  } else {
    path = HALL_BACK_PATH;
    have = &g_have_hall_back;
    last = &g_last_hall_back;
  }
  raw = hall_gpio_raw(path);
  if (raw < 0)
    return -1;
  /* Idle is 1. Report azimuth 180 = open / no magnet, 0 = magnet.
   * TYPE_ORIENTATION is HIDL 1.0-safe and CPU Info formats it as
   * degrees. Private types 65537/65538 stay blank or show as °C.
   */
  value = raw ? 180.0f : 0.0f;
  if (*have && value == *last)
    return 0;
  fill_common(ev, handle, SENSOR_TYPE_ORIENTATION);
  ev->orientation.azimuth = value;
  ev->orientation.pitch = 0.0f;
  ev->orientation.roll = 0.0f;
  ev->orientation.status = SENSOR_STATUS_ACCURACY_MEDIUM;
  *last = value;
  *have = 1;
  return 1;
}

static int apds_write(uint8_t reg, uint8_t val) {
  return i2c_write_reg(g_i2c7, APDS_ADDR, (uint8_t)(APDS_CMD | reg), val);
}

static int apds_read(uint8_t reg, uint8_t *buf, uint16_t len) {
  return i2c_read_regs(g_i2c7, APDS_ADDR, (uint8_t)(APDS_AUTO | reg), buf, len);
}

static int apds_init(void) {
  uint8_t id = 0;
  if (i2c_read_regs(g_i2c7, APDS_ADDR, (uint8_t)(APDS_CMD | APDS_ID), &id, 1) !=
      0)
    return -1;
  if (id != APDS_ID_VAL)
    ALOGW("APDS ID %02x (expected 39)", id);
  else
    ALOGI("APDS-9930 ID ok");
  apds_write(APDS_ENABLE, 0x00);
  apds_write(APDS_ATIME, 0xdb);
  apds_write(APDS_PTIME, 0xff);
  apds_write(APDS_WTIME, 0xff);
  apds_write(APDS_PPULSE, 0x08);
  apds_write(APDS_CONTROL, 0x20);
  return 0;
}

static float apds_lux(uint16_t ch0, uint16_t ch1) {
  float iac1;
  float iac2;
  float iac;
  if (ch0 == 0)
    return 0.0f;
  iac1 = (float)ch0 - 1.862f * (float)ch1;
  iac2 = 0.630f * (float)ch0 - (float)ch1;
  iac = iac1 > iac2 ? iac1 : iac2;
  if (iac < 0)
    return 0.0f;
  return iac * 0.52f;
}

static int apds_read_both(int *got_prox, sensors_event_t *prox, int *got_light,
                          sensors_event_t *light) {
  uint8_t raw[6];
  uint16_t ch0, ch1, pdata;
  float distance;
  float lux;
  *got_prox = 0;
  *got_light = 0;
  if (apds_read(APDS_CDATAL, raw, 6) != 0)
    return -1;
  ch0 = (uint16_t)(raw[0] | (raw[1] << 8));
  ch1 = (uint16_t)(raw[2] | (raw[3] << 8));
  pdata = (uint16_t)(raw[4] | (raw[5] << 8));
  if (!g_have_prox) {
    distance = (pdata >= APDS_PROX_THRESH_NEAR) ? 0.0f : 5.0f;
  } else if (g_last_prox > 2.5f) {
    /* Currently FAR: require high threshold to switch to NEAR */
    distance = (pdata >= APDS_PROX_THRESH_NEAR) ? 0.0f : 5.0f;
  } else {
    /* Currently NEAR: require dropping below low threshold to switch to FAR */
    distance = (pdata < APDS_PROX_THRESH_FAR) ? 5.0f : 0.0f;
  }
  lux = apds_lux(ch0, ch1);
  if ((g_enabled & (1 << HANDLE_PROX)) &&
      (!g_have_prox || distance != g_last_prox)) {
    fill_common(prox, HANDLE_PROX, SENSOR_TYPE_PROXIMITY);
    prox->distance = distance;
    g_last_prox = distance;
    g_have_prox = 1;
    *got_prox = 1;
    ALOGI("prox event: pdata=%u distance=%.1f", pdata, distance);
  }
  if ((g_enabled & (1 << HANDLE_LIGHT)) &&
      (!g_have_light || fabsf(lux - g_last_light) >= 1.0f)) {
    fill_common(light, HANDLE_LIGHT, SENSOR_TYPE_LIGHT);
    light->light = lux;
    g_last_light = lux;
    g_have_light = 1;
    *got_light = 1;
  }
  return 0;
}

static int accel_looks_like_gravity(float ax, float ay, float az) {
  float m = sqrtf(ax * ax + ay * ay + az * az);
  return (m > 6.0f && m < 14.0f);
}

static int16_t le16(const uint8_t *p) { return (int16_t)(p[0] | (p[1] << 8)); }

static int16_t be16(const uint8_t *p) { return (int16_t)((p[0] << 8) | p[1]); }

static int imu_icm_bank(uint8_t bank) {
  return i2c_write_reg(g_i2c4, IMU_ADDR, ICM_REG_BANK_SEL, bank) == 0;
}

static int imu_icm_wake(void) {
  if (!imu_icm_bank(ICM_BANK0))
    return 0;
  if (i2c_write_reg(g_i2c4, IMU_ADDR, ICM_REG_PWR_MGMT_1, 0x01) != 0)
    return 0;
  usleep(5000);
  i2c_write_reg(g_i2c4, IMU_ADDR, ICM_REG_PWR_MGMT_2, 0x00);
  i2c_write_reg(g_i2c4, IMU_ADDR, ICM_REG_LP_CONFIG, 0x00);
  if (!imu_icm_bank(ICM_BANK2))
    return 0;
  i2c_write_reg(g_i2c4, IMU_ADDR, ICM_REG_GYRO_CONFIG_1, 0x01);
  i2c_write_reg(g_i2c4, IMU_ADDR, ICM_REG_ACCEL_CONFIG, 0x01);
  return imu_icm_bank(ICM_BANK0);
}

static int imu_try_icm(float *ax, float *ay, float *az) {
  uint8_t who = 0, pwr1 = 0, raw[6];

  if (!imu_icm_bank(ICM_BANK0))
    return 0;
  if (i2c_read_regs(g_i2c4, IMU_ADDR, ICM_REG_WHO, &who, 1) != 0)
    return 0;
  i2c_read_regs(g_i2c4, IMU_ADDR, ICM_REG_PWR_MGMT_1, &pwr1, 1);
  if (who != ICM_WHO_CITYMAN && pwr1 != 0x41)
    return 0;
  if (!imu_icm_wake())
    return 0;
  usleep(50000);
  if (i2c_read_regs(g_i2c4, IMU_ADDR, ICM_REG_ACCEL_XOUT_H, raw, 6) != 0)
    return 0;
  *ax = be16(raw) * (9.80665f / 16384.0f);
  *ay = be16(raw + 2) * (9.80665f / 16384.0f);
  *az = be16(raw + 4) * (9.80665f / 16384.0f);
  return accel_looks_like_gravity(*ax, *ay, *az);
}

static int imu_try_lsm6(float *ax, float *ay, float *az) {
  uint8_t raw[6];
  /* CTRL1_XL / CTRL2_G: 104 Hz, ±2 g / ±250 dps. CTRL3_C: BDU+IF_INC. */
  if (i2c_write_reg(g_i2c4, IMU_ADDR, 0x10, 0x40) != 0)
    return 0;
  i2c_write_reg(g_i2c4, IMU_ADDR, 0x11, 0x40);
  i2c_write_reg(g_i2c4, IMU_ADDR, 0x12, 0x44);
  usleep(50000);
  if (i2c_read_regs(g_i2c4, IMU_ADDR, 0x28, raw, 6) != 0)
    return 0;
  *ax = le16(raw) * (9.80665f / 16384.0f);
  *ay = le16(raw + 2) * (9.80665f / 16384.0f);
  *az = le16(raw + 4) * (9.80665f / 16384.0f);
  return accel_looks_like_gravity(*ax, *ay, *az);
}

static int imu_try_bmi(float *ax, float *ay, float *az) {
  uint8_t raw[6];
  /* CMD: accel normal, gyro normal. Default accel range is ±8 g. */
  if (i2c_write_reg(g_i2c4, IMU_ADDR, 0x7e, 0x11) != 0)
    return 0;
  usleep(50000);
  i2c_write_reg(g_i2c4, IMU_ADDR, 0x7e, 0x15);
  usleep(80000);
  if (i2c_read_regs(g_i2c4, IMU_ADDR, 0x12, raw, 6) != 0)
    return 0;
  *ax = le16(raw) * (9.80665f / 4096.0f);
  *ay = le16(raw + 2) * (9.80665f / 4096.0f);
  *az = le16(raw + 4) * (9.80665f / 4096.0f);
  return accel_looks_like_gravity(*ax, *ay, *az);
}

static int imu_try_mpu(float *ax, float *ay, float *az) {
  uint8_t raw[6];
  if (i2c_write_reg(g_i2c4, IMU_ADDR, 0x6b, 0x00) != 0)
    return 0;
  usleep(100000);
  if (i2c_read_regs(g_i2c4, IMU_ADDR, 0x3b, raw, 6) != 0)
    return 0;
  *ax = be16(raw) * (9.80665f / 16384.0f);
  *ay = be16(raw + 2) * (9.80665f / 16384.0f);
  *az = be16(raw + 4) * (9.80665f / 16384.0f);
  return accel_looks_like_gravity(*ax, *ay, *az);
}

static int imu_probe(void) {
  float ax = 0, ay = 0, az = 0;
  uint8_t r0 = 0, r0f = 0, r75 = 0, r6b = 0;

  i2c_read_regs(g_i2c4, IMU_ADDR, 0x00, &r0, 1);
  i2c_read_regs(g_i2c4, IMU_ADDR, 0x0f, &r0f, 1);
  i2c_read_regs(g_i2c4, IMU_ADDR, 0x75, &r75, 1);
  i2c_read_regs(g_i2c4, IMU_ADDR, 0x6b, &r6b, 1);
  ALOGI("IMU 0x68 regs 00=%02x 0f=%02x 6b=%02x 75=%02x", r0, r0f, r6b, r75);

  if (imu_try_icm(&ax, &ay, &az)) {
    ALOGI("IMU map=ICM206xx accel=%.2f %.2f %.2f", ax, ay, az);
    return IMU_ICM;
  }
  if (imu_try_lsm6(&ax, &ay, &az)) {
    ALOGI("IMU map=LSM6 accel=%.2f %.2f %.2f", ax, ay, az);
    return IMU_LSM6;
  }
  if (imu_try_bmi(&ax, &ay, &az)) {
    ALOGI("IMU map=BMI accel=%.2f %.2f %.2f", ax, ay, az);
    return IMU_BMI;
  }
  if (imu_try_mpu(&ax, &ay, &az)) {
    ALOGI("IMU map=MPU accel=%.2f %.2f %.2f", ax, ay, az);
    return IMU_MPU;
  }
  ALOGW("IMU 0x68 not published (no gravity-sized sample)");
  return IMU_NONE;
}

static void imu_enable(void) {
  if (g_imu_kind == IMU_ICM) {
    imu_icm_wake();
  } else if (g_imu_kind == IMU_LSM6) {
    i2c_write_reg(g_i2c4, IMU_ADDR, 0x10, 0x40);
    i2c_write_reg(g_i2c4, IMU_ADDR, 0x11, 0x40);
    i2c_write_reg(g_i2c4, IMU_ADDR, 0x12, 0x44);
  } else if (g_imu_kind == IMU_BMI) {
    i2c_write_reg(g_i2c4, IMU_ADDR, 0x7e, 0x11);
    usleep(50000);
    i2c_write_reg(g_i2c4, IMU_ADDR, 0x7e, 0x15);
  } else if (g_imu_kind == IMU_MPU) {
    i2c_write_reg(g_i2c4, IMU_ADDR, 0x6b, 0x00);
  }
}

static int imu_read_accel(sensors_event_t *ev) {
  uint8_t raw[6];
  float ax, ay, az;

  if (g_imu_kind == IMU_ICM) {
    if (!imu_icm_bank(ICM_BANK0))
      return -1;
    if (i2c_read_regs(g_i2c4, IMU_ADDR, ICM_REG_ACCEL_XOUT_H, raw, 6) != 0)
      return -1;
    ax = be16(raw) * (9.80665f / 16384.0f);
    ay = be16(raw + 2) * (9.80665f / 16384.0f);
    az = be16(raw + 4) * (9.80665f / 16384.0f);
  } else if (g_imu_kind == IMU_LSM6) {
    if (i2c_read_regs(g_i2c4, IMU_ADDR, 0x28, raw, 6) != 0)
      return -1;
    ax = le16(raw) * (9.80665f / 16384.0f);
    ay = le16(raw + 2) * (9.80665f / 16384.0f);
    az = le16(raw + 4) * (9.80665f / 16384.0f);
  } else if (g_imu_kind == IMU_BMI) {
    if (i2c_read_regs(g_i2c4, IMU_ADDR, 0x12, raw, 6) != 0)
      return -1;
    ax = le16(raw) * (9.80665f / 4096.0f);
    ay = le16(raw + 2) * (9.80665f / 4096.0f);
    az = le16(raw + 4) * (9.80665f / 4096.0f);
  } else if (g_imu_kind == IMU_MPU) {
    if (i2c_read_regs(g_i2c4, IMU_ADDR, 0x3b, raw, 6) != 0)
      return -1;
    ax = be16(raw) * (9.80665f / 16384.0f);
    ay = be16(raw + 2) * (9.80665f / 16384.0f);
    az = be16(raw + 4) * (9.80665f / 16384.0f);
  } else {
    return 0;
  }
  fill_common(ev, HANDLE_ACCEL, SENSOR_TYPE_ACCELEROMETER);
  ev->acceleration.x = ax;
  ev->acceleration.y = ay;
  ev->acceleration.z = az;
  ev->acceleration.status = SENSOR_STATUS_ACCURACY_MEDIUM;
  g_last_accel[0] = ax;
  g_last_accel[1] = ay;
  g_last_accel[2] = az;
  g_have_accel = 1;
  return 1;
}

static int imu_read_gyro(sensors_event_t *ev) {
  uint8_t raw[6];
  float gx, gy, gz;
  const float dps_to_rad = 3.14159265f / 180.0f;

  if (g_imu_kind == IMU_ICM) {
    if (!imu_icm_bank(ICM_BANK0))
      return -1;
    if (i2c_read_regs(g_i2c4, IMU_ADDR, ICM_REG_GYRO_XOUT_H, raw, 6) != 0)
      return -1;
    gx = be16(raw) / 131.0f * dps_to_rad;
    gy = be16(raw + 2) / 131.0f * dps_to_rad;
    gz = be16(raw + 4) / 131.0f * dps_to_rad;
  } else if (g_imu_kind == IMU_LSM6) {
    if (i2c_read_regs(g_i2c4, IMU_ADDR, 0x22, raw, 6) != 0)
      return -1;
    /* 250 dps → 8.75 mdps/LSB */
    gx = le16(raw) * 0.00875f * dps_to_rad;
    gy = le16(raw + 2) * 0.00875f * dps_to_rad;
    gz = le16(raw + 4) * 0.00875f * dps_to_rad;
  } else if (g_imu_kind == IMU_BMI) {
    if (i2c_read_regs(g_i2c4, IMU_ADDR, 0x0c, raw, 6) != 0)
      return -1;
    /* 2000 dps → 16.4 LSB/dps */
    gx = le16(raw) / 16.4f * dps_to_rad;
    gy = le16(raw + 2) / 16.4f * dps_to_rad;
    gz = le16(raw + 4) / 16.4f * dps_to_rad;
  } else if (g_imu_kind == IMU_MPU) {
    if (i2c_read_regs(g_i2c4, IMU_ADDR, 0x43, raw, 6) != 0)
      return -1;
    /* 250 dps → 131 LSB/dps */
    gx = be16(raw) / 131.0f * dps_to_rad;
    gy = be16(raw + 2) / 131.0f * dps_to_rad;
    gz = be16(raw + 4) / 131.0f * dps_to_rad;
  } else {
    return 0;
  }
  g_last_gyro_raw[0] = gx;
  g_last_gyro_raw[1] = gy;
  g_last_gyro_raw[2] = gz;
  g_have_gyro = 1;
  gyro_update_bias(gx, gy, gz);
  gx -= g_gyro_bias[0];
  gy -= g_gyro_bias[1];
  gz -= g_gyro_bias[2];
  if (g_have_accel) {
    float an = vlen3(g_last_accel[0], g_last_accel[1], g_last_accel[2]);
    if (an >= 8.5f && an <= 11.0f && vlen3(gx, gy, gz) < GYRO_REST_DEADBAND) {
      gx = gy = gz = 0;
    }
  }
  fill_common(ev, HANDLE_GYRO, SENSOR_TYPE_GYROSCOPE);
  ev->gyro.x = gx;
  ev->gyro.y = gy;
  ev->gyro.z = gz;
  ev->gyro.status = g_gyro_calibrated ? SENSOR_STATUS_ACCURACY_HIGH
                                      : SENSOR_STATUS_ACCURACY_MEDIUM;
  return 1;
}

static void ensure_sensor_list(void) {
  if (g_imu_probed)
    return;
  if (open_buses() != 0) {
    memcpy(g_list, k_list, sizeof(k_list));
    g_list_n = (int)(sizeof(k_list) / sizeof(k_list[0]));
    return;
  }
  g_imu_kind = imu_probe();
  g_imu_probed = 1;
  memcpy(g_list, k_list, sizeof(k_list));
  g_list_n = (int)(sizeof(k_list) / sizeof(k_list[0]));
  if (g_imu_kind != IMU_NONE) {
    g_list[g_list_n++] = k_accel;
    g_list[g_list_n++] = k_gyro;
  }
}

static int open_buses(void) {
  if (g_i2c4 < 0) {
    g_i2c4 = open(I2C4_PATH, O_RDWR | O_CLOEXEC);
    if (g_i2c4 < 0) {
      ALOGE("open %s: %s", I2C4_PATH, strerror(errno));
      return -errno;
    }
  }
  if (g_i2c7 < 0) {
    g_i2c7 = open(I2C7_PATH, O_RDWR | O_CLOEXEC);
    if (g_i2c7 < 0) {
      ALOGE("open %s: %s", I2C7_PATH, strerror(errno));
      return -errno;
    }
  }
  return 0;
}

static void sample_locked(void) {
  int64_t t = now_ns();
  sensors_event_t ev;

  if ((g_enabled & (1 << HANDLE_ACCEL)) && t >= g_next_ns[HANDLE_ACCEL]) {
    if (imu_read_accel(&ev) > 0)
      enqueue_locked(&ev);
    g_next_ns[HANDLE_ACCEL] = t + g_period_ns[HANDLE_ACCEL];
  }
  if ((g_enabled & (1 << HANDLE_GYRO)) && t >= g_next_ns[HANDLE_GYRO]) {
    if (imu_read_gyro(&ev) > 0)
      enqueue_locked(&ev);
    g_next_ns[HANDLE_GYRO] = t + g_period_ns[HANDLE_GYRO];
  }
  if ((g_enabled & (1 << HANDLE_MAG)) && t >= g_next_ns[HANDLE_MAG]) {
    int n = ak_read(&ev);
    if (n > 0)
      enqueue_locked(&ev);
    g_next_ns[HANDLE_MAG] = t + g_period_ns[HANDLE_MAG];
  }
  if ((g_enabled & ((1 << HANDLE_PROX) | (1 << HANDLE_LIGHT))) &&
      t >= g_next_ns[HANDLE_PROX]) {
    sensors_event_t prox, light;
    int gp, gl;
    if (apds_read_both(&gp, &prox, &gl, &light) == 0) {
      if (gp)
        enqueue_locked(&prox);
      if (gl)
        enqueue_locked(&light);
    }
    g_next_ns[HANDLE_PROX] = t + 100000000LL;
    g_next_ns[HANDLE_LIGHT] = g_next_ns[HANDLE_PROX];
  }
  if ((g_enabled & (1 << HANDLE_HALL_FRONT)) &&
      t >= g_next_ns[HANDLE_HALL_FRONT]) {
    if (hall_read(HANDLE_HALL_FRONT, &ev) > 0)
      enqueue_locked(&ev);
    g_next_ns[HANDLE_HALL_FRONT] = t + 100000000LL;
  }
  if ((g_enabled & (1 << HANDLE_HALL_BACK)) &&
      t >= g_next_ns[HANDLE_HALL_BACK]) {
    if (hall_read(HANDLE_HALL_BACK, &ev) > 0)
      enqueue_locked(&ev);
    g_next_ns[HANDLE_HALL_BACK] = t + 100000000LL;
  }
  zpa_sample_locked(t);
}

static void enqueue_flush_locked(int handle) {
  sensors_event_t ev;
  memset(&ev, 0, sizeof(ev));
  ev.version = sizeof(sensors_event_t);
  ev.sensor = 0;
  ev.type = SENSOR_TYPE_META_DATA;
  ev.timestamp = now_ns();
  ev.meta_data.what = META_DATA_FLUSH_COMPLETE;
  ev.meta_data.sensor = handle;
  enqueue_locked(&ev);
}

static int activate(struct sensors_poll_device_t *dev, int handle,
                    int enabled) {
  int idx = handle_index(handle);
  (void)dev;
  if (idx < 0)
    return -EINVAL;
  pthread_mutex_lock(&g_lock);
  drop_handle_locked(handle);
  if (enabled) {
    g_enabled |= (1 << handle);
    if (g_period_ns[handle] <= 0)
      g_period_ns[handle] = 50000000;
    g_next_ns[handle] = now_ns();
    if (handle == HANDLE_MAG) {
      i2c_write_reg(g_i2c4, AK_ADDR, AK_CNTL2, AK_CNTL2_CONT_50HZ);
      g_period_ns[handle] = 20000000;
    }
    if (handle == HANDLE_PROX || handle == HANDLE_LIGHT) {
      apds_write(APDS_ENABLE,
                 APDS_ENABLE_PON | APDS_ENABLE_AEN | APDS_ENABLE_PEN);
      g_have_prox = 0;
      g_have_light = 0;
    }
    if (handle == HANDLE_HALL_FRONT)
      g_have_hall_front = 0;
    if (handle == HANDLE_HALL_BACK)
      g_have_hall_back = 0;
    if (handle == HANDLE_PRESS || handle == HANDLE_TEMP) {
      zpa_prepare();
      g_period_ns[handle] = 200000000;
      g_zpa_busy = 0;
      g_zpa_saw_busy = 0;
    }
    if (handle == HANDLE_ACCEL || handle == HANDLE_GYRO)
      imu_enable();
  } else {
    g_enabled &= ~(1 << handle);
    if (handle == HANDLE_MAG)
      i2c_write_reg(g_i2c4, AK_ADDR, AK_CNTL2, AK_CNTL2_POWER_DOWN);
    if ((g_enabled & ((1 << HANDLE_PROX) | (1 << HANDLE_LIGHT))) == 0)
      apds_write(APDS_ENABLE, 0x00);
    if (handle == HANDLE_HALL_FRONT)
      g_have_hall_front = 0;
    if (handle == HANDLE_HALL_BACK)
      g_have_hall_back = 0;
  }
  pthread_cond_signal(&g_cv);
  pthread_mutex_unlock(&g_lock);
  ALOGI("activate handle=%d enabled=%d", handle, enabled);
  return 0;
}

static int set_delay(struct sensors_poll_device_t *dev, int handle,
                     int64_t period_ns) {
  int idx = handle_index(handle);
  (void)dev;
  if (idx < 0)
    return -EINVAL;
  if ((handle == HANDLE_PRESS || handle == HANDLE_TEMP) &&
      period_ns < 200000000)
    period_ns = 200000000;
  if (handle == HANDLE_MAG)
    period_ns = 20000000;
  if ((handle == HANDLE_ACCEL || handle == HANDLE_GYRO) && period_ns < 20000000)
    period_ns = 20000000;
  if (period_ns < 5000000)
    period_ns = 5000000;
  if (period_ns > 1000000000LL)
    period_ns = 1000000000LL;
  pthread_mutex_lock(&g_lock);
  g_period_ns[handle] = period_ns;
  pthread_mutex_unlock(&g_lock);
  return 0;
}

static int poll_events(struct sensors_poll_device_t *dev, sensors_event_t *data,
                       int count) {
  int copied = 0;
  (void)dev;
  if (count <= 0)
    return -EINVAL;
  pthread_mutex_lock(&g_lock);
  while (g_qcount == 0) {
    struct timespec ts;
    int64_t now = now_ns();
    int64_t delta = 200000000LL;
    int64_t wake;
    int i;
    for (i = HANDLE_MAG; i <= HANDLE_TEMP; i++) {
      if ((g_enabled & (1 << i)) && g_next_ns[i] > 0) {
        int64_t d = g_next_ns[i] - now;
        if (d < delta)
          delta = d;
      }
    }
    if (delta < 2000000LL)
      delta = 2000000LL;
    wake = mono_ns() + delta;
    ts.tv_sec = wake / 1000000000LL;
    ts.tv_nsec = wake % 1000000000LL;
    pthread_cond_timedwait(&g_cv, &g_lock, &ts);
    sample_locked();
    if (g_qcount == 0 && g_enabled == 0) {
      /* Nothing enabled: wait for activate instead of spinning. */
      pthread_cond_wait(&g_cv, &g_lock);
    }
  }
  while (copied < count && g_qcount > 0) {
    data[copied++] = g_q[g_qhead];
    g_qhead = (g_qhead + 1) % QMAX;
    g_qcount--;
  }
  pthread_mutex_unlock(&g_lock);
  cal_flush();
  return copied;
}

static int batch(struct sensors_poll_device_1 *dev, int handle, int flags,
                 int64_t period_ns, int64_t max_latency_ns) {
  (void)flags;
  (void)max_latency_ns;
  return set_delay(&dev->v0, handle, period_ns);
}

static int flush_sensor(struct sensors_poll_device_1 *dev, int handle) {
  int idx = handle_index(handle);
  (void)dev;
  if (idx < 0)
    return -EINVAL;
  pthread_mutex_lock(&g_lock);
  if ((g_enabled & (1 << handle)) == 0) {
    pthread_mutex_unlock(&g_lock);
    return -EINVAL;
  }
  enqueue_flush_locked(handle);
  pthread_mutex_unlock(&g_lock);
  return 0;
}

static int inject_sensor_data(struct sensors_poll_device_1 *dev,
                              const sensors_event_t *data) {
  (void)dev;
  (void)data;
  return -EPERM;
}

static int close_sensors(struct hw_device_t *dev) {
  (void)dev;
  pthread_mutex_lock(&g_lock);
  g_enabled = 0;
  if (g_i2c4 >= 0) {
    close(g_i2c4);
    g_i2c4 = -1;
  }
  if (g_i2c7 >= 0) {
    close(g_i2c7);
    g_i2c7 = -1;
  }
  g_opened = 0;
  pthread_cond_signal(&g_cv);
  pthread_mutex_unlock(&g_lock);
  return 0;
}

static sensors_poll_device_1_t g_device;

static int open_sensors(const struct hw_module_t *module, const char *id,
                        struct hw_device_t **device) {
  int err;
  if (!id || strcmp(id, SENSORS_HARDWARE_POLL) != 0)
    return -EINVAL;
  if (!g_cv_ready) {
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(&g_cv, &attr);
    pthread_condattr_destroy(&attr);
    g_cv_ready = 1;
  }
  err = open_buses();
  if (err != 0)
    return err;
  cal_load();
  ak_init();
  zpa_init();
  apds_init();
  ensure_sensor_list();
  memset(&g_device, 0, sizeof(g_device));
  g_device.v0.common.tag = HARDWARE_DEVICE_TAG;
  g_device.v0.common.version = SENSORS_DEVICE_API_VERSION_1_3;
  g_device.v0.common.module = (struct hw_module_t *)module;
  g_device.v0.common.close = close_sensors;
  g_device.v0.activate = activate;
  g_device.v0.setDelay = set_delay;
  g_device.v0.poll = poll_events;
  g_device.batch = batch;
  g_device.flush = flush_sensor;
  g_device.inject_sensor_data = inject_sensor_data;
  g_opened = 1;
  *device = &g_device.v0.common;
  ALOGI("opened sensors poll device");
  return 0;
}

static int get_sensors_list(struct sensors_module_t *module,
                            struct sensor_t const **list) {
  (void)module;
  ensure_sensor_list();
  *list = g_list;
  ALOGI("get_sensors_list n=%d imu=%d", g_list_n, g_imu_kind);
  return g_list_n;
}

static int set_operation_mode(unsigned int mode) {
  if (mode == SENSOR_HAL_NORMAL_MODE)
    return 0;
  return -EINVAL;
}

static struct hw_module_methods_t g_methods = {
    .open = open_sensors,
};

struct sensors_module_t HAL_MODULE_INFO_SYM = {
    .common =
        {
            .tag = HARDWARE_MODULE_TAG,
            .module_api_version = SENSORS_MODULE_API_VERSION_0_1,
            .hal_api_version = HARDWARE_HAL_API_VERSION,
            .id = SENSORS_HARDWARE_MODULE_ID,
            .name = "cityman I2C Sensors",
            .author = "cityman",
            .methods = &g_methods,
        },
    .get_sensors_list = get_sensors_list,
    .set_operation_mode = set_operation_mode,
};
