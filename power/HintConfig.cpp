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

#include "HintConfig.h"

#include <cutils/properties.h>

#include <errno.h>
#include <fcntl.h>
#include <log/log.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <string>
#include <utility>

namespace cityman {
namespace power {

void HintDelta::overlay(const HintDelta &o) {
  if (o.has_little_min) {
    has_little_min = true;
    little_min_khz = o.little_min_khz;
  }
  if (o.has_little_max) {
    has_little_max = true;
    little_max_khz = o.little_max_khz;
  }
  if (o.has_little_hispeed) {
    has_little_hispeed = true;
    little_hispeed_khz = o.little_hispeed_khz;
  }
  if (o.has_big_min) {
    has_big_min = true;
    big_min_khz = o.big_min_khz;
  }
  if (o.has_big_max) {
    has_big_max = true;
    big_max_khz = o.big_max_khz;
  }
  if (o.has_big_hispeed) {
    has_big_hispeed = true;
    big_hispeed_khz = o.big_hispeed_khz;
  }
  if (o.has_gpu_min) {
    has_gpu_min = true;
    gpu_min_pwrlevel = o.gpu_min_pwrlevel;
  }
  if (o.has_gpu_max) {
    has_gpu_max = true;
    gpu_max_pwrlevel = o.gpu_max_pwrlevel;
  }
  if (o.has_gpu_default) {
    has_gpu_default = true;
    gpu_default_pwrlevel = o.gpu_default_pwrlevel;
  }
  if (o.has_boost) {
    has_boost = true;
    boost_freq = o.boost_freq;
    boost_ms = o.boost_ms;
  }
}

int snapDown(const std::vector<int> &table, int khz) {
  if (table.empty())
    return khz;
  int best = table.front();
  for (int f : table) {
    if (f <= khz)
      best = f;
    else
      break;
  }
  return best;
}

/*
 * Runtime A57 cap (kHz), default 1248000. The worn cityman battery browns out
 * (PMIC UVLO) on A57 peaks at 1.96 GHz; every big max/min/hispeed this HAL
 * writes is clamped to it. persist.vendor.power.a57_cap_khz overrides.
 */
static int a57CapKhz() {
  static int cap = -1;
  if (cap < 0) {
    cap = property_get_int32("persist.vendor.power.a57_cap_khz", 1248000);
    if (cap <= 0 || cap > kA57CeilingKhz)
      cap = kA57CeilingKhz;
  }
  return cap;
}

int clampBigKhz(int khz, int cpuinfo_max_khz) {
  int cap = a57CapKhz();
  if (cpuinfo_max_khz > 0 && cpuinfo_max_khz < cap)
    cap = cpuinfo_max_khz;
  if (khz > cap)
    khz = cap;
  if (khz < 0)
    khz = 0;
  return khz;
}

static std::string stripComments(const std::string &in) {
  std::string out;
  out.reserve(in.size());
  size_t i = 0;
  while (i < in.size()) {
    if (i + 3 < in.size() && in.compare(i, 4, "<!--") == 0) {
      size_t e = in.find("-->", i + 4);
      if (e == std::string::npos)
        break;
      i = e + 3;
      continue;
    }
    out.push_back(in[i++]);
  }
  return out;
}

static bool attr(const std::string &tag, const char *key, std::string *out) {
  std::string pat = std::string(key) + "=\"";
  size_t p = tag.find(pat);
  if (p == std::string::npos)
    return false;
  p += pat.size();
  size_t e = tag.find('"', p);
  if (e == std::string::npos)
    return false;
  *out = tag.substr(p, e - p);
  return true;
}

static bool attrInt(const std::string &tag, const char *key, int *out) {
  std::string s;
  if (!attr(tag, key, &s))
    return false;
  char *end = nullptr;
  long v = strtol(s.c_str(), &end, 10);
  if (end == s.c_str())
    return false;
  *out = static_cast<int>(v);
  return true;
}

static void parseClusterAttrs(const std::string &tag, HintDelta *d,
                              bool little) {
  int v;
  if (attrInt(tag, "min_khz", &v)) {
    if (little) {
      d->has_little_min = true;
      d->little_min_khz = v;
    } else {
      d->has_big_min = true;
      d->big_min_khz = v;
    }
  }
  if (attrInt(tag, "max_khz", &v)) {
    if (little) {
      d->has_little_max = true;
      d->little_max_khz = v;
    } else {
      d->has_big_max = true;
      d->big_max_khz = v;
    }
  }
  if (attrInt(tag, "hispeed_khz", &v)) {
    if (little) {
      d->has_little_hispeed = true;
      d->little_hispeed_khz = v;
    } else {
      d->has_big_hispeed = true;
      d->big_hispeed_khz = v;
    }
  }
}

static void parseGpuAttrs(const std::string &tag, HintDelta *d) {
  int v;
  if (attrInt(tag, "min_pwrlevel", &v)) {
    d->has_gpu_min = true;
    d->gpu_min_pwrlevel = v;
  }
  if (attrInt(tag, "max_pwrlevel", &v)) {
    d->has_gpu_max = true;
    d->gpu_max_pwrlevel = v;
  }
  if (attrInt(tag, "default_pwrlevel", &v)) {
    d->has_gpu_default = true;
    d->gpu_default_pwrlevel = v;
  }
}

static void parseBoostAttrs(const std::string &tag, HintDelta *d) {
  std::string freq;
  int ms = 0;
  bool got = false;
  if (attr(tag, "freq", &freq)) {
    d->boost_freq = freq;
    got = true;
  }
  if (attrInt(tag, "ms", &ms)) {
    d->boost_ms = ms;
    got = true;
  }
  if (got)
    d->has_boost = true;
}

static bool validateOpp(const char *name, const ClusterOpp &c, int ceiling) {
  if (c.policy_cpu < 0 || c.table.empty()) {
    ALOGE("powerhint.xml: %s cluster missing policy_cpu or Freq table", name);
    return false;
  }
  int prev = -1;
  for (int f : c.table) {
    if (f <= 0 || f <= prev) {
      ALOGE("powerhint.xml: %s Freq %d not strictly increasing", name, f);
      return false;
    }
    if (f > ceiling) {
      ALOGE("powerhint.xml: %s Freq %d above ceiling %d", name, f, ceiling);
      return false;
    }
    prev = f;
  }
  return true;
}

static bool clampDeltaToOpp(HintDelta *d, const PowerHintConfig &cfg) {
  auto snapL = [&](int khz) { return snapDown(cfg.little.table, khz); };
  auto snapB = [&](int khz) {
    return snapDown(cfg.big.table, clampBigKhz(khz, kA57CeilingKhz));
  };
  if (d->has_little_min)
    d->little_min_khz = snapL(d->little_min_khz);
  if (d->has_little_max)
    d->little_max_khz = snapL(d->little_max_khz);
  if (d->has_little_hispeed)
    d->little_hispeed_khz = snapL(d->little_hispeed_khz);
  if (d->has_big_min)
    d->big_min_khz = snapB(d->big_min_khz);
  if (d->has_big_max)
    d->big_max_khz = snapB(d->big_max_khz);
  if (d->has_big_hispeed)
    d->big_hispeed_khz = snapB(d->big_hispeed_khz);
  return true;
}

bool loadPowerHintConfig(const char *path, PowerHintConfig *out) {
  if (!path || !out)
    return false;

  int fd = TEMP_FAILURE_RETRY(open(path, O_RDONLY | O_CLOEXEC));
  if (fd < 0) {
    ALOGE("powerhint.xml open(%s): %s", path, strerror(errno));
    return false;
  }

  std::string raw;
  char buf[4096];
  for (;;) {
    ssize_t n = TEMP_FAILURE_RETRY(read(fd, buf, sizeof(buf)));
    if (n < 0) {
      int saved = errno;
      close(fd);
      ALOGE("powerhint.xml read(%s): %s", path, strerror(saved));
      return false;
    }
    if (n == 0)
      break;
    raw.append(buf, static_cast<size_t>(n));
  }
  close(fd);

  if (raw.empty()) {
    ALOGE("powerhint.xml empty: %s", path);
    return false;
  }

  std::string xml = stripComments(raw);
  PowerHintConfig cfg;
  HintDelta *cur = nullptr;
  HintDelta currentHint;
  ClusterOpp *curOpp = nullptr;
  bool inHint = false;
  bool sawRoot = false;
  std::string hintName;

  size_t i = 0;
  while (i < xml.size()) {
    size_t lt = xml.find('<', i);
    if (lt == std::string::npos)
      break;
    size_t gt = xml.find('>', lt + 1);
    if (gt == std::string::npos) {
      ALOGE("powerhint.xml: unterminated tag");
      return false;
    }
    std::string tag = xml.substr(lt + 1, gt - lt - 1);
    i = gt + 1;
    if (tag.empty() || tag[0] == '?')
      continue;
    if (tag[0] == '/') {
      std::string name = tag.substr(1);
      if (name == "Opp")
        curOpp = nullptr;
      else if (name == "Defaults") {
        cur = nullptr;
      } else if (name == "Hint") {
        if (inHint && !hintName.empty())
          cfg.hints[hintName] = *cur;
        inHint = false;
        cur = nullptr;
        hintName.clear();
      }
      continue;
    }
    if (tag.back() == '/')
      tag.pop_back();

    if (tag.compare(0, 11, "HintConfigs") == 0) {
      std::string plat;
      if (attr(tag, "platform", &plat) && plat != "msm8992" &&
          plat != "msm8994") {
        ALOGE("powerhint.xml platform=%s, expected msm8992 or msm8994",
              plat.c_str());
        return false;
      }
      sawRoot = true;
      continue;
    }
    if (tag.compare(0, 3, "Opp") == 0) {
      std::string cluster;
      int cpu = -1;
      attr(tag, "cluster", &cluster);
      attrInt(tag, "policy_cpu", &cpu);
      if (cluster == "little") {
        cfg.little.policy_cpu = cpu;
        curOpp = &cfg.little;
      } else if (cluster == "big") {
        cfg.big.policy_cpu = cpu;
        curOpp = &cfg.big;
      } else {
        ALOGE("powerhint.xml: unknown Opp cluster");
        return false;
      }
      continue;
    }
    if (tag.compare(0, 4, "Freq") == 0) {
      int khz = 0;
      if (!curOpp || !attrInt(tag, "khz", &khz)) {
        ALOGE("powerhint.xml: Freq outside Opp or missing khz");
        return false;
      }
      curOpp->table.push_back(khz);
      continue;
    }
    if (tag.compare(0, 8, "Defaults") == 0) {
      cur = &cfg.defaults;
      continue;
    }
    if (tag.compare(0, 4, "Hint") == 0) {
      if (!attr(tag, "name", &hintName) || hintName.empty()) {
        ALOGE("powerhint.xml: Hint missing name");
        return false;
      }
      currentHint = HintDelta();
      attrInt(tag, "duration_ms", &currentHint.duration_ms);
      cur = &currentHint;
      inHint = true;
      continue;
    }
    if (!cur)
      continue;
    if (tag.compare(0, 6, "Little") == 0)
      parseClusterAttrs(tag, cur, true);
    else if (tag.compare(0, 3, "Big") == 0)
      parseClusterAttrs(tag, cur, false);
    else if (tag.compare(0, 3, "Gpu") == 0)
      parseGpuAttrs(tag, cur);
    else if (tag.compare(0, 10, "InputBoost") == 0)
      parseBoostAttrs(tag, cur);
  }

  if (!sawRoot) {
    ALOGE("powerhint.xml: missing HintConfigs");
    return false;
  }
  if (!validateOpp("little", cfg.little, 1440000))
    return false;
  if (!validateOpp("big", cfg.big, kA57CeilingKhz))
    return false;
  if (cfg.big.table.back() != kA57CeilingKhz) {
    ALOGE("powerhint.xml: big table max %d, expected %d", cfg.big.table.back(),
          kA57CeilingKhz);
    return false;
  }
  if (cfg.hints.empty()) {
    ALOGE("powerhint.xml: no Hint entries");
    return false;
  }

  clampDeltaToOpp(&cfg.defaults, cfg);
  for (auto &kv : cfg.hints)
    clampDeltaToOpp(&kv.second, cfg);

  *out = std::move(cfg);
  ALOGI(
      "powerhint.xml loaded from %s (little %zu OPP, big %zu OPP, cap %d kHz)",
      path, out->little.table.size(), out->big.table.size(), kA57CeilingKhz);
  return true;
}

} // namespace power
} // namespace cityman
