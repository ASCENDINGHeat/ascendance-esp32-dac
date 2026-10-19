# Project ASCENDANCE: High-Fidelity USB DAC & Audio DSP Player

[![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v6.1%20%7C%20v5.x-blue.svg)](https://idf.espressif.com/)
[![Target MCU](https://img.shields.io/badge/MCU-ESP32--S3%20(N16R8)-green.svg)](https://www.espressif.com/en/products/socs/esp32-s3)
[![DAC](https://img.shields.io/badge/DAC-TI%20PCM5102A%20(32--bit%2F384kHz)-orange.svg)](https://www.ti.com/product/PCM5102A)
[![Headphone Amp](https://img.shields.io/badge/Amp-NXP%20TDA1308%20Class--AB-purple.svg)](https://www.nxp.com)
[![WebHID](https://img.shields.io/badge/WebHID-Driverless%20Live%20Tuning-cyan.svg)](tools/webhid_tuner/index.html)
[![License](https://img.shields.io/badge/License-Apache%202.0-yellow.svg)](LICENSE)

**Production Hardware Baseline:** Schematic 2 (`docs/schematics/DSP+DAC+SCREEN.svg` Rev 1.1)  
**Mainboard:** YD-ESP32-S3 (Xtensa Dual-Core LX7 @ 240 MHz, 16 MB Octal Flash, 8 MB Octal PSRAM)  
**Audio Frontend:** Native USB OTG (USB Audio Class 1.0 Isochronous 48 kHz / 16-bit Stereo)  
**DSP Engine:** Dynamic 1-to-10 Band Parametric Equalizer (Direct Form II Transposed Biquads) + 15 ms Double-Buffered Linear Crossfader + Saturation Clamping  
**Audio Backend:** Master I2S0 DMA $\rightarrow$ Texas Instruments GY-PCM5102A 32-bit DAC $\rightarrow$ NXP/Philips TDA1308 Class-AB Headphone Amplifier $\rightarrow$ 3.5 mm TRS Headphone Jack  
**Display & Control:** 0.96" SSD1306 128×64 Monochrome OLED (I2C @ `0x3C`) + EC11 Quadrature Rotary Encoder (Hardware PCNT) + Dual Navigation Buttons (`SW6` / `SW8`) + Onboard WS2812B aRGB LED (RMT driver)  
**Tuning Interface:** Driverless 60 FPS USB WebHID Live Tuner (`tools/webhid_tuner/index.html`) for Chromium browsers + On-Device Digit-by-Digit PEQ Editor  

---

## 1. System Overview & Architecture

Project **ASCENDANCE** turns the ESP32-S3 into a standalone, audiophile-grade **USB DAC and Audio DSP Processing Station**. It processes incoming digital audio in real-time on a dedicated CPU core, shapes the frequency response using a dynamic 1-to-10 band Parametric Equalizer (PEQ), and outputs bit-perfect stereo audio over I2S to an external **TI PCM5102A 32-bit DAC** paired with an **NXP/Philips TDA1308 Class-AB headphone amplifier**.

### 1.1 Dual-Core Workload Separation

To guarantee continuous, dropout-free audio processing under all operating conditions, tasks are strictly partitioned across the ESP32-S3's dual Xtensa LX7 cores:

* **Core 0 (System, UI & Communication):** Handles the TinyUSB event dispatcher, WebHID parameter reception, hardware PCNT rotary encoder decoding, button state machines, SSD1306 OLED graphics rendering, WS2812B RMT LED updates, and non-volatile flash (NVS) profile persistence.
* **Core 1 (Dedicated Real-Time DSP Audio Pipeline):** Dedicated entirely to audio stream consumption (`tud_audio_read`), real-time Direct Form II Transposed Biquad filtering, 15 ms double-buffered crossfading, float saturation clamping, and continuous I2S DMA transmission.

```
  ┌─────────────────────────────────────────────────────────────────────────────┐
  │ HOST PC / MAC / ANDROID PHONE                                               │
  │   • Operating System Audio (USB Audio Class 1.0, 16-bit / 48.0 kHz Stereo)  │
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
  │   │ • 16-bit Stereo PCM (48.0 kHz)    │ • WebHID Compatible (Driverless)│   │
  │   │ • Bit-Perfect Low-Jitter Audio    │ • Live 60 FPS PEQ Tuning        │   │
  │   └─────────────────┬─────────────────┴─────────────────┬───────────────┘   │
  │                     │ Audio Frames (1 ms)               │ Tuning & Control  │
  │                     ▼                                   ▼                   │
  │   ┌───────────────────────────────────┐ ┌───────────────────────────────┐   │
  │   │ CORE 0: CONTROL & WEBHID TASK     │ │ CORE 0: OLED, ENCODER & LED   │   │
  │   │ • TinyUSB Event Dispatcher        │ │ • SSD1306 128x64 Graphics     │   │
  │   │ • Volatile Parameter Manager      │ │ • EC11 Hardware PCNT Decoder  │   │
  │   │ • Explicit NVS Save Engine        │ │ • WS2812B Loudness & Mute RMT │   │
  │   └─────────────────┬─────────────────┘ └───────────────────────────────┘   │
  │                     │ Atomic Lock-Free Swap                                 │
  │                     ▼                                                       │
  │   ┌─────────────────────────────────────────────────────────────────────┐   │
  │   │ CORE 1: REAL-TIME DSP AUDIO TASK (Priority 10)                      │   │
  │   │ • Dynamic Parametric EQ Engine (1 to 10 Cascaded Biquad Bands)      │   │
  │   │ • Dynamic CPU Scaling: 3 Bands (~0.66 MIPS) to 10 Bands (~2.2 MIPS) │   │
  │   │ • 15 ms Double-Buffered Linear Crossfader (Zero Clicks/Dropouts)    │   │
  │   │ • Hard Saturation Clamping [-32768, 32767]                          │   │
  │   │ • Uninterrupted Master I2S0 DMA Transmission (48 kHz / 16-bit)      │   │
  │   └─────────────────┬───────────────────────────────────────────────────┘   │
  └─────────────────────┼───────────────────────────────────────────────────────┘
                        │
                        │ I2S: GPIO 4 (WS), GPIO 5 (DOUT), GPIO 6 (BCK), GPIO 7 (XMT Mute)
                        ▼
  ┌────────────────────────────────────────────────────────┐
  │ TI GY-PCM5102A 32-bit / 384 kHz Stereo Audio DAC       │
  │   • DirectPath™ ground-centered line-level output      │
  │   • Internal PLL synthesized from BCK (SCK tied to GND)│
  │   • Hardware Soft Mute (Active-LOW on GPIO 7)          │
  └─────────────────────┬──────────────────────────────────┘
                        │ Analog Line Out (L, R, AGND)
                        ▼
  ┌────────────────────────────────────────────────────────┐
  │ NXP/Philips TDA1308 Class-AB Stereo Headphone Amp      │
  │   • 80 mW output into 32 Ω loads (16 Ω - 600 Ω support)│
  │   • Low THD+N (0.03%), high PSRR (90 dB)               │
  └─────────────────────┬──────────────────────────────────┘
                        │
                        ▼
          [3.5mm Stereo Headphone Jack J3]
```

---

## 2. Dynamic Parametric Equalizer (PEQ) DSP Engine

The DSP engine implements a high-resolution, cascaded **Direct Form II Transposed Biquad IIR filter bank** based on the **Robert Bristow-Johnson (RBJ) Audio EQ Cookbook**.

### 2.1 Dynamic Band Count Scaling (1 to 10 Bands)

The active band count scales dynamically from **1 to 10 bands** (`PEQ_MAX_BANDS = 10`, default `PEQ_DEFAULT_NUM_BANDS = 5`). Core 1 executes filtering strictly over active bands, drastically optimizing CPU execution when fewer filters are active:

| Active Band Count | Use Case Profile | Xtensa LX7 Biquad Ops / Sample | MIPS @ 48 kHz Stereo | Core 1 CPU Utilization (%) |
| :---: | :--- | :---: | :---: | :---: |
| **1 Band** | Subsonic HPF / Single Notch Filter | 2 biquads (L/R) | ~0.24 MIPS | **~0.4%** |
| **3 Bands** | Classic Tone Controls (Bass, Mid, Treble) | 6 biquads (L/R) | ~0.72 MIPS | **~1.2%** |
| **5 Bands** | Standard Audiophile PEQ (Default) | 10 biquads (L/R) | ~1.20 MIPS | **~2.0%** |
| **7 Bands** | Extended Acoustic Tuning & Room Correction | 14 biquads (L/R) | ~1.68 MIPS | **~2.8%** |
| **10 Bands** | Pro Mastering & Calibrated AutoEQ Targets | 20 biquads (L/R) | ~2.40 MIPS | **~3.8%** |

### 2.2 DSP Audio Safety Invariants

1. **15 ms Click-Free Crossfading:** Real-time parameter updates (frequency, gain, Q factor, or band count) transition smoothly across a 15 ms linear crossfade window between double-buffered filter states. This eliminates zipper noise, clicks, and discontinuities during live tuning.
2. **Hard Saturation Clamping:** Floating-point audio buffers are explicitly bounded to `[-32768.0f, 32767.0f]` prior to integer conversion, preventing harsh digital wrap-around clipping.
3. **Pre-Gain Headroom Management:** Configurable pre-attenuation (0.0 to -12.0 dB) provides digital headroom to prevent clipping during positive EQ boosts.
4. **Subnormal Float Flushing:** Floating-point intermediate values smaller than $1 \times 10^{-15}$ are flushed to zero to prevent CPU penalties from subnormal (denormalized) floating-point calculations.
5. **Built-in Presets:**
   * **Preset 0 (Reference Flat):** Bit-perfect bypass (0 dB gain across the entire audio spectrum).
   * **Preset 1 (Warm Bass Boost):** Low Shelf +4.5 dB @ 105 Hz ($Q = 0.71$) for warm, punchy low-end response.
   * **Preset 2 (Harman OE2018 Target):** Multi-band target curve tuned for standard over-ear headphone acoustic profiles.
   * **Preset 3 (User Custom):** Fully editable custom profile saved in non-volatile flash (NVS).

---

## 3. Physical User Interface & Hardware Controls

The physical interface features a 128×64 SSD1306 monochrome OLED display, an EC11 quadrature rotary encoder, and two tactile navigation buttons:

```
                  ┌──────────────────────────────────────────────┐
                  │ 128x64 SSD1306 OLED DISPLAY                  │
                  │   ┌────────────────────────────────────────┐ │
                  │   │ [USB DAC]  48.0k 16b       ((•))  82%  │ │
                  │   │ -------------------------------------- │ │
                  │   │ ESP32-S3 Audio DSP                     │ │
                  │   │ Harman OE2018                          │ │
                  │   │ [████████████████████████░░░░░░]       │ │
                  │   └────────────────────────────────────────┘ │
                  └──────────────────────────────────────────────┘
                       [ ◄ BTN 1 ]   ( ROTARY / SW )   [ ► BTN 2 ]
```

The system provides **three operating UI modes**:

### 3.1 Mode 1: Playback Screen (Default Mode)

| Input Control | Hardware Action | Firmware Response |
| :--- | :--- | :--- |
| **Rotary Turn** | Rotate clockwise / counter-clockwise | Adjusts master volume (0–100%, 2% per detent) with temporary pop-up HUD overlay. |
| **Rotary Push (SW)** | Short Click | Toggles audio mute/unmute; triggers hardware soft mute (`GPIO 7`) and red/cyan LED feedback. |
| **Rotary Push (SW)** | Double-Click | Instantly commits active custom EQ profile to persistent NVS flash. |
| **Rotary Push (SW)** | Long Press (>600 ms) | Cycles active audio source: **USB DAC** $\rightarrow$ **Tone Test** $\rightarrow$ **Freq Sweep**. |
| **Button 1 (◄ / SW6)** | Short Click | Opens the **Main Menu**. |
| **Button 1 (◄ / SW6)** | Long Press (>600 ms) | Cycles PEQ preset backward (Custom $\leftarrow$ Harman $\leftarrow$ Bass $\leftarrow$ Flat). |
| **Button 2 (► / SW8)** | Short Click | Cycles PEQ preset forward (Flat $\rightarrow$ Bass $\rightarrow$ Harman $\rightarrow$ Custom). |

### 3.2 Mode 2: Main Menu Screen

| Input Control | Hardware Action | Firmware Response |
| :--- | :--- | :--- |
| **Rotary Turn** | Rotate clockwise / counter-clockwise | Scrolls up/down through menu items. |
| **Button 2 (► / SW8)** | Short Click | Advances down the menu list. |
| **Button 1 (◄ / SW6)** | Short Click | Exits the menu and returns to the Playback Screen. |
| **Rotary Push (SW)** | Short Click | Selects the highlighted item: <br>• **Live EQ Adjust:** Enters the on-device Digit-by-Digit PEQ Editor.<br>• **EQ Preset:** Cycles active preset.<br>• **Audio Source:** Cycles active audio source.<br>• **Preamp Headroom:** Cycles pre-gain attenuation (0.0 to -6.0 dB in 2 dB steps).<br>• **Save to NVS:** Commits current EQ settings to flash.<br>• **Exit:** Returns to Playback Screen. |
| **Rotary Push (SW)** | Long Press (>600 ms) | Instantly commits current settings to NVS flash. |

### 3.3 Mode 3: Standalone On-Device Digit-by-Digit PEQ Editor

The on-device PEQ editor allows precision, standalone tuning of every filter parameter directly on the OLED display **without requiring a PC**:

```
 ┌────────────────────────────────────────┐
 │ B1:PK  [F: 01000Hz]  G:+03.5dB  Q:0.71 │
 │ [ADD]  [DEL]  [SAVE]  [EXIT]           │
 └────────────────────────────────────────┘
```

* **Cursor Navigation:** Press **Button 1 (◄)** or **Button 2 (►)** to move the active cursor across:
  $$\text{Band Index} \longleftrightarrow \text{Filter Type} \longleftrightarrow \text{Freq Digits (10k, 1k, 100, 10, 1)} \longleftrightarrow \text{Gain (Sign, 10, 1, 0.1 dB)} \longleftrightarrow \text{Q (Int, 0.1, 0.01)} \longleftrightarrow \text{Actions}$$
* **Parameter Adjustment:** Turn the **Rotary Encoder** to increment or decrement the highlighted digit or option.
* **Section Jump:** Click the **Rotary Push Switch (SW)** to jump directly to the next parameter section.
* **Quick Add/Delete Bands:** Double-click **Button 2 (►)** to append a new band; double-click **Button 1 (◄)** to delete the active band.
* **Save & Exit:** Highlight `[SAVE]` and click, or long-press the Rotary Switch to commit to NVS flash with an on-screen confirmation banner.
* **Cancel & Revert:** Long-press **Button 1 (◄)** to discard changes and revert immediately to the pre-edit snapshot.

---

## 4. Driverless USB WebHID Live Tuner

Commercial audiophile dongles like the **FiiO JA11** and **Moondrop FreeDSP** tune their onboard DSP over the same USB-C connection used for audio. Project ASCENDANCE provides a production-grade, driverless WebHID tuner web app in [`tools/webhid_tuner/index.html`](tools/webhid_tuner/index.html).

### 4.1 WebHID Capabilities

* **100% Offline & Driverless:** Runs directly inside Google Chrome, Microsoft Edge, Opera, or Android Chrome using the standard WebHID API (`navigator.hid`). No external drivers, third-party software, or Wi-Fi required.
* **60 FPS Live Slider Dragging:** Real-time updates stream over 64-byte raw HID packets in $<1\text{ ms}$, adjusting volatile SRAM immediately.
* **Absolute Zero Flash Wear During Auditioning:** Moving sliders alters volatile memory only. Flash sectors are untouched until explicit save.
* **Interactive Frequency Response Curve:** HTML5 Canvas logarithmic graph ($20\text{ Hz}$ to $20\text{ kHz}$, $+12\text{ dB}$ to $-24\text{ dB}$) with draggable filter nodes.
* **Scroll-Wheel Q Factor Adjustment:** Scroll the mouse wheel or trackpad over any filter node to widen or narrow its bandwidth, complete with dynamic $-3\text{ dB}$ cutoff guidelines.
* **Dynamic Band Scaling (1 to 10 Bands):** Seamlessly add, remove, or re-allocate filter bands with automatic ISO frequency spacing.
* **Precision Numeric Inputs with Safe Clamping:** Direct numerical input fields alongside sliders with automated boundary validation:
  * **Frequency:** 20 Hz to 20,000 Hz (1 Hz resolution)
  * **Gain:** -24.0 dB to +12.0 dB (0.1 dB resolution)
  * **Q Factor:** 0.10 to 10.00 (0.01 resolution)
  * **Preamp Headroom:** -12.0 dB to 0.0 dB (0.1 dB resolution)
* **Bidirectional Hardware Sync:** The firmware reports its active band count in byte 16 (`reserved[0]`) of the HID report. The browser UI adapts automatically.
* **Explicit NVS Commit:** Click `[💾 Save to Hardware NVS]` to permanently store your profile in non-volatile flash, validated by a 32-bit CRC32 checksum.

---

## 5. Audio Pipeline & Zero-Pop Protection

### 5.1 4-Phase Zero-Pop Hardware Muting Protocol

To prevent pops, thumps, or DC transients through connected headphones during startup, shutdown, and source transitions, the `audio_source_arbiter` coordinates a 4-phase protection sequence:

1. **Phase 1 (Hardware Soft Mute):** Pulls the PCM5102A `XMT` pin (`GPIO 7`) LOW, activating internal DAC analog muting.
2. **Phase 2 (Digital Attenuation):** Applies a 10 ms linear digital fade-out to zero on outgoing PCM buffers.
3. **Phase 3 (Stream Handover & Buffer Reset):** Flushes DMA buffers, switches source pointers, and establishes the new sample stream.
4. **Phase 4 (Hardware Unmute & Ramp):** Releases `GPIO 7` HIGH and applies a smooth 10 ms digital fade-in to target volume.

### 5.2 Musical Startup Chime

Upon initial power-on, the audio pipeline plays an ascending 4-note arpeggio (**C5 - E5 - G5 - C6**) synthesized in real-time, verifying DAC clocking, amplifier bias, and headphone connectivity.

### 5.3 Smart Status LED & Optical Safety

An onboard WS2812B aRGB LED on `GPIO 48` provides visual status feedback using the ESP32 RMT peripheral:
* **Optical Safety Limit:** Brightness is capped at 24% duty cycle (60/255) to prevent glare and reduce power consumption.
* **Loudness Glow:** Emits a subtle cyan glow whose brightness tracks master volume (0–100%).
* **Mute Alert:** Transitions to a gentle, steady red glow when muted.
* **Startup Animation:** Synchronizes with the ascending startup chime.

---

## 6. Hardware Bill of Materials (BOM) & Pinout Table

### 6.1 Bill of Materials (Schematic 2: `DSP+DAC+SCREEN.svg` Rev 1.1)

| Component | Function / Specification | Notes |
| :--- | :--- | :--- |
| **YD-ESP32-S3 (N16R8)** | Main Controller (Dual-Core LX7 @ 240 MHz, 16 MB Flash, 8 MB Octal PSRAM) | Dual USB-C ports (Native USB OTG + CH343 UART) |
| **GY-PCM5102A Module** | 32-bit / 384 kHz Stereo Audio DAC (TI PCM5102A) | Ground-centered DirectPath™ outputs (no DC caps needed) |
| **TDA1308 Module** | Hi-Fi Class-AB Stereo Headphone Amplifier (NXP/Philips TDA1308) | 80 mW into 32 Ω loads, 16 Ω to 600 Ω headphone support |
| **SSD1306 0.96" OLED** | 128×64 Monochrome I2C Display (Address `0x3C`) | Displays playback HUD, menu, and digit-by-digit EQ editor |
| **EC11 Rotary Encoder** | Quadrature rotary encoder with push switch | Volume/mute control, menu navigation, and EQ parameter tuning |
| **Tactile Pushbutton 1**| Navigation Button 1 / SW6 (GPIO 18) | Menu / Back / Left (◄) / Cancel |
| **Tactile Pushbutton 2**| Navigation Button 2 / SW8 (GPIO 10) | Preset Cycle / Down / Right (►) / Add Band |
| **WS2812B RGB LED** | Onboard aRGB Smart LED (GPIO 48) | Real-time volume loudness brightness & mute indicator |
| **3.5 mm TRS Jack (J3)** | Stereo headphone audio output connector | Tip = Left, Ring = Right, Sleeve = Analog Ground |

### 6.2 Complete Master Pinout Table

| From (Source) | Pin Label | To (Destination) | Pin Label | Wire Function / Electrical Characteristics |
| :--- | :--- | :--- | :--- | :--- |
| **ESP32-S3** | **GPIO 4** | **PCM5102A** | **LCK** | I2S Word Select / Left-Right Clock (WS, 48 kHz) |
| **ESP32-S3** | **GPIO 5** | **PCM5102A** | **DIN** | I2S Serial Audio Data Out (DOUT, 16-bit stereo) |
| **ESP32-S3** | **GPIO 6** | **PCM5102A** | **BCK** | I2S Continuous Bit Clock (BCLK, 1.536 MHz) |
| **ESP32-S3** | **GPIO 7** | **PCM5102A** | **XMT** | Soft Mute Control (HIGH = Play, LOW = Soft Mute) |
| **ESP32-S3** | **GPIO 8** | **OLED1** | **SDA** | I2C Data Line (SSD1306 @ `0x3C`, 400 kHz fast mode) |
| **ESP32-S3** | **GPIO 9** | **OLED1** | **SCL** | I2C Clock Line (SSD1306 @ `0x3C`, 400 kHz fast mode) |
| **ESP32-S3** | **GPIO 10** | **SW8** | **Pin 1** | Navigation Button 2 / Right (Active-LOW, internal pull-up) |
| **ESP32-S3** | **GPIO 15** | **EC11 / SW2** | **A** | Rotary Encoder Phase A (PCNT unit, 1000 ns glitch filter) |
| **ESP32-S3** | **GPIO 16** | **EC11 / SW2** | **B** | Rotary Encoder Phase B (PCNT unit, 1000 ns glitch filter) |
| **ESP32-S3** | **GPIO 17** | **EC11 / SW2** | **SW** | Rotary Push Switch (Active-LOW, debounced) |
| **ESP32-S3** | **GPIO 18** | **SW6** | **Pin 1** | Navigation Button 1 / Left (Active-LOW, 10 kΩ pull-up `R8`) |
| **ESP32-S3** | **GPIO 19** | **Native USB** | **D-** | Full-Speed USB OTG Data Minus |
| **ESP32-S3** | **GPIO 20** | **Native USB** | **D+** | Full-Speed USB OTG Data Plus |
| **ESP32-S3** | **GPIO 48** | **Onboard LED** | **DIN** | WS2812B aRGB LED (RMT driver; loudness & mute alert) |
| **ESP32-S3** | **5V (VIN)** | **PCM5102A** | **VCC** | DAC Power supply (module has onboard 3.3V LDO) |
| **ESP32-S3** | **5V (VIN)** | **TDA1308** | **VCC** | Headphone Amp Power supply (3V to 6V operating range) |
| **ESP32-S3** | **3.3V** | **OLED1** | **VCC** | Display Power supply (3.3V DC) |
| **ESP32-S3** | **GND** | **System** | **GND** | System Common Star Ground |
| **PCM5102A** | **L** | **TDA1308** | **IN_L** | Left Channel analog line-level signal |
| **PCM5102A** | **R** | **TDA1308** | **IN_R** | Right Channel analog line-level signal |
| **PCM5102A** | **G** | **TDA1308** | **GND** | Analog Ground reference |
| **TDA1308** | **OUT_L/R/G** | **Jack J3** | **Tip/Ring/Sleeve** | 3.5 mm Stereo Headphone Output |

### 6.3 Mandatory Hardware Solder Jumpers & Grounding Rules

1. **GY-PCM5102A Solder Jumpers (Underside SMD Pads):**
   * **`SCK` $\rightarrow$ `GND`:** **Mandatory.** Directs the DAC's internal PLL to synthesize MCLK from BCK. (Leaving open produces silence).
   * **`FMT` $\rightarrow$ `GND`:** **Mandatory.** Configures standard Philips I2S format. (Bridging to HIGH causes loud static).
   * **`FLT` $\rightarrow$ `GND`:** Normal latency FIR filter rolloff.
   * **`DMP` $\rightarrow$ `GND`:** De-emphasis filter off.
   * **`XMT` (Front Pin):** Connect to `GPIO 7`. Never leave floating.
2. **YD-ESP32-S3 Development Board:**
   * **Bridge `IN-OUT` SMD Jumper:** Connects USB-C VBUS to the `5V` header pin to power the DAC and amplifier.
3. **Star-Grounding (Eliminating 1 kHz USB Frame Buzz):**
   * Connect digital grounds and analog grounds to a single common star point at the power entry.
   * **Critical:** Connect the 3.5 mm headphone jack sleeve ground **directly to the TDA1308 analog ground pad**, never to the ESP32 digital ground pin.

---

## 7. Project Directory Layout

```
esp32s3_dsp_dac/
├── CMakeLists.txt                # Root CMakeLists (Project ASCENDANCE, TinyUSB UAC flags)
├── dependencies.lock             # ESP Component Manager locked versions
├── pytest_hello_world.py         # Pytest target verification script
├── README.md                     # This authoritative project overview & guide
├── sdkconfig.defaults            # ESP32-S3 16MB Flash, 8MB Octal PSRAM & TinyUSB configs
├── main/                         # Production Firmware C Source & Headers
│   ├── CMakeLists.txt            # Component source registration
│   ├── Kconfig.projbuild         # Menuconfig options & pin mappings
│   ├── idf_component.yml         # ESP-IDF component dependencies (esp_tinyusb)
│   ├── board_pins.h              # Master pinout mapping for Schematics 1, 2, and 3
│   ├── main.c                    # Dual-core tasks, UI state machine, and entry point
│   ├── audio_pipeline.c/.h       # Master I2S0 DMA driver, volume control & startup chime
│   ├── audio_source_arbiter.c/.h # 4-phase zero-pop muting arbiter (USB DAC, Tone, Sweep)
│   ├── peq_dsp.c/.h              # 1-to-10 band PEQ biquads, crossfader, NVS persistence
│   ├── webhid_peq.c/.h           # TinyUSB composite UAC 1.0 + WebHID 64-byte protocol
│   ├── oled_display.c/.h         # SSD1306 128x64 driver: Playback, Menu & EQ Editor
│   ├── controls.c/.h             # Hardware PCNT rotary encoder & button debouncer
│   ├── led_indicator.c/.h        # WS2812B aRGB LED via RMT with optical safety cap
│   └── minimp3.h                 # Standalone MP3 decoding engine (research)
├── tools/                        # Development & Tuning Tools
│   └── webhid_tuner/
│       └── index.html            # Driverless WebHID live browser tuner app
└── docs/                         # Comprehensive 6-Tier Documentation
    ├── README.md                 # Master Documentation Portal Index
    ├── firmware/                 # Firmware implementations and roadmaps
    ├── hardware/                 # BOM, physical wiring guides, and schematics
    ├── guides/                   # Subsystem deep dives (UAC, PEQ, UI, Arbiter, etc.)
    ├── reference/                # Master Agentic AI reference & architecture
    ├── research/                 # Dual-MCU, SDMMC player, and networking research
    ├── schematics/               # Vector schematics (SVG, PNG, ZIP netlists)
    └── software_functions/       # Per-function C99 specifications for AI agents
```

---

## 8. Build, Flash & Usage Guide

### 8.1 Prerequisites & Toolchain

* **ESP-IDF:** v6.1 (or v5.x) installed and configured in your shell environment.
* **Compiler & Build Tools:** CMake 3.22+, Ninja, Python 3.10+.
* **Browser:** Modern Chromium-based browser (Google Chrome, Microsoft Edge, Opera, or Android Chrome) for WebHID live tuning.

### 8.2 Build Firmware

```bash
# Navigate to project root
cd esp32s3_dsp_dac

# Set target to ESP32-S3
idf.py set-target esp32s3

# Build the firmware
idf.py build
```

### 8.3 Flash Firmware & Open Serial Monitor

Connect the YD-ESP32-S3 via the **UART COM Port** (CH343 / CP2102 bridge):

```bash
# Flash and open serial monitor
idf.py -p /dev/ttyACM0 flash monitor
```

*(Press `Ctrl + ]` to exit the serial monitor).*

### 8.4 Linux Permissions for WebHID Live Tuner

On Linux, Chromium browsers require permissions to communicate with raw USB HID devices. Add a udev rule to grant access:

```bash
sudo bash -c 'cat << "EOF" > /etc/udev/rules.d/99-esp-webhid.rules
# Espressif TinyUSB WebHID Interface (VID: 303A, PID: 4002)
SUBSYSTEM=="hidraw", ATTRS{idVendor}=="303a", ATTRS{idProduct}=="4002", MODE="0666"
EOF'

# Reload udev rules
sudo udevadm control --reload-rules && sudo udevadm trigger
```

### 8.5 Using the WebHID Live Tuner

1. Connect the YD-ESP32-S3 **Native USB port** (GPIO 19/20) to your PC, Mac, or Android device using a USB Type-C cable.
2. Open [`tools/webhid_tuner/index.html`](tools/webhid_tuner/index.html) in Google Chrome or Microsoft Edge.
3. Click **"Connect USB DAC"** and select **ESP32-S3 Hi-Fi DSP DAC** from the browser device prompt.
4. Tune your sound in real-time (60 FPS volatile SRAM auditioning, zero flash wear):
   * **Drag Control Nodes:** Drag numbered circles on the frequency response graph ($X$ = Frequency, $Y$ = Gain).
   * **Scroll-Wheel Q Factor:** Scroll over any node to adjust its filter sharpness ($Q = 0.10$ to $10.00$).
   * **Double-Click Reset:** Double-click a node to reset gain to 0.0 dB; double-click empty canvas to create a new band.
   * **Dynamic Bands (1 to 10):** Add custom bands via `+ Add Band`, remove via `✕`, or enter a count in the stepper.
   * **Direct Value Typing:** Type exact values within safety bounds for Frequency, Gain, Q Factor, and Preamp.
5. Click **"💾 Save to Hardware NVS"** to permanently store your profile in non-volatile flash.

---

## 9. Comprehensive Documentation Reference

The complete documentation suite is organized into six tiers. Visit the **[Master Documentation Portal](docs/README.md)** for detailed guides:

* **[Master Documentation Portal](docs/README.md):** Central documentation directory, 6-tier taxonomy, and hardware quick-reference.
* **[Current Firmware Implementation Record](docs/firmware/current_firmware_implementation.md):** Consolidated technical record of active production firmware, UAC 1.0 descriptors, PEQ engine, and build/flash specifications.
* **[Software Architecture & Implementation](docs/firmware/software.md):** Subsystem architecture, ESP-IDF v6.1 `driver/i2s_std.h`, FreeRTOS dual-core task layout, and developer invariants.
* **[Hardware & Wiring Guide](docs/hardware/hardware.md):** BOM, full pinout table, GY-PCM5102A jumper settings, TDA1308 amp integration, and star-grounding practices.
* **[Hardware Schematics Guide](docs/hardware/schematics.md):** Schematic 2 (`DSP+DAC+SCREEN.svg`), vector schematics, netlists, and sign-off checklists.
* **[USB Audio Class (UAC) DAC Guide](docs/guides/usb_dac_guide.md):** TinyUSB UAC 1.0 descriptors, isochronous ringbuffer sync, WebHID packet schemas, and live tuning architecture.
* **[Parametric Equalizer (PEQ) DSP Engine Guide](docs/guides/parametric_eq_guide.md):** RBJ biquad mathematics, dynamic band count scaling (1–10 bands), CPU scaling table, click-free crossfader, and NVS persistence.
* **[User Interface, Display & Controls Guide](docs/guides/ui_and_controls_guide.md):** SSD1306 128x64 OLED graphics driver, hardware PCNT rotary decoding, and 3-mode UI state machine.
* **[Multi-Mode Audio Switching & Handover Guide](docs/guides/mode_switching_guide.md):** Central `AudioSourceManager` arbiter, 4-phase zero-pop hardware muting protocol (`GPIO 7: XMT`), and tone/sweep generators.
* **[Troubleshooting & Root Cause Analysis](docs/guides/troubleshooting.md):** Deep post-mortems for 1 kHz USB whine elimination, ground loops, stack overflows, and brownout resets.
* **[Master Agentic AI Developer Reference](docs/reference/agentic_ai_reference.md):** The 10 Golden Invariants, master pinouts, task affinities, and automated triage trees for AI agents.
* **[Future Enhancements & Architectural Roadmap](docs/firmware/future_enhancements.md):** Architectural blueprints for Channel Matrix Mixer & Headphone Crossfeed, 24-bit/96kHz pipeline, dual-rate reclocking, and OLED response curves.
* **[Per-Function Software Specifications](docs/software_functions/README.md):** Function-by-function reference for autonomous coding agents with exact C99 prototypes and memory boundaries.
