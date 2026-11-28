# Project ASCENDANCE — Hi-Fi USB DAC & DSP

[![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v6.1%20%7C%20v5.x-blue.svg)](https://idf.espressif.com/)
[![Target MCU](https://img.shields.io/badge/MCU-ESP32--S3%20(N16R8)-green.svg)](https://www.espressif.com/en/products/socs/esp32-s3)
[![DAC](https://img.shields.io/badge/DAC-TI%20PCM5102A%20(32--bit%2F384kHz)-orange.svg)](https://www.ti.com/product/PCM5102A)
[![Headphone Amp](https://img.shields.io/badge/Amp-NXP%20TDA1308%20Class--AB-purple.svg)](https://www.nxp.com)
[![WebHID](https://img.shields.io/badge/WebHID-Driverless%20Live%20Tuning-cyan.svg)](tools/webhid_tuner/index.html)
[![Android](https://img.shields.io/badge/Android-USB%20Companion-3ddc84.svg)](tools/android_companion/)
[![License](https://img.shields.io/badge/License-Apache%202.0-yellow.svg)](LICENSE)

**ASCENDANCE** turns an **ESP32-S3 (N16R8)**, **TI PCM5102A DAC**, and **NXP TDA1308 Class-AB headphone amp** into a driverless **24-bit / 44.1–48 kHz USB DAC** with real-time Dual-Mono DSP, a **128×64 OLED display**, rotary controls, a **desktop WebHID tuner**, and a **native Android Companion app**.

---

## Key Features

- **Driverless Dual-Rate USB Audio:** Plug-and-play Asynchronous USB Audio Class 1.0 (**24-bit / 44.1 & 48.0 kHz** stereo, `294`-byte isochronous OUT with I2S-DMA-coupled 10.14 PI+EMA feedback fitting ESP32-S3's `1,024`-byte DWC2 FIFO) over a single USB-C cable alongside live tuning controls.
- **Dual-Mono 10-Band Parametric EQ & Auto-Preamp:** Dynamically scales **1–10 bands** per channel (Stereo Linked or independent Left/Right) across 8 filter types (`Peaking`, `Low Shelf`, `High Shelf`, `High Pass`, `Low Pass`, `Notch`, `Band Pass`, `Bypass`) with **15 ms click-free crossfading**, Preamp headroom (`0` to `-12 dB`) + automatic **Auto-Preamp Headroom Guard**, and instant level-matched **A/B Bypass**.
- **8 Custom DAC Presets (`Slot 1`–`Slot 8`):** Editable, user-named hardware presets (`16`-char custom names + factory reset + optional `Flat`, `Bass`, `Harman` templates), stored persistently on-device with zero flash wear during live **Preview Live** slider tuning.
- **Bass Enhancer, Dynamic Bass & Harmonic Exciter:** Global PSRAM-backed (`4096`-pt LUTs + `2×` polyphase half-band oversampling) Sub-Bass harmonic synthesis (`50–150 Hz`), envelope-controlled **Dynamic Bass** (`90 Hz` sub-bass boost that smoothly ducks on loud hits), and **Tube Warmth / Tape / Air Sheen** Exciter (`1.5–8.0 kHz`).
- **Channel Matrix & 4-Level BS2B Crossfeed:** Balance, M/S Stereo Width (`0–200%`), Mono Downmix, L/R Swap, **Custom Channel Matrix**, and 4-level tonal-neutral **BS2B Headphone Crossfeed** (`Off` / `Chu Moy` / `Jan Meier` / `J. Meier`).
- **Dynamics, Output Purity & FIR Impulse Filter:** **True-Peak Limiter** (`-0.5 dBFS` ceiling, `1.5 ms` lookahead), volume-coupled **Equal Loudness (ISO 226)** (`Off / ISO Mild / ISO Full`), **18 Hz Subsonic / DC Blocker**, **24-Bit TPDF Dither**, selectable **Logarithmic dB Volume Taper (`-60..0 dB`)**, and hardware-accelerated **FIR Impulse Filter** (`.txt` / `.csv` / `.wav`, up to `256` taps/ch @ `44.1 / 48 kHz`).
- **5 OLED Screen Views & Smart Power:** SSD1306 `128×64` display (`0x3C`/`0x3D` auto-probe) with 5 cycleable **OLED Screen Views** (`Classic`, `EQ Curve`, `VU Meters` post-DSP `-60..0 dBFS`, `Split`, and live **32-Band 1024-pt FFT Spectrum** with peak-hold), **OLED Auto-Dim & Sleep**, **Auto-Mute on Silence**, USB suspend low-power mode, and 4-phase zero-pop muting.

---

## Architecture at a Glance

```text
  Host PC / Mac / Android  ──(USB Audio 24-bit 44.1/48 kHz + USB Control)──► ESP32-S3
     ├─ Core 0: USB Stack, OLED UI & 1024-pt FFT (10 FPS), Rotary/Buttons, Preset Storage
     └─ Core 1: Dual-Mono 10-Band EQ + Loudness ─► FIR ─► Harmonics/DynBass ─► Matrix ─► 18 Hz HPF ─► Limiter/TPDF ─► I2S (32-bit)
                                                                                                                           │
                                                                    3.5 mm Headphones ◄── TDA1308 Amp ◄── PCM5102A DAC ◄──┘
```

---

## Tuning & Control Interfaces

### 1. On-Device Controls & OLED (`ASCENDANCE Hi-Fi`)
| Control | Playback Screen | Main Menu (17 Items) | Live EQ Adjust |
| :--- | :--- | :--- | :--- |
| **Rotary Turn** | Master volume (`±2%`) | Scroll menu items | Adjust selected digit / option |
| **Rotary Click** | Mute / Unmute (Double-click: **Save to DAC**, Long press: cycle **Input**) | Select / Cycle setting (Long press: **Save to DAC**) | Jump parameter (`Band → Freq → Gain → Q → Toolbar`) |
| **Button 1 (◄)** | Open Main Menu (Long press: prev **Slot**) | Exit Menu | Move cursor Left (Double-click: remove band, Long press: revert) |
| **Button 2 (►)** | Next **Slot** (Double-click: cycle **OLED Screen View**) | Move Down | Move cursor Right (Double-click: add band) |

Main Menu items: `1. Live EQ Adjust` · `2. Slot` · `3. A/B` · `4. Crossfeed (Off/CM/JM/JM+)` · `5. Loudness` · `6. Limiter` · `7. View (Classic/Curve/VU/Split/Spectrum)` · `8. Bass Enh` · `9. DynBass` · `10. Exciter (Off/Tube/Tape/Air)` · `11. Preamp` · `12. AutoPreamp` · `13. VolTaper (Std/Log)` · `14. Subsonic` · `15. Input` · `16. Save to DAC` · `17. Exit Menu`

### 2. WebHID Live Tuner (`tools/webhid_tuner/`)
Modular React + Vite app bundled into a single offline-ready HTML file (`dist/index.html`) for Chrome, Edge, and Opera:
1. Build once locally (`cd tools/webhid_tuner && npm install && npm run build`) or download `ascendance-webhid-tuner-<tag>.html` from Releases.
2. Open `tools/webhid_tuner/dist/index.html` (or run `npm run dev`), connect the **Native USB-C port**, click **Connect USB DAC**, and select **ASCENDANCE Hi-Fi DAC**.
3. Tune live (`Preview Live`), load **Templates** (`Flat` / `Bass Boost` / `Harman`) or AutoEQ/APO `.txt` / `.json` / FIR `.wav` files, adjust **Bass Enhancer**, **Dynamic Bass**, **Harmonic Exciter**, **BS2B Crossfeed**, **Subsonic / TPDF Dither**, and **Volume Taper**, then click **Save to DAC** to store permanently.

<details>
<summary>Linux WebHID udev permission rule (one-time setup)</summary>

```bash
sudo bash -c 'cat << "EOF" > /etc/udev/rules.d/99-esp-webhid.rules
SUBSYSTEM=="hidraw", ATTRS{idVendor}=="303a", ATTRS{idProduct}=="4002", MODE="0666"
EOF'
sudo udevadm control --reload-rules && sudo udevadm trigger
```
</details>

### 3. Android Companion App (`tools/android_companion/`)
Native Kotlin + Jetpack Compose app (`com.ascendance.dac`, Android 8.0+) for mobile USB-C control without interrupting Spotify, Tidal, or system audio:
- **Top Bar & Quick Controls:** Tappable active **Slot 1–8** dropdown switcher from any tab, instant **A/B Bypass**, and **Save** with an amber unsaved-changes indicator plus tactile haptic feedback.
- **EQ:** Touch/pinch Frequency Response / Phase / Group Delay canvas (focused on the EQ tab for maximum vertical breathing room elsewhere), Dual-Mono L/R tabs, and Preamp Headroom.
- **Tone:** Modular cards for **Bass Enhancer, Dynamic Bass & Harmonic Exciter**, **True-Peak Limiter, Auto-Preamp, Subsonic 18 Hz, 24-Bit TPDF Dither & Equal Loudness (ISO 226)**, **Channel Matrix & 4-Level BS2B Crossfeed** (with collapsible **Custom 2×2 Matrix**), and collapsible **FIR Impulse Filter** upload (`.txt` / `.wav`).
- **Presets:** **DAC Presets (1–8)** with custom naming, **Template** (`Flat` / `Bass` / `Harman`), **Preview Live** vs **Save to DAC**, AutoEQ/APO `.txt`/`.json` import/export, and unlimited local phone profiles.
- **Device:** **USB Connection** / **Sync from DAC**, **Offline Preview Mode**, **OLED Screen View** (`Classic` / `Curve` / `VU` / `Split` / `FFT`), **Volume Taper** (`Standard` / `Log dB`), **Auto-Mute on Silence**, **OLED Auto-Dim & Sleep**, persisted **Appearance** themes (`Dark Studio`, `AMOLED`, `Dynamic` Material You), and **Activity Log**.

---

## Build, Flash & CI

Requires **ESP-IDF v6.1** (or v5.x), CMake 3.22+, Ninja, and Python 3.10+.

```bash
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

* **CI Workflows:** [`.github/workflows/build.yml`](.github/workflows/build.yml) builds the ESP32-S3 firmware (`ASCENDANCE_merged.bin`, `ASCENDANCE.map`, size reports), [`.github/workflows/android.yml`](.github/workflows/android.yml) runs Android unit tests/lint/APK builds, and [`.github/workflows/release.yml`](.github/workflows/release.yml) publishes tagged release assets.

---

## Hardware & Documentation

For wiring diagrams, mandatory solder jumpers (`PCM5102A` `SCK/FMT -> GND`, `YD-ESP32-S3` `IN-OUT` jumper), star-grounding rules, and deep DSP/firmware specs, see **[`docs/README.md`](docs/README.md)** and **[`CHANGELOG.md`](CHANGELOG.md)**:

- **Hardware & Wiring:** [`docs/hardware/hardware.md`](docs/hardware/hardware.md) · [`docs/hardware/schematics.md`](docs/hardware/schematics.md) (`docs/schematics/DSP+DAC+SCREEN.svg`)
- **Firmware & Architecture:** [`docs/firmware/current_firmware_implementation.md`](docs/firmware/current_firmware_implementation.md) · [`docs/firmware/software.md`](docs/firmware/software.md) · [`docs/reference/architecture.md`](docs/reference/architecture.md)
- **Subsystem Guides:** [`USB DAC Guide`](docs/guides/usb_dac_guide.md) · [`Parametric EQ Guide`](docs/guides/parametric_eq_guide.md) · [`UI & Controls Guide`](docs/guides/ui_and_controls_guide.md) · [`Mode Switching Guide`](docs/guides/mode_switching_guide.md) · [`Troubleshooting`](docs/guides/troubleshooting.md)
- **AI / Function Specs:** [`docs/reference/agentic_ai_reference.md`](docs/reference/agentic_ai_reference.md) · [`docs/software_functions/README.md`](docs/software_functions/README.md) · [`docs/firmware/future_enhancements.md`](docs/firmware/future_enhancements.md)
