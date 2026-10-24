# Microsoft Lumia 950 XL (cityman)

## Android 13 is here

The Lumia 950 XL now runs **Android 13 (LineageOS 20)**. As far as we know, this is the first Android 13 build for the 950 XL; until now the newest Android on this phone was 11 (LineageOS 18.1).

**[Download the latest release](https://github.com/shrimpapplepro/android_device_msft_cityman/releases/tag/20261024-lineage20-sleep)**. You need both files: `lineage-20.0-20261024-UNOFFICIAL-cityman.zip` and `boot-20261024-lineage20-sleep-cityman.img`.

### What works (tested on our phone)

- Boot, display and touch
- Wi-Fi and Bluetooth (pairing, and a BLE gamepad as input)
- Audio playback through the loudspeaker
- Rear camera in Aperture: preview, full-resolution photos and autofocus. Preview runs at about 15 fps.
- Internal storage and USB adb
- Deep sleep with the screen off

### Not working yet

- **Cellular: no calls, SMS or mobile data.** The modem stays off on Android 13. If you need your SIM, use [Android 11](#lineageos-181-android-11-port-status) for now.
- LineageOS recovery has no USB, so this guide uses TWRP instead.
- Front camera and rear flash, same as on Android 11.

SELinux is permissive. `/data` is unencrypted: the phone's 3.10 kernel has no file-based encryption, and Android 13 removed full-disk encryption.

---

## How to install Android 13

Pick your starting point:

- **[A. From scratch](#a-from-scratch-the-phone-runs-windows-10-mobile)**: the phone still runs Windows 10 Mobile.
- **[B. From TWRP](#b-from-twrp-the-phone-already-runs-android)**: the phone already runs Android (Android 11, or an earlier Android 13 build).

### What you need

- A PC and a USB cable.
- `adb` and `fastboot`. On Windows, they are in the `bin` folder of [installer-cityman](https://github.com/shrimpapplepro/installer-cityman) (**Code > Download ZIP**). On Linux or macOS, install [Android platform-tools](https://developer.android.com/tools/releases/platform-tools).
- `twrp.img` from installer-cityman's `DATA` folder.
- Both files from the [release](https://github.com/shrimpapplepro/android_device_msft_cityman/releases/tag/20261024-lineage20-sleep). Check them against the md5 or sha256 checksums on the release page before you flash.
- A battery that is at least half charged.

Run every command below from the folder that holds the two release files and `twrp.img`.

### A. From scratch: the phone runs Windows 10 Mobile

The installer can't put Android 13 on a Windows phone directly yet. It installs Android 11 first and takes care of the steps that need care: unlocking the bootloader, backing up your phone's unique data, partitioning, TWRP and the modem firmware. Then you move to Android 13 with section B.

1. Download [installer-cityman](https://github.com/shrimpapplepro/installer-cityman) (**Code > Download ZIP**) and unzip it.
2. Download the Android 11 zip, `lineage-18.1-20261023-UNOFFICIAL-cityman.zip`, from the [Android 11 release](https://github.com/shrimpapplepro/android_device_msft_cityman/releases/tag/20261023-charging). Put it next to `Install LineageOS.cmd`.
3. Follow the [installer's guide](https://github.com/shrimpapplepro/installer-cityman#readme): unlock the bootloader with WPinternals, switch to Mass Storage Mode, then run `Install LineageOS.cmd`.
4. **Keep the `backup-<date>` folder** the installer creates. It holds your IMEI, radio calibration and Wi-Fi/Bluetooth addresses, and it can't be downloaded again.
5. Wait until Android 11 has booted. Then continue with section B. You are coming from Android 11, so the Format Data step applies to you.

### B. From TWRP: the phone already runs Android

1. **Turn on USB debugging.** Open **Settings > About phone** and tap **Build number** seven times. Then turn on **Settings > System > Developer options > USB debugging**. Connect the phone and allow the PC when the phone asks.
2. **Reboot to the bootloader:**
   ```
   adb reboot bootloader
   ```
   The phone shows a black screen with text (LK2ND). Check that `fastboot devices` lists it. On Windows, if it doesn't, open Device Manager: the phone should show up as *Android Bootloader Interface*. If it has a warning sign, install the [Google USB driver](https://developer.android.com/studio/run/win-usb) for it.
3. **Start TWRP.** This only runs TWRP. It doesn't install it.
   ```
   fastboot boot twrp.img
   ```
   Wait until TWRP's main screen is up and `adb devices` lists the phone as `recovery`.
4. **Format Data, only if you're coming from Android 11 (or from section A).** This erases all apps and files on the phone, so copy anything you want to keep to your PC first. In TWRP, tap **Wipe > Format Data**, type `yes` and confirm.
   If you're updating from an earlier Android 13 build, skip this step: your apps and files are kept.
5. **Install the zip.** It takes a few minutes.
   ```
   adb push lineage-20.0-20261024-UNOFFICIAL-cityman.zip /sdcard/
   adb shell twrp install /sdcard/lineage-20.0-20261024-UNOFFICIAL-cityman.zip
   ```
   You can also tap **Install** in TWRP and pick the zip.
6. **Write the boot image.** TWRP sometimes reboots before the zip has written its boot image, so always write it yourself. If TWRP has already rebooted, start it again with steps 2 and 3 first.
   ```
   adb push boot-20261024-lineage20-sleep-cityman.img /tmp/
   adb shell dd if=/tmp/boot-20261024-lineage20-sleep-cityman.img of=/dev/block/platform/soc.0/f9824900.sdhci/by-name/boot
   ```
7. **Reboot:**
   ```
   adb reboot
   ```
   The first boot of Android 13 takes a few minutes.

### If something goes wrong

- **`adb` doesn't see the phone after a failed boot:** hold **Power + Volume Down** for about 10 seconds to force a restart. Run `adb reboot bootloader` as soon as `adb devices` lists the phone, then carry on from step 3.
- **The phone restarts in a loop, or shows the Windows logo or a sad face:** use the installer's **Phone won't start** option (`installer.bat`, option 4). See the [installer's troubleshooting table](https://github.com/shrimpapplepro/installer-cityman#if-something-goes-wrong).
- **No sound, or TWRP is gone:** use the installer's **Repair sound / signal** option (`installer.bat`, option 3). Your apps and files are kept.

### Going back to Android 11

Start TWRP (steps 1 to 3) and **Format Data**. Then install `lineage-18.1-20261023-UNOFFICIAL-cityman.zip` as in step 5, write `boot-20261023-cityman.img` as in step 6, and reboot. Both files are on the [Android 11 release](https://github.com/shrimpapplepro/android_device_msft_cityman/releases/tag/20261023-charging). Going back erases your data. We haven't tried this direction on our phone yet.

### Building Android 13 yourself

Build steps, the kernel and device changes Android 13 needed, and the patches outside this repo are in the [`lineage-20.0` branch README](https://github.com/shrimpapplepro/android_device_msft_cityman/tree/lineage-20.0#readme).

---

## LineageOS 18.1 (Android 11) Port Status

Device tree and hardware support for the Microsoft Lumia 950 XL (`cityman`) powered by Qualcomm Snapdragon 810 (MSM8994).

> **This fork (`lineage-18.1-camera`)** adds a working rear camera with autofocus and fixes audio on top of shadowofleaf96's port. Download and install guide: [Releases](https://github.com/shrimpapplepro/android_device_msft_cityman/releases).

---

### Tested on this build

- **Rear camera (Sony IMX230)**: preview and full-resolution 5344x4016 photos in Snap, via the OnePlus 2 MSM8994 camera stack and an IMX230 adapter.
- **Rear autofocus**: tap-to-focus, through the module's ROHM BU63165 lens driver. Tested close-up, ~0.5 m and far. No flash.
- **Loudspeaker audio**: media playback through the rear speaker.
- **Wi-Fi** and **Bluetooth**: on and connecting.
- **USB**: ADB and charging.

### Reported working upstream (not yet re-tested on this build)

The list below comes from shadowofleaf96's original README. We haven't verified these items ourselves, so treat them as unconfirmed.

- **Cellular Network & RIL**:
  - Full carrier registration (LTE / 3G / 2G) on live networks.
  - Bidirectional voice calls with Circuit-Switched Fallback (CSFB).
  - SMS send and receive.
  - Interactive USSD sessions (e.g. balance check, service menus).
  - Mobile data connectivity.
  - Modem subsystem stability secured via `libril_wrapper.so` and `lumia-modem-init` watchdog.
- **Call Audio Routing**:
  - Earpiece (Handset) receiver via WCD9330 TomTom codec (`SLIMBUS_0_RX`) with dual digital mic uplink (`voice-dmic-ef`).
  - Loudspeaker (Speakerphone) via Texas Instruments TAS2553 smart amplifier on `QUAT_MI2S_RX` with `voice-speaker-dmic-ef`.
  - Dynamic in-call switching between Earpiece and Speakerphone.
  - Wired Headset / Earphones call audio and microphone.
  - Bluetooth SCO (Voice calling over Bluetooth headsets and car kits, 8kHz NB & 16kHz WB).
  - In-call dialpad DTMF tones and ringback audio.
- **Audio Playback & Recording**:
  - **Dual-Speaker True Stereo**: High-fidelity stereo playback utilizing both the front earpiece (Left channel on WCD9330 codec / `SLIMBUS_0_RX`) and the rear loudspeaker (Right channel on TI TAS2552 smart amplifier / `QUAT_MI2S_RX`).
  - Distortion-free playback tuning with dedicated Right-channel routing and calibrated gain (eliminating mono clipping and crackling).
  - Dynamic stereo orientation reversal (`speaker-reverse`) in landscape mode.
  - 3.5mm Headphone Jack audio playback and headset microphone recording.
  - Four calibrated digital microphones (`dmic1`-`dmic4`) for ambient and directional capture.
  - Bluetooth A2DP audio streaming with `hwbinder` HAL transport.
- **Display & Touch**:
  - 5.7" WQHD AMOLED display (1440x2560) with hardware composer.
  - Capacitive multi-touch screen with wakeup support.
  - Brightness control and proximity blanking during calls.
- **Connectivity & Peripherals**:
  - Wi-Fi 802.11 a/b/g/n/ac (dual-band 2.4GHz and 5GHz).
  - Bluetooth 4.1 (Qualcomm Rome SoC).
  - NFC (NXP PN547 stack).
  - USB Type-C MTP file transfer and Rooted ADB debugging.
- **Power & Battery**:
  - 13W USB Fast Charging with safety timer protections.
  - Precise battery fuel gauge reporting for Microsoft BV-T4D 3340mAh battery via PMI8994.
  - Energy-aware power tuning: `schedutil` governor, restored CPU retention/L2 idle states, and optimized interactive boost durations (100ms) to ensure cool idle thermals (~34°C) and extended battery life.
  - Resolved `android.hardware.audio.service` runaway 100% CPU lockup and severe thermal drain.
- **Sensors**:
  - Accelerometer, Gyroscope, Magnetometer.
  - Ambient Light Sensor, Proximity Sensor.
  - Barometer / Pressure Sensor, Temperature Sensor.

---

### Needs Testing

- Location / GPS satellite lock accuracy.
- Hardware video encoding/decoding performance with high-bitrate HEVC/AVC.

---

### Not Working / In Progress

- **Camera & Imaging**:
  - Front-Facing Camera: Not working (the module did not answer on any camera bus on our test unit).
  - Iris Scanner: Not working.
- **Flashlight / Torch**: Not working on the 950 XL. The flash is an ams AS3638 LED driver on the camera I2C bus (CCI0, 0x30), enabled by GPIO 8; GPIO 12 (`led:flash_torch`) is its torch input. The chip answers and accepts the Windows driver's own torch and flash sequences, but on our test unit its LED output never switches on, so no light. It may be a fault on our unit; it hasn't been tried on a second phone.
- **Modem stability**: on our unit the modem crashes and restarts now and then (coex assert), so cellular may be unreliable.
- **VoLTE / VoWiFi**: Carrier-specific IMS profile customization (standard Circuit-Switched Fallback handles 2G/3G voice calls reliably).

---

## Credits & Acknowledgments

- **shadowofleaf96**: the cityman LineageOS 18.1 port (device, kernel, vendor) and the original installer.
- **EpicLPer**: Heavy lifting in porting the TAS2552/3 speaker amplifier driver, Quaternary MI2S layout, and initial Sensors/NFC research for the Lumia 950 series.
- **LineageOS & CAF**: Base Android 11 and Android 13 bringup and MSM8994 audio/display HAL infrastructure.
- **LineageOS-UL**: the lineage-20.0 manifest with the no-BPF patches that let Android 13 run on old kernels.
- **LumiaWOA & Lumia Community**: Hardware documentation, ACPI tables, and schematics.
