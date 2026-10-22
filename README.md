# Microsoft Lumia 950 XL (cityman)

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

- **EpicLPer**: Heavy lifting in porting the TAS2552/3 speaker amplifier driver, Quaternary MI2S layout, and initial Sensors/NFC research for the Lumia 950 series.
- **LineageOS & CAF**: Base Android 11 bringup and MSM8994 audio/display HAL infrastructure.
- **LumiaWOA & Lumia Community**: Hardware documentation, ACPI tables, and schematics.
