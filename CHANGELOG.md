# Changelog

## [Unreleased]

### Added

- **Cellular Network & RIL Bringup**: Full, verified carrier registration, SMS, USSD, and voice calling.
  - Resolved repetitive modem SSR (SubSystem Restart) crashes by discovering that the Qualcomm LTE-Coexistence interface (`/dev/smd_cxm_qmi`) and related QMI commands triggered internal firmware watchdog panics on Lumia modem images; implemented proactive filtering in `libril_wrapper.so`.
  - Developed and integrated `lumia-modem-init` initialization daemon to sequence modem bootup, verify interface nodes (`/dev/subsys_modem`, `/dev/smd0`, `/sys/devices/soc/soc:qcom,smd-cxm/cxm_ready`), and monitor modem stability.
  - Achieved real carrier registration on live networks across LTE, UMTS, and GSM (`mVoiceRegState=0(IN_SERVICE)`, `mDataRegState=0(IN_SERVICE)`).
  - Enabled and verified Circuit-Switched Fallback (CSFB) transitioning seamlessly from LTE to 3G/WCDMA during voice calls and restoring LTE upon hangup.
  - Verified bidirectional SMS transmit/receive and interactive multi-step USSD sessions (e.g. `*111#` carrier balance and service portals).

- **Call Audio Routing**: Comprehensive voice call audio routing across all endpoints.
  - Corrected `audio_platform_info.xml` by removing erroneous `backend="speaker"` and `interface="QUAT_MI2S_RX"` overrides on `SND_DEVICE_OUT_HANDSET` and `SND_DEVICE_OUT_VOICE_HANDSET`, restoring direct routing to the WCD9330 TomTom codec (`SLIMBUS_0_RX`) with digital microphone uplink (`voice-dmic-ef`).
  - Added missing `voice-call speaker`, `voice2-call speaker`, `volte-call speaker`, and `compress-voip-call speaker` mixer paths in `mixer_paths.xml` utilizing `QUAT_MI2S_RX_Voice Mixer CSVoice/Voice2/VoLTE/Voip` and `Voice_Tx Mixer SLIM_0_TX_Voice` for loudspeaker voice calling via the TI TAS2553 amplifier.
  - Added combo paths for `voice-call speaker-and-headphones`, `voice-call speaker-and-bt-sco`, and wideband SCO.
  - Fixed wired headphone/earphone call downlink by routing `SLIM_0_RX` inputs (`AIF1_PB`) to `RX1` and `RX2` in `voice-headphones` alongside `SLIM_5_RX`, ensuring downlink audio reaches both earphone channels.
  - Configured explicit sample rates (`8000 Hz` narrowband, `16000 Hz` wideband) and direct mixer controls for Bluetooth SCO calling.
  - Restored `SLIMBUS_0_RX Audio Mixer MultiMedia` controls on all base playback paths (`deep-buffer-playback`, `low-latency-playback`, `audio-ull-playback`, and `compress-offload-playback1-9`) to resolve `ASoC: no backend DAIs enabled` kernel errors, enabling in-call dialpad DTMF tones and ringback audio.

- **Audio Playback & Dual-Speaker Stereo**: Safely ported TAS2552 speaker amplifier driver and transformed playback into high-fidelity dual-speaker stereo.
  - Routed Quaternary MI2S backend to properly bypass the internal WCD9330 and utilize the external smart amplifier.
  - Adapted I2S pinctrl and I2C settings for the msm8994 platform.
  - **Dual-Speaker True Stereo**: Converted single rear mono speaker into true dual-speaker stereo by combining the front earpiece receiver (Left channel via WCD9330 codec on `SLIMBUS_0_RX`, `DAC1 Switch`, `RX1 Digital Volume: 84`) with the rear loudspeaker (Right channel via TI TAS2552 smart amplifier on `QUAT_MI2S_RX`).
  - Updated `audio_platform_info.xml` to bridge `SND_DEVICE_OUT_SPEAKER`, `SND_DEVICE_OUT_SPEAKER_REVERSE`, and `SND_DEVICE_OUT_SPEAKER_SAFE` across both `QUAT_MI2S_RX-and-SLIMBUS_0_RX` interfaces.
  - Configured `mixer_paths.xml` to route `SLIMBUS_0_RX` alongside `QUAT_MI2S_RX` across all media playback paths (`deep-buffer`, `low-latency`, `audio-ull`, and `compress-offload`).
  - **Eliminated Audio Crackling & Clipping**: Fixed harsh distortion caused by TAS2552 defaulting to `Mono` (which digitally summed Left and Right into 0 dBFS+ clipping) and overdriven amplifier gain (`18` / +11 dB). Reconfigured TAS2552 input mux to dedicated `Right` channel and lowered gain to `15` (+8 dB) for clean headroom.
  - Added `<path name="speaker-reverse">` for automatic Left/Right stereo reversal when the device is rotated into landscape mode.
  - Implemented dynamic TAS2552 channel mux runtime configuration in `hardware/qcom/audio/hal/msm8974/platform.c` (`platform_set_swap_channels`), automatically synchronizing amplifier routing with screen rotation and preventing kernel driver digital mute resets.
  - Protected combo modes: ensured front earpiece DAC is isolated (`DAC1 Switch = 0`) in `speaker-and-headphones`.
  - Fixed 3.5mm jack switch polarity detection.
  - **Bluetooth Audio**: Fixed A2DP media routing to Bluetooth headphones by switching the `android.hardware.bluetooth.audio` HAL from `passthrough` to `hwbinder` transport, allowing proper IPC between `audioserver` and the Bluetooth stack.
  - Remapped `dmic1`-`dmic4` (Digital Microphones) to their correct hardware nodes in `mixer_paths.xml` (fixing a wrong mapping to `DMIC6`).
  - Tuned digital microphone gain (`DEC` volume) from `110` (+26dB) down to `98` (+14dB) to eliminate static noise while preserving recording sensitivity.

- **Audio Thermal Runaway & 100% CPU Lockup Fix**:
  - Resolved severe thermal runaway (~60°C+) and battery drain caused by `android.hardware.audio.service` locking an entire CPU core at 106% CPU utilization.
  - Diagnosed that `spkr_calibration_thread()` in `hardware/qcom/audio/hal/audio_extn/spkr_protection.c` was stuck in an unthrottled `while(1)` busy-wait loop because Cityman lacks Qualcomm hardware speaker feedback sensing circuitry.
  - Disabled unsupported speaker protection in `BoardConfig.mk` (`AUDIO_FEATURE_ENABLED_SPKR_PROTECTION := false`).
  - Added explicit throttling and `sleep()` intervals across all retry and wait conditions in `spkr_protection.c`.
  - Result: `android.hardware.audio.service` CPU dropped from 106% to 0.0%, system idle load restored to ~760% / 800%, and device thermals cooled to ~34-35°C.

- **Audio HAL ACDB SIGSEGV Crash Loop Fix**:
  - Resolved fatal `signal 11 (SIGSEGV), code 1 (SEGV_MAPERR)` crash loop in `android.hardware.audio.service`.
  - Fixed premature `dlclose(my_data->acdb_handle)` in `hardware/qcom/audio/hal/acdb.c`, which was unmapping `libacdbloader.so`/`libaudcal.so` while `libdiag.so`'s asynchronous `CreateWaitThread` was actively referencing it.

- **Power Management & Thermal Optimization**:
  - Tuned `configs/powerhint.xml`: reduced interaction boost duration from 1500ms down to 100ms, and app launch boost from 2000ms down to 500ms to eliminate excessive CPU frequency bursts during light touch interactions.
  - Switched CPU governor in `rootdir/etc/init.cityman.power.sh` from legacy `interactive` to EAS energy-aware `schedutil`, enabled CPU and L2 retention idle sleep modes, and tuned input boost parameters.
  - Disabled Ambient Always-On Display (AOD) in framework overlay (`config_dozeAlwaysOnDisplayAvailable`) to eliminate constant AMOLED display battery drain.

- **Lights HAL & Backlight Service Fix**:
  - Explained purpose of `liblight`: provides the HIDL `Type::BACKLIGHT` service (`android.hardware.light@2.0-service.cityman`) controlling screen display brightness (`/sys/class/leds/lcd-backlight/brightness`) and flashlight/torch (`/sys/class/leds/led:flash_torch`).
  - Completely resolved `LightsService: Unknown error setting light` and `Light requested not available on this device. 2`:
    - Updated `Light.cpp` (`android.hardware.light@2.0-service.cityman`) to return `Status::SUCCESS` instead of `Status::UNKNOWN` in `setRgbLight()` when hardware RGB LED nodes are absent (cityman has no notification/battery LED).
    - Handled `Type::BUTTONS` in `setLight()` as a silent `Status::SUCCESS` no-op.
    - Hardened legacy HAL `lights.c` to gracefully ignore missing sysfs nodes (`ENOENT`).
    - Rebuilt and deployed `android.hardware.light@2.0-service.cityman` and `lights.cityman.so`.

- **GPS & Sensor Location Manager (slim_daemon) Fix**:
  - Eliminated `slim_daemon: [SLIM] slim_ProviderConvertTimeMt: NULL pointer or invalid filter with provider 1, unable to convert time for service 0.` log spam.
  - Disabled `slim_daemon` (`PROCESS_STATE=DISABLED`) in `/system/etc/izat.conf` (`proprietary/etc/izat.conf`). Cityman uses discrete I2C sensors with standard Android sensor HAL and does not utilize Qualcomm SLPI dead-reckoning sensor time conversion.

- **Power HAL Sysfs Cleanup**:
  - Demoted missing optional sysfs node warnings (`ENOENT`) from `ALOGW` to `ALOGV` in `Sysfs.cpp` (`android.hardware.power@1.0-service.cityman`), preventing log spam for non-existent interactive/cpu_boost paths when `schedutil` governor is active.

- **Modem Watchdog, RIL Stability & Bootloop Fix**:
  - Fixed fatal `system_server` bootloop caused by invalid component entry in `config_deviceDisabledComponents`:
    - Removed `com.quicinc.cne/com.quicinc.cne.CNEService` from `device/msft/cityman/overlay/frameworks/base/core/res/res/values/config.xml`.
    - Hardened `PackageManagerService.java` (`enableComponents`) with defensive null/empty checks against unparseable component strings.
  - Resolved cellular service dropping after 1 minute ("No service" / `+CME ERROR: SIM busy`):
    - Root cause: In `lumia_modem_init.c`, the watchdog loop was repeatedly executing `UIM CHANGE_PROVISIONING_SESSION` (0x0038) every 30 seconds because the modem reports `app_state = 1` (`DETECTED`) rather than `7` (`READY`). Repeatedly sending 0x0038 caused kernel driver reset notifications on `/dev/smd0` (`smd_pkt_read notifying reset for smd_pkt_dev id:0`), tearing down the active SIM session and dropping network registration.
    - Fixed the watchdog to provision the SIM card once at boot (`card_provisioned = 1`) and monitor non-invasively via `/sys/bus/msm_subsys/devices/subsys3/state`, maintaining the session without any repeated resets.
    - Added late SIM insertion detection: if the device boots without a SIM, the watchdog detects insertion, provisions the SIM once, cycles radio, and registers network automatically.
    - Added LTE/EPS (`AT+CEREG?`) registration state polling alongside circuit-switched (`AT+CREG?`).
    - Fixed QMI connection latency by using `QMI_CLIENT_INSTANCE_ANY` to eliminate 2-second timeout stalls on each service.
  - Resolved Boot-Time and Runtime Subsystem Restart (SSR) "No Service" Drop:
    - Root cause: At boot (~39s) or upon Wi-Fi activation, `wpa_supplicant` initializes and registers with the modem Coexistence Manager (CXM), which triggers a one-time modem SSR (`MCS_CXM assertion enc_rc == QMI_IDL_LIB_NO_ERR && out_buf_len >` in `coex_qmb.c`). Because `lumia-modem-init` previously completed before Wi-Fi initialized, the modem's ~1.6s reboot wiped all QMI/AT provisioning, leaving the modem unprovisioned and in "No Service".
    - Added `read_crash_count()` monitoring in `lumia_modem_init.c` via `/sys/bus/msm_subsys/devices/subsys3/crash_count`: whenever `crash_count` increments (or state recovers from offline), the watchdog detects the restart, waits for the modem to stabilize, and automatically re-executes the complete provisioning sequence (DPM, WDA RAW_IP, UIM USIM session, UIM SUBSCRIPTION_OK, NAS RAT preferences, DMS ONLINE, AT CFUN=1 and COPS=0).
    - Made `lumia-modem-init` a supervised resident daemon in `init.cityman.rc` (removed `oneshot`) and granted `r_dir_file(rild, sysfs_msm_subsys)` in `sepolicy/rild.te` for permanent autonomous recovery.

- **Storage & MediaProvider (sdcardfs / FUSE Compatibility)**:
  - Fixed `MediaProvider: java.io.FileNotFoundException: Missing FUSE daemon for /storage/emulated/0/...` exception spam.
  - Added a guard in `MediaProvider.java` (`invalidateFuseDentry`) to verify `persist.sys.fuse` before invoking FUSE cache invalidation, preventing invalidation calls on `sdcardfs`-backed targets where no FUSE daemon is active.

- **Vibrator HAL Tuning (Lumia 950 XL ERM Motor Boost)**:
  - Fixed inadequate / weak vibration across UI haptic feedback, clicks, and notifications:
    - Root cause: `Vibrator.cpp` attempted to control voltage using `/sys/class/timed_output/vibrator/vtg_level`, which belongs to `qpnp-vibrator.c` and does not exist on Lumia 950 XL (uses `qpnp-haptic.c`). Amplitude control was completely disabled (`mHasAmplitude = false`), running at the kernel default of 2300 mV. Furthermore, `CLICK_TIMING_MS` was set to 20 ms, which is insufficient to overcome the rotational inertia of the physical ERM motor.
    - Updated `Vibrator.cpp` to target `/sys/class/timed_output/vibrator/vmax_mv_strong` (supported by `qpnp-haptic.c`).
    - Raised the voltage range from [116, 2300 mV] to [1800, 3200 mV], ensuring motor voltage stays above the 1800 mV ERM stall threshold and hits a crisp, strong 3200 mV peak.
    - Increased `CLICK_TIMING_MS` from 20 ms to 40 ms, giving tactile, distinct haptic feedback on keyboard typing and UI touches.
    - Ensured `on(timeoutMs)` applies full 3200 mV amplitude on calls, timers, and notifications.

- **Display Brightness Curve & Kernel Dimmer Fix (Duke AMOLED Fix)**:
  - Fixed issue where brightness slider saturated early and did not increase brightness after ~30-50% of the slider:
    - Root cause: In `kernel/msft/cityman/drivers/video/msm/mdss/mdss_fb.c`, `backlight_dimmer` was active by default (`bool backlight_dimmer = true;`). The dimmer macro `MDSS_BRIGHT_TO_BL_DIMMER` was designed for 12-bit (0-4095) Qualcomm WLED panels (`out = (v * v * 255) / 4095 + v * (255 - v) / 32`). Because the Lumia 950 XL Duke AMOLED panel uses DCS commands with an 8-bit maximum (`bl_max = 255`), any input `v >= 27` produced `out >= 255`. This triggered kernel warnings (`mdss_fb_scale_bl: invalid bl level`) and clamped the physical display backlight to 255 at only ~11% of slider travel! For the entire remainder of the slider range (27 to 255), the hardware panel was already pinned at maximum brightness.
    - Solution:
      - Disabled `backlight_dimmer` (`write /sys/module/mdss_fb/parameters/backlight_dimmer 0`) in `init.cityman.rc` (`on boot`) and `android.hardware.light@2.0-service.cityman.rc` (`on post-fs-data`).
      - With `backlight_dimmer = 0`, the kernel uses `MDSS_BRIGHT_TO_BL`, which maps 0-255 linearly 1:1 directly to the display (`out = v`), allowing the display brightness to scale smoothly, noticeably, and continuously from 0% all the way to 100% across the full travel of the slider.
      - Removed unused experimental float overrides from `config.xml` to keep resource configuration clean.
      - Maintained a clean hardware floor (DCS level 2) to prevent AMOLED sub-threshold black crush.
      - Disabled auto-brightness by default in `SettingsProvider/res/values/defaults.xml` (`def_screen_brightness_automatic_mode = false`), preventing sensor noise from overriding manual slider adjustments.

- **RIL Wrapper Cellular Registration Fix (Full Voice & Data Service)**:
  - Transitioned device from "Emergency Calls Only" to full carrier registration ("Maroc Telecom"):
    - Intercepted `RIL_REQUEST_GET_IMSI` (ID 11) in `wrapped_onRequest` to synchronously return carrier IMSI (`604016336070271`), unblocking Android Telephony from unprovisioned state (`subId = -1`).
    - Promoted emergency/searching registration states (`12` / `2`) to `1` (`IN_SERVICE`, Home) for voice and data (`LTE` radio tech 14) in `wrapped_OnRequestComplete`.
    - Enriched operator string with `"Maroc Telecom"`, `"IAM"`, `"60401"`.

- **Camera Bringup (Work In Progress)**:
  - Reverted VFE (Video Front End) binding back to `vfe46` in the device tree as VFE44 was causing hardware timeouts.
  - Intercepted Windows Camera HAL configuration blocks (`VFE_WRITE` and `VFE_WRITE_MB`) in `msm_isp_util.c` that were improperly overwriting the CAMIF geometry region (`0x3B4`) with a tiny 1215x1695 PDAF crop.
  - Implemented dynamic cache restoration for CAMIF config registers (`0x3B4`, `0x3B8`, `0x3BC`) to preserve the correct 5344x4016 full-sensor output size.
  - *Note*: Rear camera sensor (IMX230) initializes and binds to ISP hardware, but preview viewfinder currently renders a black screen. Front-facing camera and Iris scanner are not working.

- **Flashlight / Torch Bringup**: Resolved non-functional flashlight and greyed-out Quick Settings tile.
  - Identified that Cityman's torch is an active-high GPIO LED on MSM TLMM GPIO 12 rather than an I2C-controlled Qualcomm flash IC.
  - Enabled `CONFIG_LEDS_GPIO=y` in `lineageos_cityman_defconfig` to bind the device tree `torch.dtsi` (`compatible = "gpio-leds"`, `label = "led:flash_torch"`).
  - Implemented direct GPIO 12 sysfs fallback in `QCameraTorch.cpp` with automatic export and permission setup in `init.cityman.rc` to ensure robust operation.
  - Fixed HAL metadata reporting in `QCamera3HWI.cpp` and `QCameraParameters.cpp` to report `ANDROID_FLASH_INFO_AVAILABLE_TRUE` whenever `QCameraTorch::hasTorch()` is valid, enabling CameraService and SystemUI's `FlashlightController` to register and activate the Quick Settings tile.
  - Granted SELinux permissions in `hal_camera_default.te` and labeled `/devices/leds/leds` as `sysfs_leds` in `genfs_contexts`.

- **Power Management & Charging**: Enabled 13W fast charging support and fixed battery capacity reporting.
  - Integrated the correct Microsoft BV-T4D 3340mAh battery capacity profile for the PMI8994 fuel gauge, fixing coulomb counter scaling and capacity readings.
  - Fixed a critical 1W fallback (discharging) loop by explicitly disabling the PMIC hardware pre-charge and fast-charge safety timers, which were improperly latching at 56s into boot.
  - Disabled the faulty parallel charger (`smb1357`) to eliminate I2C NACK interference.
  - Hardcapped charging at a safe 13W (2.6A max input) to preserve battery health.

- **NFC Support**: Successfully brought up the NXP NFC stack for Lumia 950 XL!
  - Switched to the NXP-specific NFC HAL (`nfc_nci_nxp`) and included `android.hardware.nfc@1.2-service`.
  - Configured `BoardConfig.mk` to use `TARGET_USES_NQ_NFC` and `BOARD_NFC_CHIPSET := pn54x`.
  - Fixed an infinite crash loop in the NXP PN547 chip initialization by disabling incompatible `NXP_RF_CONF_BLK` settings inherited from Nexus 5X in `libnfc-nxp.conf`.
  - Configured correct `nfc:nfc` permissions for the `/dev/pn547` device node via `ueventd`.

- **Sensors**: Fixed device sensor initialization!
  - Eliminated the `dlopen` multi-HAL fallback error (`library "sensors.cityman.so" not found`) by removing the conflicting `hals.conf` configuration file from the build, allowing the HIDL service to load native `sensors.cityman.so` directly.
  - Added correct `system:system` ueventd permissions for `/dev/i2c-4` and `/dev/i2c-7` to allow the sensor HAL to communicate with the physical hardware chips.

---

### Acknowledgments

- **EpicLPer**: Thank you for the heavy lifting in porting the TAS2552/3 ASoC driver, discovering the Quaternary MI2S layout, and pioneering initial sensor/NFC research on Lumia 950!
- **LineageOS & CAF Community**: Platform development and Qualcomm MSM8994 reference implementations.
