/*
 * Copyright (C) 2017 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 *
 * Out-of-line android_atomic_* for old camera blobs
 * (OnePlus 2 libmmcamera2_stats_algorithm.so). Built as C: in C++ the
 * Android 11 cutils/atomic.h pulls in <atomic>, which cannot sit in extern "C".
 */
#define ANDROID_ATOMIC_INLINE
#include <cutils/atomic.h>
