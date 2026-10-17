# Microsoft Lumia 950 XL (cityman)

## LineageOS 18.1 (Android 11) Port Status

Device tree and hardware support for the Microsoft Lumia 950 XL (`cityman`) powered by Qualcomm Snapdragon 810 (MSM8994).

---

### What is Working

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
- **Flashlight / Torch**:
  - Quick Settings flashlight tile fully operational (toggle on/off).
  - Driven via GPIO 12 (`led:flash_torch` sysfs with fallback) and HAL metadata reporting.
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
  - Rear Camera (20MP PureView Sony IMX230): Sensor initializes and binds to ISP, but viewfinder shows a black preview.
  - Front-Facing Camera: Not working.
  - Iris Scanner: Not working.
- **VoLTE / VoWiFi**: Carrier-specific IMS profile customization (standard Circuit-Switched Fallback handles 2G/3G voice calls reliably).

---

## Credits & Acknowledgments

- **EpicLPer**: Heavy lifting in porting the TAS2552/3 speaker amplifier driver, Quaternary MI2S layout, and initial Sensors/NFC research for the Lumia 950 series.
- **LineageOS & CAF**: Base Android 11 bringup and MSM8994 audio/display HAL infrastructure.
- **LumiaWOA & Lumia Community**: Hardware documentation, ACPI tables, and schematics.
