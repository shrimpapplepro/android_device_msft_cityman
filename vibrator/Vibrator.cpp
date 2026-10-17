/*
 * Copyright (C) 2016 The Android Open Source Project
 * Copyright (C) 2018 The LineageOS Project
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

#define LOG_TAG "VibratorService"

#include "Vibrator.h"

#include <log/log.h>

#include <cmath>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

namespace android {
namespace hardware {
namespace vibrator {
namespace V1_0 {
namespace implementation {

/*
 * qpnp-haptic.c timed_dev.name = "vibrator"
 *   → /sys/class/timed_output/vibrator/enable
 * vtg_level is vmax_mv (haptics.dtsi 2300, qpnp clamp 116-3596).
 */
static const char kEnablePath[] = "/sys/class/timed_output/vibrator/enable";
static const char kVmaxMvStrongPath[] = "/sys/class/timed_output/vibrator/vmax_mv_strong";
static const char kVtgLevelPath[] = "/sys/class/timed_output/vibrator/vtg_level";
static const char kVtgMinPath[] = "/sys/class/timed_output/vibrator/vtg_min";
static const char kVtgMaxPath[] = "/sys/class/timed_output/vibrator/vtg_max";

static constexpr uint32_t CLICK_TIMING_MS = 40;

static bool nodeWritable(const char *path) {
    int fd = TEMP_FAILURE_RETRY(open(path, O_RDWR));
    if (fd < 0)
        return false;
    close(fd);
    return true;
}

static int writeNode(const char *path, const char *value) {
    int fd = TEMP_FAILURE_RETRY(open(path, O_WRONLY));
    if (fd < 0)
        return -errno;

    size_t len = strlen(value);
    int written = TEMP_FAILURE_RETRY(write(fd, value, len));
    int saved = errno;
    close(fd);

    if (written < 0)
        return -saved;
    if (static_cast<size_t>(written) != len)
        return -EIO;
    return 0;
}

static bool readIntNode(const char *path, int *out) {
    int fd = TEMP_FAILURE_RETRY(open(path, O_RDONLY));
    if (fd < 0)
        return false;

    char buf[32];
    int n = TEMP_FAILURE_RETRY(read(fd, buf, sizeof(buf) - 1));
    close(fd);
    if (n <= 0)
        return false;

    buf[n] = '\0';
    char *end = nullptr;
    long v = strtol(buf, &end, 10);
    if (end == buf)
        return false;
    *out = static_cast<int>(v);
    return true;
}

static uint8_t amplitudeForStrength(EffectStrength strength) {
    switch (strength) {
        case EffectStrength::LIGHT:
            return 160;
        case EffectStrength::MEDIUM:
            return 210;
        case EffectStrength::STRONG:
        default:
            return 255;
    }
}

Vibrator::Vibrator() {
    mEnableOk = nodeWritable(kEnablePath);
    if (!mEnableOk) {
        ALOGE("%s missing or not writable (%s)", kEnablePath, strerror(errno));
        return;
    }

    /* Check for qpnp-haptic vmax_mv_strong first (Lumia 950 XL hardware) */
    if (nodeWritable(kVmaxMvStrongPath)) {
        mVtgPath = kVmaxMvStrongPath;
        mVtgMin = 1800; // Minimum spin voltage for ERM motor
        mVtgMax = 3200; // Designed peak voltage (PMI8994 max 3596 mV)
        mHasAmplitude = true;
        ALOGI("Vibrator using qpnp-haptic: %s [min=%d, max=%d]", mVtgPath, mVtgMin, mVtgMax);
    } else {
        /* Fallback to qpnp-vibrator nodes */
        int vmin = 0, vmax = 0;
        bool haveMin = readIntNode(kVtgMinPath, &vmin);
        bool haveMax = readIntNode(kVtgMaxPath, &vmax);
        if (haveMin && haveMax && vmin > 0 && vmax >= vmin && nodeWritable(kVtgLevelPath)) {
            mVtgPath = kVtgLevelPath;
            mVtgMin = std::max(vmin, 1800);
            mVtgMax = (vmax > 3200) ? 3200 : vmax;
            mHasAmplitude = true;
            ALOGI("Vibrator using qpnp-vibrator: %s [min=%d, max=%d]", mVtgPath, mVtgMin, mVtgMax);
        }
    }

    if (mHasAmplitude) {
        applyAmplitude(255);
    }
}

Return<Status> Vibrator::on(uint32_t timeoutMs) {
    std::lock_guard<std::mutex> lock(mLock);
    if (mHasAmplitude && timeoutMs > 0) {
        applyAmplitude(255);
    }
    return activate(timeoutMs);
}

Return<Status> Vibrator::off() {
    std::lock_guard<std::mutex> lock(mLock);
    return activate(0);
}

Return<bool> Vibrator::supportsAmplitudeControl() {
    return mHasAmplitude;
}

Return<Status> Vibrator::setAmplitude(uint8_t amplitude) {
    std::lock_guard<std::mutex> lock(mLock);
    return applyAmplitude(amplitude);
}

Return<void> Vibrator::perform(Effect effect, EffectStrength strength, perform_cb _hidl_cb) {
    if (effect != Effect::CLICK) {
        _hidl_cb(Status::UNSUPPORTED_OPERATION, 0);
        return Void();
    }

    std::lock_guard<std::mutex> lock(mLock);
    if (mHasAmplitude) {
        Status amp = applyAmplitude(amplitudeForStrength(strength));
        if (amp != Status::OK) {
            _hidl_cb(amp, 0);
            return Void();
        }
    }
    Status st = activate(CLICK_TIMING_MS);
    _hidl_cb(st, st == Status::OK ? CLICK_TIMING_MS : 0);
    return Void();
}

Status Vibrator::activate(uint32_t timeoutMs) {
    if (!mEnableOk)
        return Status::UNKNOWN_ERROR;

    char value[20];
    snprintf(value, sizeof(value), "%u\n", timeoutMs);
    int ret = writeNode(kEnablePath, value);
    if (ret != 0) {
        ALOGE("write %s failed (%s)", kEnablePath, strerror(-ret));
        return Status::UNKNOWN_ERROR;
    }
    return Status::OK;
}

Status Vibrator::applyAmplitude(uint8_t amplitude) {
    if (!mHasAmplitude)
        return Status::UNSUPPORTED_OPERATION;
    if (amplitude == 0)
        return Status::BAD_VALUE;

    long mv = std::lround((amplitude - 1) / 254.0 * (mVtgMax - mVtgMin) + mVtgMin);
    if (mv < mVtgMin)
        mv = mVtgMin;
    if (mv > mVtgMax)
        mv = mVtgMax;

    char value[20];
    snprintf(value, sizeof(value), "%ld\n", mv);
    if (!mVtgPath)
        return Status::UNSUPPORTED_OPERATION;
    int ret = writeNode(mVtgPath, value);
    if (ret != 0) {
        ALOGE("write %s failed (%s)", mVtgPath, strerror(-ret));
        return Status::UNKNOWN_ERROR;
    }
    return Status::OK;
}

}  // namespace implementation
}  // namespace V1_0
}  // namespace vibrator
}  // namespace hardware
}  // namespace android
