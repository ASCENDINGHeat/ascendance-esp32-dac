# High-Fidelity USB DAC & Audio DSP Player (Schematic 2: DSP+DAC+SCREEN)

**Hardware Platform:** YD-ESP32-S3 (N16R8) + GY-PCM5102A 32-bit DAC + TDA1308 Headphone Amp + SSD1306 OLED + EC11 Rotary Encoder + WS2812B aRGB LED  
**Software Framework:** ESP-IDF v6.1 (C / FreeRTOS)  
**Audio Pipeline:** Native USB OTG (UAC 1.0) / Audio DSP Synth $\rightarrow$ Dynamic Parametric Equalizer (1–10 Bands) $\rightarrow$ 15 ms Click-Free Crossfader $\rightarrow$ Hard Saturation Clamping $\rightarrow$ Master I2S0 $\rightarrow$ PCM5102A $\rightarrow$ TDA1308 $\rightarrow$ 3.5mm Stereo Headphone Output  
**Control Interface:** Onboard EC11 Rotary Encoder + Mode Button + WS2812B Loudness & Mute LED + Driverless USB WebHID Live Tuner (Chrome / Edge)  

---

## 1. System Overview & Architecture

This project turns the **YD-ESP32-S3** microcontroller into a standalone, audiophile-grade **USB DAC and Audio DSP Player** based on **Schematic 2 (`docs/schematics/DSP+DAC+SCREEN.svg`)**. It decodes digital audio in real-time, processes it through a **dynamic 1-to-10 band Parametric Equalizer (PEQ)**, and outputs bit-perfect 16-bit stereo audio over I2S to an external **TI PCM5102A 32-bit DAC**, followed by an **NXP/Philips TDA1308 Class-AB headphone driver**.

*(Note: Radio streaming has been omitted from firmware features in accordance with project requirements).*

```
  ┌─────────────────────────────────────────────────────────────────────────────┐
  │ HOST PC / MAC / ANDROID PHONE                                               │
  │   • Operating System Audio (USB Audio Class 1.0, 16-bit / 44.1 & 48 kHz)    │
  │   • Google Chrome / Microsoft Edge Browser (WebHID API: navigator.hid)      │
  └──────────────────────────────────────┬──────────────────────────────────────┘
                                         │ Single USB Type-C Cable
                                         ▼
  ┌─────────────────────────────────────────────────────────────────────────────┐
  │ YD-ESP32-S3 NATIVE USB OTG PORT (GPIO 19: D-, GPIO 20: D+)                  │
  │                                                                             │
  │   ┌───────────────────────────────────┬─────────────────────────────────┐   │
  │   │ INTERFACE 0 & 1: USB AUDIO (UAC)  │ INTERFACE 2: VENDOR USB HID     │   │
  │   │ • Standard UAC 1.0 Isochronous    │ • Custom 64-byte Raw Reports    │   │
  │   │ • 16-bit Stereo PCM Playback      │ • WebHID Compatible (Driverless)│   │
  │   │ • Bit-Perfect Low-Jitter Audio    │ • Live 60 FPS PEQ Tuning        │   │
  │   └─────────────────┬─────────────────┴─────────────────┬───────────────┘   │
  │                     │ Audio Frames (1 ms)               │ Tuning & Control  │
  │                     ▼                                   ▼                   │
  │   ┌───────────────────────────────────┐ ┌───────────────────────────────┐   │
  │   │ CORE 0: CONTROL & WEBHID TASK     │ │ CORE 0: OLED, ENCODER & LED   │   │
  │   │ • TinyUSB Event Dispatcher        │ │ • SSD1306 128x64 Graphics     │   │
  │   │ • Volatile Parameter Manager      │ │ • EC11 Hardware PCNT Decoder  │   │
  │   │ • Explicit NVS Save Engine        │ │ • WS2812B Volume Loudness RMT │   │
  │   └─────────────────┬─────────────────┘ └───────────────────────────────┘   │
  │                     │ Atomic Lock-Free Swap                                 │
  │                     ▼                                                       │
  │   ┌─────────────────────────────────────────────────────────────────────┐   │
  │   │ CORE 1: REAL-TIME DSP AUDIO TASK                                    │   │
  │   │ • Dynamic Parametric EQ Engine (1 to 10 Cascaded Biquad Bands)      │   │
  │   │ • Dynamic CPU Scaling: 3 Bands (~0.66 MIPS) to 10 Bands (~2.2 MIPS) │   │
  │   │ • 15 ms Double-Buffered Linear Crossfader (Zero Clicks/Dropouts)    │   │
  │   │ • Hard Saturation Clamping [-32768, 32767]                          │   │
  │   │ • Uninterrupted Master I2S0 DMA Transmission                        │   │
  │   └─────────────────┬───────────────────────────────────────────────────┘   │
  └─────────────────────┼───────────────────────────────────────────────────────┘
                        │
                        │ I2S: GPIO 4 (WS/LCK), GPIO 5 (DOUT/DIN), GPIO 6 (BCK), GPIO 7 (Mute/XMT)
                        ▼
  ┌────────────────────────────────────────────────────────┐
  │ TI GY-PCM5102A 32-bit / 384 kHz Stereo Audio DAC       │
  │   • DirectPath™ ground-centered line-level output      │
  │   • Internal PLL synthesized from BCK (SCK tied to GND)│
  └─────────────────────┬──────────────────────────────────┘
                        │ Analog Line Out (L, R, AGND)
                        ▼
  ┌────────────────────────────────────────────────────────┐
  │ NXP/Philips TDA1308 Class-AB Stereo Headphone Amp      │
  │   • 80 mW output into 32 Ω loads (16 Ω - 600 Ω support)│
  └─────────────────────┬──────────────────────────────────┘
                        │
                        ▼
          [3.5mm Stereo Headphone Jack J3]
```

---

## 2. Dynamic Parametric Equalizer (PEQ) DSP Engine

The DSP engine implements a high-resolution, cascaded Direct Form II Transposed Biquad IIR filter bank based on the **Robert Bristow-Johnson (RBJ) Audio EQ Cookbook**.

### 2.1 Dynamic Band Count Scaling (1 to 10 Bands)
The active band count is fully dynamic, scaling from **1 to 10 bands** (`PEQ_MAX_BANDS = 10`, default `PEQ_DEFAULT_NUM_BANDS = 5`). Core 1 executes computation strictly over active bands, drastically reducing CPU consumption when fewer bands are needed:

| Active Band Count | Use Case Profile | Xtensa LX7 Biquad Ops / Sample | MIPS @ 44.1 kHz Stereo | Core 1 CPU Utilization (%) |
| :---: | :--- | :---: | :---: | :---: |
| **1 Band** | Subsonic HPF / Single Notch Filter | 2 biquads (L/R) | ~0.22 MIPS | **~0.4%** |
| **3 Bands** | Classic Tone Controls (Bass, Mid, Treble) | 6 biquads (L/R) | ~0.66 MIPS | **~1.2%** |
| **5 Bands** | Standard Audiophile PEQ (Default) | 10 biquads (L/R) | ~1.10 MIPS | **~2.0%** |
| **7 Bands** | Extended Acoustic Tuning & Room Correction | 14 biquads (L/R) | ~1.54 MIPS | **~2.8%** |
| **10 Bands** | Pro Mastering & Calibrated AutoEQ Targets | 20 biquads (L/R) | ~2.20 MIPS | **~3.8%** |

### 2.2 DSP Features & Audio Safety Invariants
1. **15 ms Click-Free Crossfading:** Seamlessly blends between coefficient sets and band count changes across a double-buffered transition window, eliminating zipper noise and audible clicks.
2. **Hard Saturation Clamping:** Clamps all floating-point audio to `[-32768, 32767]` before integer conversion, preventing deafening integer wrap-around.
3. **Pre-Gain Headroom Management:** Configurable pre-attenuation slider (0.0 to -12.0 dB) guarantees digital clipping immunity during high positive EQ boosts.
4. **Built-in Presets:**
   * **Preset 0 (Reference Flat):** Bit-perfect bypass (0 dB gain across all frequencies).
   * **Preset 1 (Warm Bass Boost):** +4.5 dB Low Shelf @ 105 Hz (Q = 0.71).
   * **Preset 2 (Harman OE2018 Target):** Multi-band target curve matching standard headphone acoustic profiles.
   * **Preset 3 (User Custom):** Full custom configuration stored in persistent NVS flash.

---

## 3. Driverless USB WebHID Live Tuner (FiiO JA11 Style)

Commercial audiophile dongles like the **FiiO JA11** and **Moondrop FreeDSP** tune their onboard DSP over the same USB-C cable that streams audio. This project provides a production, driverless WebHID tuner web app in [`tools/webhid_tuner/index.html`](tools/webhid_tuner/index.html).

### 3.1 WebHID Capabilities
* **100% Offline & Driverless:** Operates directly inside Google Chrome, Microsoft Edge, Opera, and Android Chrome using the standard WebHID API (`navigator.hid`). No software, drivers, or Wi-Fi required.
* **60 FPS Live Slider Dragging:** Parameter updates are streamed over 64-byte raw HID packets in $<1\text{ ms}$, modifying volatile SRAM only.
* **Absolute Zero Flash Wear During Auditioning:** Moving sliders does **not** write to flash memory. SPI flash sectors are protected from wear-out.
* **Dynamic Band Count Selector:** Seamlessly switch between **3-Band (Tone)**, **5-Band (Standard)**, **7-Band (Extended)**, and **10-Band (Pro Mastering)** modes with automatic ISO frequency allocation.
* **Custom Value (Within Bound) Numeric Inputs:** Precision number input boxes for every band parameter and preamp headroom with live bidirectional binding alongside range sliders:
  * **Frequency:** 20 Hz to 20,000 Hz (1 Hz resolution)
  * **Gain:** -24.0 dB to +12.0 dB (0.1 dB resolution)
  * **Q Factor:** 0.10 to 10.00 (0.01 resolution)
  * **Preamp Headroom:** -12.0 dB to 0.0 dB (0.1 dB resolution)
  * Automatic boundary clamping (`clamp(val, min, max)`) on blur/Enter with CSS `:invalid` feedback.
* **Bidirectional Synchronization:** Firmware transmits the active band count in byte 16 (`reserved[0]`) of the HID report. The browser UI automatically adapts to match the hardware.
* **Explicit Save to NVS:** Commits active settings to persistent flash only when clicking `[💾 Save to Hardware NVS]`, verified by a 32-bit CRC32 checksum and `memcmp()` delta check.

---

## 4. Hardware Bill of Materials (BOM) & Pinout Table

### 4.1 Bill of Materials (Schematic 2: `DSP+DAC+SCREEN.svg`)

| Component | Function / Specification | Notes |
| :--- | :--- | :--- |
| **YD-ESP32-S3 (N16R8)** | Main Controller (Xtensa dual-core LX7 @ 240 MHz, 16MB Flash, 8MB Octal PSRAM) | Dual USB-C ports (USB OTG + CH343/CP2102 UART) |
| **GY-PCM5102A Module** | 32-bit / 384 kHz Stereo Audio DAC (TI PCM5102A) | DirectPath™ ground-centered output (no DC blocking caps required) |
| **TDA1308 Module** | Class-AB Stereo Headphone Amplifier (NXP/Philips TDA1308) | Drives 16 $\Omega$ – 600 $\Omega$ loads, 80 mW into 32 $\Omega$ |
| **SSD1306 0.96" OLED** | 128x64 Monochrome I2C Display (Address `0x3C`) | Displays volume, preset, sample rate, menu, and digit-by-digit EQ editor |
| **EC11 Rotary Encoder** | Quadrature rotary encoder with integrated push switch | Default: Volume / Mute; Menu: Scroll / Select; EQ Adjust: Digit (0-9) adjust & jump sections |
| **Tactile Pushbutton 1**| Navigation Button 1 / SW6 (GPIO 18) | Default: Open Menu; Menu: Exit / Back; EQ Adjust: Move cursor Left (◄) / Cancel (Long) |
| **Tactile Pushbutton 2**| Navigation Button 2 / SW_NAV2 (GPIO 10) | Default: Cycle EQ Preset; Menu: Down; EQ Adjust: Move cursor Right (►) / Add Band |
| **WS2812B RGB LED** | Onboard aRGB Smart LED | Real-time volume loudness brightness & mute alert indicator |

### 4.2 Complete Master Pinout Table (Schematic 2)

| From (Source) | Pin Label | To (Destination) | Pin Label | Wire Function / Notes |
| :--- | :--- | :--- | :--- | :--- |
| **ESP32-S3** | **GPIO 4** | **PCM5102A** | **LCK** | I2S Word Select / Left-Right Clock (WS) |
| **ESP32-S3** | **GPIO 5** | **PCM5102A** | **DIN** | I2S Serial Audio Data Out (DOUT) |
| **ESP32-S3** | **GPIO 6** | **PCM5102A** | **BCK** | I2S Continuous Bit Clock (BCLK) |
| **ESP32-S3** | **GPIO 7** | **PCM5102A** | **XMT** | Soft Mute Control (HIGH = Unmute/Play, LOW = Soft Mute) |
| **ESP32-S3** | **GPIO 8** | **OLED1** | **SDA** | I2C Data Line (SSD1306 @ `0x3C`) |
| **ESP32-S3** | **GPIO 9** | **OLED1** | **SCL** | I2C Clock Line (SSD1306 @ `0x3C`) |
| **ESP32-S3** | **GPIO 10** | **SW_NAV2** | **Pin 1** | Navigation Button 2 / Right (Active-LOW, internal pull-up) |
| **ESP32-S3** | **GPIO 15** | **EC11 / SW2** | **A** | Rotary Encoder Phase A (PCNT unit, 10k hardware pull-up) |
| **ESP32-S3** | **GPIO 16** | **EC11 / SW2** | **B** | Rotary Encoder Phase B (PCNT unit, 10k hardware pull-up) |
| **ESP32-S3** | **GPIO 17** | **EC11 / SW2** | **SW** | Rotary Encoder Push Switch (Active-LOW, 10k hardware pull-up) |
| **ESP32-S3** | **GPIO 18** | **SW6** | **Pin 1** | Navigation Button 1 / Left (Active-LOW, 10k hardware pull-up) |
| **ESP32-S3** | **GPIO 19** | **USB Port** | **D-** | Native USB OTG Full-Speed Data Minus |
| **ESP32-S3** | **GPIO 20** | **USB Port** | **D+** | Native USB OTG Full-Speed Data Plus |
| **ESP32-S3** | **GPIO 48** | **Onboard LED** | **DIN** | WS2812B aRGB LED (RMT driver; volume loudness & mute indicator) |
| **ESP32-S3** | **5V (VIN)** | **PCM5102A** | **VCC** | DAC Power supply (module has onboard 3.3V LDO) |
| **ESP32-S3** | **5V (VIN)** | **TDA1308** | **VCC** | Amp Power supply (3V to 6V operating range) |
| **ESP32-S3** | **3.3V** | **OLED1** | **VCC** | Display Power supply (3.3V DC) |
| **ESP32-S3** | **GND** | **System** | **GND** | System Common Ground |
| **PCM5102A** | **L** | **TDA1308** | **IN_L** | Left Channel analog line-level signal |
| **PCM5102A** | **R** | **TDA1308** | **IN_R** | Right Channel analog line-level signal |
| **PCM5102A** | **G** | **TDA1308** | **GND** | Analog Ground reference |
| **TDA1308** | **OUT_L/R/G** | **Jack J3** | **Tip/Ring/Sleeve** | 3.5mm Stereo Headphone Output |

### 4.3 GY-PCM5102A Hardware Jumper Configuration
To allow the DAC's internal PLL to synthesize the master clock (MCLK) directly from BCK:
* **`SCK` $\rightarrow$ Connect to `GND`:** Enables internal PLL mode.
* **`FMT` $\rightarrow$ Connect to `GND`:** Selects standard Philips I2S format.
* **`FLT` $\rightarrow$ Connect to `GND`:** Normal latency FIR filter mode (optimal audio quality).
* **`DMP` $\rightarrow$ Connect to `GND`:** De-emphasis off.
* **`XMT` $\rightarrow$ Connect to `GPIO 7`:** Soft mute control (managed automatically by `audio_source_arbiter`).

---

## 5. Build, Flash & Usage Guide

### 5.1 Prerequisites & Toolchain
* ESP-IDF v6.1 (or v5.x) installed and configured in your environment.
* Modern Chromium-based browser (Google Chrome, Microsoft Edge, Opera, or Android Chrome) for WebHID tuning.

### 5.2 Build Firmware
```bash
# Set target to ESP32-S3
idf.py set-target esp32s3

# Build the firmware
idf.py build
```

### 5.3 Flash Firmware & Open Serial Monitor
Connect the YD-ESP32-S3 via the **COM Port** (CH343 / CP2102 UART):
```bash
idf.py -p /dev/ttyACM0 flash monitor
```
*(Press `Ctrl + ]` to exit the serial monitor).*

### 5.4 Using the WebHID Live Tuner
1. Connect the YD-ESP32-S3 **Native USB port** (GPIO 19/20) to your PC, Mac, or Android device.
2. Open [`tools/webhid_tuner/index.html`](tools/webhid_tuner/index.html) in Google Chrome or Microsoft Edge.
3. Click **"Connect USB DAC"** and select the **ESP32-S3 Hi-Fi DSP DAC** from the browser popup.
4. Tune in real time (60 FPS volatile SRAM auditioning, zero flash wear):
   * **Drag Control Nodes:** Directly drag numbered circles on the frequency response graph ($X$ = Freq, $Y$ = Gain).
   * **Mouse Wheel / Trackpad Scroll:** Scroll over any node to adjust its Q Factor ($0.10$ to $10.00$) with dynamic $-3\text{ dB}$ bandwidth guides.
   * **Double-Click:** Double-click a node to reset its gain to 0.0 dB; double-click empty space to create a new band.
   * **Dynamic Bands (1 to 10):** Add custom bands via `+ Add Band`, remove via `✕`, or enter an arbitrary count in the stepper.
   * **Type Precise Values:** Input custom values within safe bounds for Freq, Gain, Q, and Preamp.
5. Click **"💾 Save to Hardware NVS"** to persist your profile into non-volatile flash.

---

## 6. Detailed Documentation Reference

The project documentation is organized into six tiers. Visit the **[Master Documentation Portal](docs/README.md)** for the complete index and architectural guides:

* **[Master Documentation Portal](docs/README.md):** Central documentation directory, 6-tier taxonomy, and hardware quick-reference.
* **[Current Firmware Implementation Record](docs/firmware/current_firmware_implementation.md):** Consolidated technical record of active production firmware, UAC 1.0 descriptors, PEQ engine, and build/flash specifications.
* **[Software Architecture & Implementation](docs/firmware/software.md):** Subsystem architecture, ESP-IDF v6.1 `driver/i2s_std.h`, FreeRTOS dual-core task layout, and developer invariants.
* **[Hardware & Wiring Guide](docs/hardware/hardware.md):** BOM, full pinout table, GY-PCM5102A jumper settings, TDA1308 amp integration, and star-grounding practices.
* **[Hardware Schematics Guide](docs/hardware/schematics.md):** Schematic 2 (`DSP+DAC+SCREEN.svg`), vector schematics, netlists, and sign-off checklists.
* **[USB Audio Class (UAC) DAC Guide](docs/guides/usb_dac_guide.md):** TinyUSB UAC 1.0 descriptors, isochronous ringbuffer sync, WebHID packet schemas, and live tuning architecture.
* **[Parametric Equalizer (PEQ) DSP Engine Guide](docs/guides/parametric_eq_guide.md):** RBJ biquad mathematics, dynamic band count scaling (1–10 bands), CPU scaling table, click-free crossfader, and NVS persistence.
* **[Future Enhancements & Architectural Roadmap](docs/firmware/future_enhancements.md):** Architectural blueprints for Channel Matrix Mixer & Headphone Crossfeed, 24-bit/96kHz pipeline, dual-rate reclocking, OLED response curves, VU meters, and PWA offline tooling.
* **[User Interface, Display & Controls Guide](docs/guides/ui_and_controls_guide.md):** SSD1306 128x64 OLED graphics driver, hardware PCNT rotary decoding, and UI state machine.
* **[Multi-Mode Audio Switching & Handover Guide](docs/guides/mode_switching_guide.md):** Central `AudioSourceManager` arbiter, 4-phase zero-pop hardware muting protocol (`GPIO 7: XMT`), and tone/sweep generators.
* **[Troubleshooting & Root Cause Analysis](docs/guides/troubleshooting.md):** Deep post-mortems for 1 kHz USB whine elimination, ground loops, stack overflows, and brownout resets.
* **[Master Agentic AI Developer Reference](docs/reference/agentic_ai_reference.md):** The 10 Golden Invariants, master pinouts, task affinities, and automated triage trees for AI agents.
* **[Per-Function Software Specifications](docs/software_functions/README.md):** Function-by-function reference for autonomous coding agents with exact C99 prototypes and memory boundaries.
