#!/bin/bash
# Run from the top of a synced LineageOS-UL lineage-20.0 tree (with lineage-20/local_manifest.xml).
# 1. The UL forks lag LineageOS; current LineageOS apps need its newer framework APIs,
#    so merge LineageOS lineage-20.0 into the forks that need it (NOT vendor/lineage:
#    upstream dropped prebuilt apns-conf.xml there).
# 2. Apply the cityman patches on top.
set -e
top=$(pwd)
here=$top/device/msft/cityman/lineage-20
merge() {
  cd "$top/$1"
  git fetch -q --shallow-since=2024-06-01 https://github.com/LineageOS/$2 lineage-20.0
  git branch -f los-upstream FETCH_HEAD
  git fetch -q --shallow-since=2024-06-01 "$(git remote get-url "$(git remote | head -1)")" HEAD
  git merge --no-edit -m "Merge LineageOS lineage-20.0 into $1" los-upstream
}
merge frameworks/base android_frameworks_base
merge frameworks/av android_frameworks_av
merge packages/modules/Bluetooth android_packages_modules_Bluetooth
merge packages/modules/Wifi android_packages_modules_Wifi
for p in frameworks_av:frameworks/av packages_modules_Bluetooth:packages/modules/Bluetooth hardware_qcom_bt:hardware/qcom/bt; do
  cd "$top/${p#*:}" && git am -3 "$here/patches/${p%%:*}"/*.patch
done
echo "cityman: tree ready; build with: source build/envsetup.sh && lunch lineage_cityman-userdebug && WITH_ADB_INSECURE=true m bacon"
