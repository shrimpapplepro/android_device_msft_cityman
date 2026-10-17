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

#define LOG_TAG "android.hardware.power@1.0-service.cityman"

#include "Power.h"

#include "Sysfs.h"

#include <errno.h>
#include <log/log.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

namespace android {
namespace hardware {
namespace power {
namespace V1_0 {
namespace implementation {

using cityman::power::HintDelta;
using cityman::power::clampBigKhz;
using cityman::power::kA57CeilingKhz;
using cityman::power::loadPowerHintConfig;
using cityman::power::nodeExists;
using cityman::power::readFile;
using cityman::power::readIntNode;
using cityman::power::snapDown;
using cityman::power::writeIntNode;
using cityman::power::writeNode;

static const char kHintPath[] = "/vendor/etc/powerhint.xml";
static const char kLittleMin[] = "/sys/devices/system/cpu/cpu0/cpufreq/scaling_min_freq";
static const char kLittleMax[] = "/sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq";
static const char kLittleHispeed[] =
        "/sys/devices/system/cpu/cpu0/cpufreq/interactive/hispeed_freq";
static const char kBigMin[] = "/sys/devices/system/cpu/cpu4/cpufreq/scaling_min_freq";
static const char kBigMax[] = "/sys/devices/system/cpu/cpu4/cpufreq/scaling_max_freq";
static const char kBigHispeed[] = "/sys/devices/system/cpu/cpu4/cpufreq/interactive/hispeed_freq";
static const char kBigCpuinfoMax[] = "/sys/devices/system/cpu/cpu4/cpufreq/cpuinfo_max_freq";
static const char kPerfMax[] = "/sys/module/msm_performance/parameters/cpu_max_freq";
static const char kBoostFreq[] = "/sys/module/cpu_boost/parameters/input_boost_freq";
static const char kBoostMs[] = "/sys/module/cpu_boost/parameters/input_boost_ms";
static const char kGpuMaxPwr[] = "/sys/class/kgsl/kgsl-3d0/max_pwrlevel";
static const char kGpuMinPwr[] = "/sys/class/kgsl/kgsl-3d0/min_pwrlevel";
static const char kGpuDefPwr[] = "/sys/class/kgsl/kgsl-3d0/default_pwrlevel";
static const char kRpmStats[] = "/sys/kernel/debug/rpm_stats";

static int writeBigMax(int khz) {
    int fused = 0;
    if (readIntNode(kBigCpuinfoMax, &fused) == 0 && fused > 0) {
        if (fused > kA57CeilingKhz)
            ALOGW("cpuinfo_max_freq %d kHz above A57 ceiling %d, clamping", fused,
                  kA57CeilingKhz);
        khz = clampBigKhz(khz, fused);
    } else {
        khz = clampBigKhz(khz, kA57CeilingKhz);
    }

    int rc = writeIntNode(kBigMax, khz);
    if (rc == -ENOENT)
        rc = 0;
    char vote[64];
    snprintf(vote, sizeof(vote), "4:%d 5:%d", khz, khz);
    int rc2 = writeNode(kPerfMax, vote);
    if (rc2 == -ENOENT)
        rc2 = 0;
    if (rc < 0 && rc2 < 0)
        return rc;
    return 0;
}

Power::Power() {
    mUsable = loadPowerHintConfig(kHintPath, &mCfg);
    if (!mUsable) {
        ALOGE("refusing IPower: %s missing or invalid (no sysfs stub)", kHintPath);
        return;
    }
    if (!nodeExists(kLittleMin)) {
        ALOGE("refusing IPower: %s missing", kLittleMin);
        mUsable = false;
        return;
    }

    mTimer = std::thread(&Power::timerLoop, this);
    {
        std::lock_guard<std::mutex> lock(mLock);
        commitLocked();
    }
}

Power::~Power() {
    {
        std::lock_guard<std::mutex> lock(mLock);
        mStop = true;
    }
    mCv.notify_all();
    if (mTimer.joinable())
        mTimer.join();
}

const HintDelta *Power::hintNamed(const char *name) const {
    auto it = mCfg.hints.find(name);
    if (it == mCfg.hints.end())
        return nullptr;
    return &it->second;
}

int Power::applyDeltaLocked(const HintDelta &d) {
    int failed = 0;
    int ok = 0;

    auto oneInt = [&](const char *path, int v, bool required) {
        int rc = writeIntNode(path, v);
        if (rc == 0)
            ok++;
        else if (required || rc != -ENOENT)
            failed++;
    };
    auto oneStr = [&](const char *path, const char *v, bool required) {
        int rc = writeNode(path, v);
        if (rc == 0)
            ok++;
        else if (required || rc != -ENOENT)
            failed++;
    };

    if (d.has_little_min)
        oneInt(kLittleMin, snapDown(mCfg.little.table, d.little_min_khz), true);
    if (d.has_little_max)
        oneInt(kLittleMax, snapDown(mCfg.little.table, d.little_max_khz), true);
    if (d.has_little_hispeed)
        oneInt(kLittleHispeed, snapDown(mCfg.little.table, d.little_hispeed_khz), false);

    if (d.has_big_min)
        oneInt(kBigMin, snapDown(mCfg.big.table, clampBigKhz(d.big_min_khz, kA57CeilingKhz)),
               false);
    if (d.has_big_hispeed)
        oneInt(kBigHispeed, snapDown(mCfg.big.table, clampBigKhz(d.big_hispeed_khz, kA57CeilingKhz)),
               false);
    if (d.has_big_max) {
        int rc = writeBigMax(snapDown(mCfg.big.table, d.big_max_khz));
        if (rc < 0)
            failed++;
        else
            ok++;
    }

    if (d.has_gpu_max)
        oneInt(kGpuMaxPwr, d.gpu_max_pwrlevel, false);
    if (d.has_gpu_min)
        oneInt(kGpuMinPwr, d.gpu_min_pwrlevel, false);
    if (d.has_gpu_default)
        oneInt(kGpuDefPwr, d.gpu_default_pwrlevel, false);

    if (d.has_boost) {
        if (!d.boost_freq.empty())
            oneStr(kBoostFreq, d.boost_freq.c_str(), false);
        oneInt(kBoostMs, d.boost_ms, false);
    }

    if (ok == 0) {
        ALOGE("power hint applied 0 sysfs nodes (%d failed)", failed);
        return -EIO;
    }
    if (failed)
        ALOGW("power hint: %d sysfs writes failed, %d ok", failed, ok);
    return 0;
}

void Power::commitLocked() {
    HintDelta d = mCfg.defaults;
    auto overlay = [&](const char *name, bool on) {
        if (!on)
            return;
        const HintDelta *h = hintNamed(name);
        if (h)
            d.overlay(*h);
    };

    overlay("VIDEO_ENCODE", mVideoEncode);
    overlay("LAUNCH", mLaunch && mInteractive);
    overlay("INTERACTION", mInteraction && mInteractive);
    overlay("SUSTAINED_PERFORMANCE", mSustained);
    overlay("VR_MODE", mVr);
    overlay("LOW_POWER", mLowPower);
    overlay("INTERACTIVE_OFF", !mInteractive);

    if (d.has_little_min && d.has_little_max && d.little_min_khz > d.little_max_khz)
        d.little_min_khz = d.little_max_khz;
    if (d.has_big_min && d.has_big_max && d.big_min_khz > d.big_max_khz)
        d.big_min_khz = d.big_max_khz;

    int rc = applyDeltaLocked(d);
    if (rc < 0)
        ALOGE("commit sysfs failed: %d", rc);
}

void Power::armTimerLocked() {
    mCv.notify_all();
}

void Power::timerLoop() {
    std::unique_lock<std::mutex> lock(mLock);
    while (!mStop) {
        clock::time_point next = clock::time_point::max();
        if (mLaunch)
            next = std::min(next, mLaunchUntil);
        if (mInteraction)
            next = std::min(next, mInteractionUntil);
        if (next == clock::time_point::max()) {
            mCv.wait(lock, [&] { return mStop || mLaunch || mInteraction; });
            continue;
        }
        if (mCv.wait_until(lock, next) == std::cv_status::timeout) {
            auto now = clock::now();
            bool changed = false;
            if (mLaunch && now >= mLaunchUntil) {
                mLaunch = false;
                mLaunchUntil = clock::time_point::max();
                changed = true;
            }
            if (mInteraction && now >= mInteractionUntil) {
                mInteraction = false;
                mInteractionUntil = clock::time_point::max();
                changed = true;
            }
            if (changed)
                commitLocked();
        }
    }
}

Return<void> Power::setInteractive(bool interactive) {
    std::lock_guard<std::mutex> lock(mLock);
    if (!mUsable)
        return Void();
    mInteractive = interactive;
    if (!interactive) {
        mLaunch = false;
        mInteraction = false;
        mLaunchUntil = clock::time_point::max();
        mInteractionUntil = clock::time_point::max();
    }
    commitLocked();
    return Void();
}

Return<void> Power::powerHint(PowerHint hint, int32_t data) {
    std::lock_guard<std::mutex> lock(mLock);
    if (!mUsable)
        return Void();

    auto enableTimed = [&](bool *flag, clock::time_point *until, const char *name, int fallbackMs) {
        const HintDelta *h = hintNamed(name);
        int ms = fallbackMs;
        if (h && h->duration_ms > 0)
            ms = h->duration_ms;
        if (data > 1)
            ms = data;
        *flag = true;
        *until = clock::now() + std::chrono::milliseconds(ms);
        armTimerLocked();
    };

    switch (hint) {
        case PowerHint::VSYNC:
            return Void();
        case PowerHint::INTERACTION:
            enableTimed(&mInteraction, &mInteractionUntil, "INTERACTION", 1500);
            break;
        case PowerHint::LAUNCH:
            if (data == 0) {
                mLaunch = false;
                mLaunchUntil = clock::time_point::max();
            } else if (data == 1) {
                const HintDelta *h = hintNamed("LAUNCH");
                int ms = (h && h->duration_ms > 0) ? h->duration_ms : 2000;
                mLaunch = true;
                mLaunchUntil = clock::now() + std::chrono::milliseconds(ms);
                armTimerLocked();
            } else {
                enableTimed(&mLaunch, &mLaunchUntil, "LAUNCH", data);
            }
            break;
        case PowerHint::VIDEO_ENCODE:
            mVideoEncode = data != 0;
            break;
        case PowerHint::VIDEO_DECODE:
            return Void();
        case PowerHint::LOW_POWER:
            mLowPower = data != 0;
            break;
        case PowerHint::SUSTAINED_PERFORMANCE:
            mSustained = data != 0;
            break;
        case PowerHint::VR_MODE:
            mVr = data != 0;
            break;
        default:
            ALOGV("unhandled hint %u data %d", static_cast<unsigned>(hint), data);
            return Void();
    }

    commitLocked();
    return Void();
}

Return<void> Power::setFeature(Feature feature, bool activate) {
    if (feature == Feature::POWER_FEATURE_DOUBLE_TAP_TO_WAKE) {
        ALOGI("DOUBLE_TAP_TO_WAKE activate=%d: no cityman sysfs node", activate);
    }
    return Void();
}

Return<void> Power::getPlatformLowPowerStats(getPlatformLowPowerStats_cb _hidl_cb) {
    hidl_vec<PowerStatePlatformSleepState> states;
    std::string raw;
    if (readFile(kRpmStats, &raw) < 0 || raw.empty()) {
        _hidl_cb(states, Status::FILESYSTEM_ERROR);
        return Void();
    }

    std::vector<PowerStatePlatformSleepState> parsed;
    PowerStatePlatformSleepState cur;
    bool have = false;
    auto flush = [&]() {
        if (have)
            parsed.push_back(cur);
        cur = PowerStatePlatformSleepState();
        cur.supportedOnlyInSuspend = false;
        have = false;
    };

    size_t pos = 0;
    while (pos < raw.size()) {
        size_t nl = raw.find('\n', pos);
        std::string line = raw.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = (nl == std::string::npos) ? raw.size() : nl + 1;
        if (line.compare(0, 9, "RPM_Mode:") == 0 ||
            line.compare(0, 9, "RPM Mode:") == 0) {
            flush();
            cur.name = line.substr(9);
            have = true;
            continue;
        }
        if (!have)
            continue;
        const char *p = line.c_str();
        while (*p == '\t' || *p == ' ')
            p++;
        unsigned long long ull = 0;
        int count = 0;
        /* Kernel format: "vlow_count:0" / "vmin_count:40076" */
        if (sscanf(p, "%*[a-z_]count:%d", &count) == 1 ||
            sscanf(p, "count:%d", &count) == 1) {
            cur.totalTransitions = static_cast<uint64_t>(count);
        /* Kernel format: "accumulated_vlow_time(msec):48213000" */
        } else if (sscanf(p, "accumulated_%*[a-z_]time(msec):%llu", &ull) == 1 ||
                   sscanf(p, "actual last sleep(msec):%llu", &ull) == 1) {
            cur.residencyInMsecSinceBoot = ull;
        }
    }
    flush();

    if (parsed.empty()) {
        ALOGE("%s parsed 0 RPM modes", kRpmStats);
        _hidl_cb(states, Status::FILESYSTEM_ERROR);
        return Void();
    }

    states.resize(parsed.size());
    for (size_t i = 0; i < parsed.size(); i++)
        states[i] = parsed[i];
    _hidl_cb(states, Status::SUCCESS);
    return Void();
}

}  // namespace implementation
}  // namespace V1_0
}  // namespace power
}  // namespace hardware
}  // namespace android
