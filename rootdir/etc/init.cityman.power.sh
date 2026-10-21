#!/vendor/bin/sh

# Cityman MSM8994 (4x Cortex-A53 + 4x Cortex-A57).
#
# CAF already exposes interactive, cpu_boost, HMP, and kgsl through sysfs.
# init.cityman.rc runs this script `on boot`, which is the device-tree
# place to set those knobs.
#
# Take A57s offline while msm_thermal core_control is off so interactive
# can bind on cpu4, then plugin cpu4/cpu5/cpu6/cpu7. Permanent offline is
# charger-only (init.cityman.rc on charger).

################################################################################
# helper functions to allow Android init like script

function write() {
    echo -n $2 > $1
}

function copy() {
    cat $1 > $2
}

function get-set-forall() {
    for f in $1 ; do
        cat $f
        write $f $2
    done
}

################################################################################

# take the A57s offline when thermal hotplug is disabled
write /sys/devices/system/cpu/cpu4/online 0
write /sys/devices/system/cpu/cpu5/online 0
write /sys/devices/system/cpu/cpu6/online 0
write /sys/devices/system/cpu/cpu7/online 0

# disable thermal bcl hotplug to switch governor
write /sys/module/msm_thermal/core_control/enabled 0
get-set-forall /sys/devices/soc.0/qcom,bcl.*/mode disable
bcl_hotplug_mask=`get-set-forall /sys/devices/soc.0/qcom,bcl.*/hotplug_mask 0`
bcl_hotplug_soc_mask=`get-set-forall /sys/devices/soc.0/qcom,bcl.*/hotplug_soc_mask 0`
get-set-forall /sys/devices/soc.0/qcom,bcl.*/mode enable

# some files in /sys/devices/system/cpu are created after the restorecon of
# /sys/. These files receive the default label "sysfs".
# Restorecon again to give new files the correct label.
restorecon -R /sys/devices/system/cpu

# Best effort limiting for first time boot if msm_performance module is absent
write /sys/devices/system/cpu/cpu4/cpufreq/scaling_max_freq 960000

# Limit A57 max freq from msm_perf module in case CPU 4 is offline
write /sys/module/msm_performance/parameters/cpu_max_freq "4:960000 5:960000 6:960000 7:960000"

# Disable CPU retention
# write /sys/module/lpm_levels/system/a53/cpu0/retention/idle_enabled 0
# write /sys/module/lpm_levels/system/a53/cpu1/retention/idle_enabled 0
# write /sys/module/lpm_levels/system/a53/cpu2/retention/idle_enabled 0
# write /sys/module/lpm_levels/system/a53/cpu3/retention/idle_enabled 0
# write /sys/module/lpm_levels/system/a57/cpu4/retention/idle_enabled 0
# write /sys/module/lpm_levels/system/a57/cpu5/retention/idle_enabled 0
# write /sys/module/lpm_levels/system/a57/cpu6/retention/idle_enabled 0
# write /sys/module/lpm_levels/system/a57/cpu7/retention/idle_enabled 0

# Disable L2 retention
# write /sys/module/lpm_levels/system/a53/a53-l2-retention/idle_enabled 0
# write /sys/module/lpm_levels/system/a57/a57-l2-retention/idle_enabled 0

# configure governor settings for little cluster
write /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor schedutil
restorecon -R /sys/devices/system/cpu
write /sys/devices/system/cpu/cpu0/cpufreq/scaling_min_freq 384000

# online CPU4
write /sys/devices/system/cpu/cpu4/online 1

# configure governor settings for big cluster
write /sys/devices/system/cpu/cpu4/cpufreq/scaling_governor schedutil
restorecon -R /sys/devices/system/cpu
write /sys/devices/system/cpu/cpu4/cpufreq/scaling_min_freq 384000

# A57 max: 1.248 GHz, not 1.958. The worn 3340 mAh cell (~300 mOhm ESR) browns
# out (PMIC UVLO -> SMPL reboot) on A57 + camera current peaks, and the 810 runs
# hot; 1248 MHz roughly halves the A57 peak power.
write /sys/devices/system/cpu/cpu4/cpufreq/scaling_max_freq 1248000

# plugin remaining A57s
write /sys/devices/system/cpu/cpu5/online 1
write /sys/devices/system/cpu/cpu6/online 1
write /sys/devices/system/cpu/cpu7/online 1

# Restore CPU 4-7 max freq from msm_performance
write /sys/module/msm_performance/parameters/cpu_max_freq "4:1248000 5:1248000 6:1248000 7:1248000"

# input boost configuration (kernel cpu-boost, fires on every touch event).
# Boost policy CPU only; scheduler handles the rest. 300ms matches INTERACTION hint.
write /sys/module/cpu_boost/parameters/input_boost_freq "0:960000 4:1248000"
write /sys/module/cpu_boost/parameters/input_boost_ms 300
write /sys/module/cpu_boost/parameters/sched_boost_on_input Y

# Setting B.L scheduler parameters
write /proc/sys/kernel/sched_migration_fixup 1
write /proc/sys/kernel/sched_upmigrate 75
write /proc/sys/kernel/sched_downmigrate 60
write /proc/sys/kernel/sched_freq_inc_notify 400000
write /proc/sys/kernel/sched_freq_dec_notify 400000

# enable rps static configuration
write /sys/class/net/rmnet_ipa0/queues/rx-0/rps_cpus 8

# android background processes are set to nice 10. Never schedule these on the a57s.
write /proc/sys/kernel/sched_upmigrate_min_nice 9

get-set-forall  /sys/class/devfreq/qcom,cpubw*/governor bw_hwmon

# Disable sched_boost
write /proc/sys/kernel/sched_boost 0

# re-enable thermal core control
write /sys/module/msm_thermal/core_control/enabled 1
# Qualcomm BCL on: the VPH dips were real, not false positives - with BCL off the
# phone reset on them (PMIC: power-off UVLO, power-on SMPL). Clamp to 768 MHz
# while VPH < 3.3 V (released at 3.5 V) instead of losing power.
get-set-forall /sys/devices/soc.0/qcom,bcl.*/freq_max 768000
get-set-forall /sys/devices/soc.0/qcom,bcl.*/mode enable

# GPU wake level 4 (300MHz), same as qcom,initial-pwrlevel.
# Adreno 430 on the 1440x2560 Cityman panel benefits from waking at 300MHz.
write /sys/class/kgsl/kgsl-3d0/default_pwrlevel 4

# VM tuning — fix dentry/inode cache hoarding (default was 10, far too low)
write /proc/sys/vm/vfs_cache_pressure 100

# Power HAL runs as system. Nodes it writes (A57 online, interactive created).
chown system system /sys/devices/system/cpu/cpu0/cpufreq/scaling_min_freq
chown system system /sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq
chown system system /sys/devices/system/cpu/cpu4/cpufreq/scaling_min_freq
chown system system /sys/devices/system/cpu/cpu4/cpufreq/scaling_max_freq
chmod 0664 /sys/devices/system/cpu/cpu0/cpufreq/scaling_min_freq
chmod 0664 /sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq
chmod 0664 /sys/devices/system/cpu/cpu4/cpufreq/scaling_min_freq
chmod 0664 /sys/devices/system/cpu/cpu4/cpufreq/scaling_max_freq
chown system system /sys/module/cpu_boost/parameters/input_boost_freq
chown system system /sys/module/cpu_boost/parameters/input_boost_ms
chmod 0664 /sys/module/cpu_boost/parameters/input_boost_freq
chmod 0664 /sys/module/cpu_boost/parameters/input_boost_ms
chown system system /sys/module/msm_performance/parameters/cpu_max_freq
chmod 0664 /sys/module/msm_performance/parameters/cpu_max_freq
chown system system /sys/class/kgsl/kgsl-3d0/max_pwrlevel
chown system system /sys/class/kgsl/kgsl-3d0/min_pwrlevel
chown system system /sys/class/kgsl/kgsl-3d0/default_pwrlevel
chmod 0664 /sys/class/kgsl/kgsl-3d0/max_pwrlevel
chmod 0664 /sys/class/kgsl/kgsl-3d0/min_pwrlevel
chmod 0664 /sys/class/kgsl/kgsl-3d0/default_pwrlevel
