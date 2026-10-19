# System Architecture & Overview

> **Authoritative Technical Specification & Architectural Blueprint**  
> **Target Platform:** YD-ESP32-S3 (Xtensa Dual-Core LX7 @ 240 MHz, 16 MB Flash, 8 MB Octal PSRAM)  
> **Production Baseline:** Schematic 2 (`docs/schematics/DSP+DAC+SCREEN.svg`)  
> **Primary Technical Record:** [`docs/current_firmware_implementation.md`](../firmware/current_firmware_implementation.md)  
> **Software Architecture:** [`docs/software.md`](../firmware/software.md)

---

## 1. Executive Summary

This project implements an audiophile-grade, driverless **USB Audio Class 1.0 (UAC 1.0) DAC and Real-Time DSP Audio Processor** using the **YD-ESP32-S3 (N16R8)** development board. Digital audio streams at 48,000 Hz / 16-bit stereo PCM are received over the Native USB OTG port, processed through a **dynamic 1-to-10 band Parametric Equalizer (PEQ)** with **click-free double-buffered linear crossfading**, and transmitted over an I2S digital audio bus into a **Texas Instruments GY-PCM5102A 32-bit DAC**. The ground-centered analog output is boosted by an onboard **NXP/Philips TDA1308 Class-AB headphone amplifier** to drive headphones from 16 Ω to 600 Ω.

Simultaneously, the device provides an on-device **SSD1306 128x64 OLED display**, hardware **EC11 rotary encoder**, tactile mode pushbutton, and a **driverless WebHID live tuner web application** (`tools/webhid_tuner/index.html`) allowing 60 FPS real-time parametric curve editing inside standard web browsers over the same USB-C cable.

---

## 2. End-to-End Dataflow Pipeline

```
  ┌─────────────────────────────────────────────────────────┐
  │ HOST PC / MAC / LINUX / ANDROID PHONE                   │
  │   • USB Audio Class 1.0 (48.0 kHz / 16-bit Stereo PCM)  │
  │   • WebHID Client (Google Chrome, MS Edge, Opera)       │
  └────────────────────────────┬────────────────────────────┘
                               │ Native USB Type-C Cable
                               ▼
  ┌─────────────────────────────────────────────────────────┐
  │ YD-ESP32-S3 NATIVE USB OTG (GPIO 19: D-, GPIO 20: D+)   │
  │                                                         │
  │  ┌─────────────────────────┐   ┌─────────────────────┐  │
  │  │ Interface 0 & 1: UAC 1.0│   │ Interface 2: WebHID │  │
  │  │ • 1 ms Isochronous OUT  │   │ • 64-byte IN/OUT    │  │
  │  │ • Feature Unit Vol/Mute │   │ • 60 FPS Live Tuning│  │
  │  └────────────┬────────────┘   └──────────┬──────────┘  │
  │               │ 192 bytes/ms              │ 64-byte raw │
  │               ▼                           ▼             │
  │  ┌─────────────────────────┐   ┌─────────────────────┐  │
  │  │ CORE 1: AUDIO DSP TASK  │   │ CORE 0: CONTROL     │  │
  │  │ (Priority 10)           │   │ • TinyUSB Event Loop│  │
  │  │ • UAC 1.0 FIFO Read     │   │ • WebHID Parser     │  │
  │  │ • 1-10 Band PEQ Biquads │   │ • OLED I2C Display  │  │
  │  │ • 15 ms Crossfade Blend │   │ • PCNT Rotary Decode│  │
  │  │ • Saturation Clamping   │   │ • NVS Wear-Leveling │  │
  │  │ • I2S0 DMA Stream Out   │   │ • Source Arbiter    │  │
  │  └────────────┬────────────┘   └─────────────────────┘  │
  └───────────────┼─────────────────────────────────────────┘
                  │ I2S Philips Bus (3-Wire):
                  │ GPIO 4: LCK (Word Select / 48 kHz)
                  │ GPIO 5: DIN (Serial Audio Data)
                  │ GPIO 6: BCK (Bit Clock / 1.536 MHz)
                  │ GPIO 7: XMT (Hardware Soft-Mute, Active-LOW)
                  ▼
  ┌─────────────────────────────────────────────────────────┐
  │ TEXAS INSTRUMENTS GY-PCM5102A STEREO DAC                │
  │   • Internal PLL: Generates MCLK from BCK (SCK = GND)   │
  │   • DirectPath™ Charge Pump: 2.1 VRMS Ground-Centered   │
  │   • 32-bit / 384 kHz capable, 112 dB Dynamic Range      │
  └────────────────────────────┬────────────────────────────┘
                               │ Analog Line Out (L, R, AGND)
                               ▼
  ┌─────────────────────────────────────────────────────────┐
  │ NXP/PHILIPS TDA1308 CLASS-AB HEADPHONE AMPLIFIER        │
  │   • 0.03% THD+N, 110 dB SNR, 80 mW @ 32 Ω               │
  │   • Drives 16 Ω to 600 Ω Headphones                     │
  └────────────────────────────┬────────────────────────────┘
                               │ Analog Headphone Output
                               ▼
                 [3.5mm Stereo Headphone Jack J3]
```

### 2.1 Standalone Diagnostic Audio Sources
The system also incorporates an internal **Audio Source Arbiter** ([`main/audio_source_arbiter.c`](../../main/audio_source_arbiter.c)) that can generate onboard diagnostic signals on Core 1 when no USB host is streaming:
1. `AUDIO_SOURCE_STANDALONE`: Default UAC 1.0 USB DAC stream consumer.
2. `AUDIO_SOURCE_TONE_TEST`: Synthesizes an A minor arpeggio (A4 $440\text{ Hz}$, C5 $523\text{ Hz}$, E5 $659\text{ Hz}$, A5 $880\text{ Hz}$) cycling every 250 ms to evaluate EQ curves without host audio.
3. `AUDIO_SOURCE_SWEEP`: Continuous logarithmic/linear sweep from 100 Hz to 4,000 Hz for acoustic and distortion testing.

---

## 3. Key Design Decisions & Trade-Offs

### 3.1 Native USB UAC 1.0 vs. Bluetooth Classic (Why Not A2DP?)
The ESP32-S3 SoC features Wi-Fi 4 and Bluetooth 5 (Low Energy / BLE). **It does NOT have Bluetooth Classic (BR/EDR) hardware**.
* Because consumer smartphones, computers, and tablets transmit audio using Bluetooth Classic **A2DP** (Advanced Audio Distribution Profile), an ESP32-S3 cannot natively act as an A2DP Bluetooth speaker without an auxiliary co-processor.
* **Why USB Audio Class 1.0:** UAC 1.0 is natively supported by Windows, macOS, Linux, Android, and iOS without third-party drivers. It guarantees uncompressed, low-latency ($<15\text{ ms}$) transmission free from the lossy compression, SBC/AAC transcoding artifacts, and RF dropouts inherent to Bluetooth.

### 3.2 Driverless WebHID vs. Proprietary Desktop Apps
Traditional USB audio dongles with onboard DSP (e.g., FiiO JA11, Moondrop FreeDSP) require proprietary Android APKs or custom Windows software.
* By implementing a **Vendor USB HID interface** alongside UAC 1.0, the DSP is tuned live in any Chromium browser via the standard `navigator.hid` API.
* Works 100% offline, requires zero administrative installation privileges, and functions across Windows, macOS, Linux, Chromebooks, and Android.

### 3.3 Dual-Core Workload Distribution
* **Core 0 (Control & System):** TinyUSB background processing, USB HID command parsing, SSD1306 OLED rendering, PCNT hardware rotary decoding, mode button polling, and NVS flash persistence.
* **Core 1 (Real-Time Audio DSP):** Dedicated strictly to real-time audio tasks (`audio_dsp_task`, UAC 1.0 FIFO reading, cascaded biquad calculations, linear crossfade blending, and I2S0 DMA feeding).
* This hard isolation ensures that display I2C traffic or flash operations never cause audio buffer underflows or audible clicks.

### 3.4 Click-Free Double-Buffered Crossfading
Altering biquad filter coefficients instantaneously during playback causes transient waveform discontinuities perceived as clicks, ticks, or zipper noise:
* The DSP engine synthesizes new coefficients into a staging buffer (`target_biquads[]`).
* A linear interpolation loop smoothly blends between active and target filters over 720 samples (15 ms @ 48 kHz).
* Once the crossfade is complete, the target biquads become active atomically with zero pointer race conditions.

### 3.5 Dual-MCU Upgrade Roadmap for Bluetooth A2DP & Total RF Isolation
For installations requiring wireless smartphone streaming while maintaining audiophile DSP quality, the repository specifies an optional **Dual-MCU Distributed Architecture** ([`docs/dual_esp32_architecture.md`](../research/dual_esp32_architecture.md)):
* **Auxiliary ESP32 Classic (Media & Connectivity Hub):** Hosts Bluetooth Classic A2DP Sink, Wi-Fi streaming, MicroSD storage, and user controls.
* **Primary ESP32-S3 (Pure DSP & DAC Engine):** Dedicated to USB UAC audio, Xtensa PIE SIMD Parametric Equalization, and driving the PCM5102A DAC over I2S with its radio permanently disabled for total RF silence.

---

## 4. Reference for AI Agents & Autonomous Coders

When generating, modifying, or refactoring code within this repository, AI agents MUST strictly comply with these core invariants:

1. **Octal PSRAM Pin Protection (FATAL):** Never allocate, reconfigure, or route signals to `GPIO 33, 34, 35, 36, 37`. These pins are dedicated to the Octal PSRAM bus (`APS6404L`). Accessing them triggers an unrecoverable kernel panic.
2. **Never Propose Bluetooth Classic (A2DP) on ESP32-S3:** The ESP32-S3 silicon does not support BR/EDR. Attempts to import `esp_a2dp_api.h` or enable Bluetooth Classic will fail compilation. Media streaming must use USB UAC, Wi-Fi HTTP, or an external ESP32 co-processor.
3. **Core 1 Isolation for Audio DSP:** The real-time audio loop must execute strictly on **Core 1** at high priority (Priority 10). Never run non-deterministic tasks (USB stack, NVS flash commits, display I2C) on Core 1.
4. **Zero Dynamic Allocation in Audio Loops:** Never call `malloc()`, `calloc()`, or `realloc()` inside `audio_dsp_task` or `peq_dsp_process_interleaved_s16()`. Pre-allocate all buffers in static BSS or during initialization.
5. **Hard Saturation Clamping:** Clamp all floating-point DSP output samples to signed 16-bit integer boundaries (`[-32768, 32767]`) before casting to `int16_t`.
6. **Flash Wear Protection:** Never write to NVS flash inside real-time control loops or WebHID slider dragging callbacks. All slider adjustments must be applied volatilely in SRAM, reserving flash writes for explicit user commit commands.
7. **Hardware Soft-Mute Protocol:** Always pull `GPIO 7 LOW` before altering I2S clock dividers, switching audio sample rates, or transitioning audio sources. Hold mute for at least 15 ms, reconfigure the pipeline, and hold for 15 ms after unmuting.
8. **I2S Master Clock Synthesizer:** Do NOT configure an external MCLK GPIO on the ESP32-S3. The GY-PCM5102A hardware board has its `SCK` jumper tied to `L` (GND) to utilize its internal low-jitter PLL derived directly from BCK (GPIO 6).
