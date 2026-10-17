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

#include "Sysfs.h"

#include <errno.h>
#include <fcntl.h>
#include <log/log.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <string>

namespace cityman {
namespace power {

int writeNode(const char *path, const char *value) {
    if (!path || !value)
        return -EINVAL;

    int fd = TEMP_FAILURE_RETRY(open(path, O_WRONLY | O_CLOEXEC));
    if (fd < 0) {
        int err = -errno;
        if (err == -ENOENT)
            ALOGV("open(%s) write: %s", path, strerror(-err));
        else
            ALOGE("open(%s) write: %s", path, strerror(-err));
        return err;
    }

    size_t len = strlen(value);
    ssize_t n = TEMP_FAILURE_RETRY(write(fd, value, len));
    int saved = errno;
    close(fd);

    if (n < 0) {
        ALOGE("write(%s, \"%s\"): %s", path, value, strerror(saved));
        return -saved;
    }
    if (static_cast<size_t>(n) != len) {
        ALOGE("write(%s, \"%s\"): short %zd/%zu", path, value, n, len);
        return -EIO;
    }
    return 0;
}

int writeIntNode(const char *path, int value) {
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%d", value);
    if (n < 0 || static_cast<size_t>(n) >= sizeof(buf))
        return -EINVAL;
    return writeNode(path, buf);
}

int readNode(const char *path, std::string *out) {
    if (!path || !out)
        return -EINVAL;

    int fd = TEMP_FAILURE_RETRY(open(path, O_RDONLY | O_CLOEXEC));
    if (fd < 0) {
        int err = -errno;
        ALOGE("open(%s) read: %s", path, strerror(-err));
        return err;
    }

    char buf[256];
    ssize_t n = TEMP_FAILURE_RETRY(read(fd, buf, sizeof(buf) - 1));
    int saved = errno;
    close(fd);
    if (n < 0) {
        ALOGE("read(%s): %s", path, strerror(saved));
        return -saved;
    }

    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r' || buf[n - 1] == ' '))
        n--;
    buf[n] = '\0';
    *out = buf;
    return 0;
}

int readFile(const char *path, std::string *out) {
    if (!path || !out)
        return -EINVAL;

    int fd = TEMP_FAILURE_RETRY(open(path, O_RDONLY | O_CLOEXEC));
    if (fd < 0) {
        int err = -errno;
        ALOGE("open(%s) read: %s", path, strerror(-err));
        return err;
    }

    std::string acc;
    char buf[1024];
    for (;;) {
        ssize_t n = TEMP_FAILURE_RETRY(read(fd, buf, sizeof(buf)));
        if (n < 0) {
            int saved = errno;
            close(fd);
            ALOGE("read(%s): %s", path, strerror(saved));
            return -saved;
        }
        if (n == 0)
            break;
        acc.append(buf, static_cast<size_t>(n));
    }
    close(fd);
    *out = std::move(acc);
    return 0;
}

int readIntNode(const char *path, int *out) {
    std::string s;
    int rc = readNode(path, &s);
    if (rc < 0)
        return rc;
    char *end = nullptr;
    long v = strtol(s.c_str(), &end, 10);
    if (end == s.c_str())
        return -EINVAL;
    *out = static_cast<int>(v);
    return 0;
}

bool nodeExists(const char *path) {
    return access(path, F_OK) == 0;
}

}  // namespace power
}  // namespace cityman
