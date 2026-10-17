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
#ifndef CITYMAN_POWER_HINTCONFIG_H
#define CITYMAN_POWER_HINTCONFIG_H

#include <map>
#include <string>
#include <vector>

namespace cityman {
namespace power {

/* MSM8994 cityman speedbin supports 1958400 kHz (cpuinfo_max_freq). */
static constexpr int kA57CeilingKhz = 1958400;

struct ClusterOpp {
  int policy_cpu = -1;
  std::vector<int> table;
};

struct HintDelta {
  bool has_little_min = false;
  bool has_little_max = false;
  bool has_little_hispeed = false;
  int little_min_khz = 0;
  int little_max_khz = 0;
  int little_hispeed_khz = 0;

  bool has_big_min = false;
  bool has_big_max = false;
  bool has_big_hispeed = false;
  int big_min_khz = 0;
  int big_max_khz = 0;
  int big_hispeed_khz = 0;

  bool has_gpu_min = false;
  bool has_gpu_max = false;
  bool has_gpu_default = false;
  int gpu_min_pwrlevel = 0;
  int gpu_max_pwrlevel = 0;
  int gpu_default_pwrlevel = 0;

  bool has_boost = false;
  std::string boost_freq;
  int boost_ms = 0;

  int duration_ms = 0;

  void overlay(const HintDelta &o);
};

struct PowerHintConfig {
  ClusterOpp little;
  ClusterOpp big;
  HintDelta defaults;
  std::map<std::string, HintDelta> hints;
};

bool loadPowerHintConfig(const char *path, PowerHintConfig *out);

int snapDown(const std::vector<int> &table, int khz);
int clampBigKhz(int khz, int cpuinfo_max_khz);

} // namespace power
} // namespace cityman

#endif
