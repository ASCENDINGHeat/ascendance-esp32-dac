# ESP32-S3 High-Fidelity USB DAC & DSP: Software Architecture & Implementation

> **Authoritative Technical Specification & Developer Reference**  
> **Target Platform:** YD-ESP32-S3 (Xtensa Dual-Core LX7 @ 240 MHz, 16 MB Flash, 8 MB Octal PSRAM)  
> **Target Framework:** ESP-IDF v6.1 (`driver/i2s_std.h`, FreeRTOS SMP, TinyUSB, esp-dsp)  
> **Hardware Implementation:** Schematic 2 (`docs/schematics/DSP+DAC+SCREEN.svg`)  
> **Primary Technical Record:** [`docs/current_firmware_implementation.md`](current_firmware_implementation.md)

---

## 1. Overview & Source Architecture

The firmware transforms the ESP32-S3 into a driverless, high-resolution **USB Audio Class 1.0 (UAC 1.0) DAC** featuring a real-time **1-to-10 band Parametric Equalizer (PEQ)** with **click-free double-buffered crossfading**, zero-pop hardware muting, on-device OLED visualizer, hardware rotary encoder controls, and a concurrent **WebHID live browser tuner interface** over a single USB-C cable.

```
                           YD-ESP32-S3 (N16R8)
 ┌─────────────────────────────────────────────────────────────────────────────┐
 │ Native USB OTG (GPIO 19: D-, GPIO 20: D+)                                   │
 │   • Interface 0: USB Audio Control (AC) - Feature Unit Mute & Volume        │
 │   • Interface 1: USB Audio Streaming (AS) - 24-bit / 44.1, 48 & 96 kHz PCM  │
 │   • Interface 2: Vendor HID (WebHID) - 64-Byte Bidirectional Reports        │
 ├─────────────────────────────────────────────────────────────────────────────┤
 │ Core 0 (System, Control & Visual Subsystems)                                │
 │   • TinyUSB Device Stack Task (`tinyusb_task`, Priority 5, 16 KB SW FIFO)   │
 │   • Hardware PCNT Rotary Encoder (GPIO 15/16) + Push Switch (GPIO 17)       │
 │   • Dual Mode/Nav Pushbuttons (`SW6` GPIO 18, `SW8` GPIO 10)                │
 │   • Control Dispatcher Task (`ctrl_disp`, Priority 4)                       │
 │   • SSD1306 128x64 OLED Display Driver (I2C Master: SDA=8, SCL=9, Prio 2)  │
 │   • Onboard WS2812B aRGB LED Volume Loudness Driver (GPIO 48 via RMT)       │
 │   • Audio Source Arbiter State Machine & 4-Phase Mute Protocol              │
 │   • NVS v2 Persistence (`user_cfg_v2`, CRC32 + Automatic V1 Migration)      │
 ├─────────────────────────────────────────────────────────────────────────────┤
 │ Core 1 (Hard Real-Time Audio DSP Subsystem)                                 │
 │   • High-Priority Audio DSP Task (`audio_dsp_task`, Priority 10, 8,192 B)   │
 │   • Safe Zero-Pop Sample-Rate Reclocking (44.1 / 48.0 / 96.0 kHz)           │
 │   • 1-to-10 Band Dual-Mono L/R PEQ (8 RBJ Biquad Topologies)                │
 │   • 2x2 Channel Matrix Mixer & 3-Level Chu Moy Headphone Crossfeed          │
 │   • 15 ms Click-Free Crossfader + Preamp-Normalized Instant A/B Bypass      │
 │   • Preamp Gain Headroom & 24-bit Clamping `[-8388608, 8388607]`            │
 │   • Master I2S0 DMA Streamer (32-bit Stereo Slots MSB-Aligned)              │
 ├─────────────────────────────────────────────────────────────────────────────┤
 │ External Audio & User Interface Hardware                                    │
 │   • GPIO 4: Master I2S Word Select / LCK (WS)  ────────► GY-PCM5102A DAC    │
 │   • GPIO 5: Master I2S Serial Data (DIN/DOUT)  ────────► (32-bit / 384 kHz) │
 │   • GPIO 6: Master I2S Bit Clock (BCK)         ────────►                    │
 │   • GPIO 7: Active-LOW Hardware Soft-Mute (XMT)────────►                    │
 │                                                              │ Analog Out   │
 │                                                              ▼              │
 │                                                        TDA1308 Stereo Amp   │
 │                                                        (3.5mm Headphone J3) │
 └─────────────────────────────────────────────────────────────────────────────┘
```

### 1.1 Source Tree Layout
The codebase is structured into modular, decoupled C subsystems in [`main/`](../../main) and browser tooling in [`tools/`](../../tools):

```
├── .github/workflows/
│   ├── build.yml                          # ESP-IDF v6.1 CI build, merged bin, size report & WebHID check
│   └── release.yml                        # Tagged GitHub Release firmware & tuner packaging
├── CMakeLists.txt                         # Top-level ESP-IDF project build script
├── sdkconfig.defaults                     # Platform, Flash (16MB), and Octal PSRAM configurations
├── docs/                                  # Complete technical specifications & schematics
│   ├── firmware/
│   │   ├── current_firmware_implementation.md # Exhaustive firmware technical record
│   │   ├── future_enhancements.md         # Roadmap (Matrix Mixer, 24-bit, OLED curves, PWA)
│   │   └── software.md                    # (This document) Software architecture & HAL reference
│   ├── hardware/hardware.md               # Electrical pinouts, schematics & jumper configurations
│   ├── guides/parametric_eq_guide.md      # DSP biquad mathematics & filter design manual
│   ├── guides/usb_dac_guide.md            # USB UAC 1.0 + WebHID stack architecture
│   └── software_functions/                # Modular subsystem function manuals
├── main/
│   ├── CMakeLists.txt                     # Component registration & TinyUSB compile definitions
│   ├── idf_component.yml                  # ESP Component Manager dependencies (`espressif/esp_tinyusb`)
│   ├── Kconfig.projbuild                  # Menuconfig schema (pins, buffer sizes, debug flags)
│   ├── board_pins.h                       # Centralized master hardware pinout definitions
│   ├── main.c                            # Entry point (`app_main`), task orchestration & loops
│   ├── audio_pipeline.c / .h              # Master I2S0 DMA driver, soft-mute & startup chime
│   ├── audio_source_arbiter.c / .h        # 4-Phase zero-pop source switcher (USB DAC/Tone/Sweep)
│   ├── controls.c / .h                    # Hardware PCNT rotary encoder & debounced button handlers
│   ├── oled_display.c / .h                # SSD1306 128x64 OLED driver (I2C 400kHz, non-blocking)
│   ├── peq_dsp.c / .h                     # 1-to-10 band PEQ DSP engine, RBJ biquads & crossfader
│   ├── led_indicator.c / .h               # WS2812B aRGB LED volume loudness driver (RMT 800kHz)
│   ├── webhid_peq.c / .h                  # TinyUSB composite UAC 1.0 + WebHID stack & callbacks
│   └── minimp3.h                          # Public-domain zero-dependency MP3 decoder (research)
└── tools/
    └── webhid_tuner/
        └── index.html                     # Driverless WebHID live GUI client (HTML5 Canvas)
```

---

## 2. Hardware Abstraction & Peripheral Drivers

### 2.1 Master I2S0 Audio Driver ([`main/audio_pipeline.c`](../../main/audio_pipeline.c))
The I2S controller is configured using the modern ESP-IDF v6.1 driver API (`esp_driver_i2s`):

```c
i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
chan_cfg.auto_clear    = true;
chan_cfg.dma_desc_num  = 8;
chan_cfg.dma_frame_num = 256; // 256 stereo frames (2,048 bytes @ 32-bit slots) per descriptor

i2s_std_config_t std_cfg = {
    .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate), // 44100 / 48000 / 96000 Hz
    .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
    .gpio_cfg = {
        .mclk = I2S_GPIO_UNUSED,
        .bclk = (gpio_num_t)BOARD_I2S_BCK_PIN,       // GPIO 6
        .ws   = (gpio_num_t)BOARD_I2S_WS_PIN,        // GPIO 4
        .dout = (gpio_num_t)BOARD_I2S_DATA_OUT_PIN,  // GPIO 5
        .din  = I2S_GPIO_UNUSED,
    },
};
```
* **DMA FIFO Sizing:** 8 descriptors $\times$ 256 stereo frames $\times$ 8 bytes (32-bit L/R slots) = 16,384 bytes total (~42.7 ms at 48 kHz, ~21.3 ms at 96 kHz), absorbing FreeRTOS scheduling and NVS commit jitter.
* **Master Clock Generation:** The Texas Instruments **GY-PCM5102A** DAC generates its internal high-frequency Master Clock (MCLK) via an integrated PLL directly from the Bit Clock (BCK: `2.8224 / 3.072 / 6.144 MHz`). `mclk` is designated `I2S_GPIO_UNUSED`, conserving GPIOs.
* **Hardware Soft-Mute (`GPIO 7`):** Actively asserted LOW during boot, frequency switches, and source transitions to ensure zero audible pops or clicks.
* **Perceptual Volume Taper:** Software master volume uses a quadratic perceptual curve (`norm * norm`) aligned with the UAC 1.0 dB Feature Unit mapping and applied inside Core 1's 32-bit DSP output stage (`audio_pipeline_get_volume_factor()`).

### 2.2 Native USB OTG Composite Stack ([`main/webhid_peq.c`](../../main/webhid_peq.c))
Operating over Native USB Full-Speed (12.0 Mbps) on GPIO 19 (`D-`) and GPIO 20 (`D+`):
* **Interface Association Descriptor (IAD):** Groups Interface 0 (Audio Control) and Interface 1 (Audio Streaming) as a compliant UAC 1.0 speaker.
* **Interface 0 (Audio Control):** Feature Unit provides Master Volume and Mute control integrated directly into the OS volume mixer.
* **Interface 1 (Audio Streaming):** Isochronous OUT endpoint (`0x01`, `588` max bytes, 1 ms polling, `16,384`-byte TinyUSB SW FIFO) delivering **24-bit PCM** at **44.1 kHz, 48.0 kHz, or 96.0 kHz**.
* **Interface 2 (Vendor WebHID):** Interrupt endpoints (`EP 0x02 OUT`, `EP 0x82 IN`, 64 bytes) for bidirectional communication with the WebHID browser tuner (Dual-Mono L/R PEQ, A/B Toggle, 2×2 Matrix/Crossfeed, Sample Rate Telemetry).

### 2.3 Hardware Pulse Counter (PCNT) Driver ([`main/controls.c`](../../main/controls.c))
Quadrature decoding of the EC11 rotary encoder uses the ESP32-S3 dedicated hardware PCNT peripheral:
* **Glitch Filter:** Hardware filter suppresses contact bounce under $1{,}000\text{ ns}$.
* **Offloaded Decoding:** Core 0/1 CPUs are not interrupted on rotary state transitions; pulses accumulate in hardware counter registers and are polled efficiently at 50 Hz (`20 ms` period).

### 2.4 Fast I2C Master Bus ([`main/oled_display.c`](../../main/oled_display.c))
* **Bus Speed:** 400 kHz Fast Mode on GPIO 8 (`SDA`) and GPIO 9 (`SCL`) with external 4.7 kΩ pull-ups.
* **Non-Blocking Framebuffer:** SSD1306 updates are flushed via a low-priority background task on Core 0 at 10 FPS, preventing display I/O from stalling audio processing.

### 2.5 Onboard WS2812B aRGB LED Driver ([`main/led_indicator.c`](../../main/led_indicator.c))
* **Peripheral Engine:** Driven using ESP-IDF v6.1 native `esp_driver_rmt` (Remote Control peripheral) on `GPIO 48` on Core 0.
* **Timing & Resolution:** RMT channel configured at 10 MHz resolution (100 ns/tick) paired with a bytes encoder implementing WS2812B NRZ symbols (Bit 0: 300 ns High / 900 ns Low; Bit 1: 900 ns High / 300 ns Low; 24-bit GRB sequence).
* **Optical Safety & Thermal Clamping:** Peak software brightness is capped to ~25% duty cycle (`MAX_BRIGHTNESS = 60`) per Section 3.5 of `docs/hardware/hardware.md`, eliminating glare while preserving a dynamic range of 60 discrete luminance levels.
* **Loudness & Mute Feedback:**
  - `Muted`: Constant soft red alert (`R=25, G=0, B=0`).
  - `0% Volume`: Completely dark.
  - `1% - 100% Volume`: Luminance tracks volume linearly; color transitions smoothly across an audiophile spectrum (Deep Blue $\rightarrow$ Pure Cyan $\rightarrow$ Vibrant Mint-White).
  - Synchronously driven on every volume change (Rotary, UAC 1.0 host slider, WebHID) and startup chime sequence.

---

## 3. Parametric Equalizer (PEQ) DSP Engine

Implemented in [`main/peq_dsp.c`](../../main/peq_dsp.c), the audio engine provides studio-grade acoustic correction.

### 3.1 Biquad Filter Topologies
Filters are implemented as Robert Bristow-Johnson (RBJ) Audio EQ Cookbook **Direct Form II Transposed** biquads:

$$y[n] = b_0 x[n] + w_1[n-1]$$
$$w_1[n] = b_1 x[n] - a_1 y[n] + w_2[n-1]$$
$$w_2[n] = b_2 x[n] - a_2 y[n]$$

All coefficients are normalized by dividing by $a_0$.
* **Peaking Filter (Parametric Bell):**
  $$\omega_0 = 2\pi \frac{f_0}{f_s}, \quad A = 10^{\frac{\text{Gain}_{\text{dB}}}{40}}, \quad \alpha = \frac{\sin(\omega_0)}{2Q}$$
  $$b_0 = 1 + \alpha A, \quad b_1 = -2\cos(\omega_0), \quad b_2 = 1 - \alpha A$$
  $$a_0 = 1 + \frac{\alpha}{A}, \quad a_1 = -2\cos(\omega_0), \quad a_2 = 1 - \frac{\alpha}{A}$$
* **Low-Shelf Filter:**
  $$b_0 = A\left((A+1) - (A-1)\cos(\omega_0) + 2\sqrt{A}\alpha\right)$$
  $$b_1 = 2A\left((A-1) - (A+1)\cos(\omega_0)\right)$$
  $$b_2 = A\left((A+1) - (A-1)\cos(\omega_0) - 2\sqrt{A}\alpha\right)$$
  $$a_0 = (A+1) + (A-1)\cos(\omega_0) + 2\sqrt{A}\alpha$$
  $$a_1 = -2\left((A-1) + (A+1)\cos(\omega_0)\right)$$
  $$a_2 = (A+1) + (A-1)\cos(\omega_0) - 2\sqrt{A}\alpha$$
* **High-Shelf Filter:**
  $$b_0 = A\left((A+1) + (A-1)\cos(\omega_0) + 2\sqrt{A}\alpha\right)$$
  $$b_1 = -2A\left((A-1) + (A+1)\cos(\omega_0)\right)$$
  $$b_2 = A\left((A+1) + (A-1)\cos(\omega_0) - 2\sqrt{A}\alpha\right)$$
  $$a_0 = (A+1) - (A-1)\cos(\omega_0) + 2\sqrt{A}\alpha$$
  $$a_1 = 2\left((A-1) - (A+1)\cos(\omega_0)\right)$$
  $$a_2 = (A+1) - (A-1)\cos(\omega_0) - 2\sqrt{A}\alpha$$

### 3.2 Extended Topologies, Dual-Mono L/R & Matrix/Crossfeed
* In addition to Peaking, Low-Shelf, and High-Shelf, the engine implements gain-independent RBJ topologies: **High-Pass (HPF, 4)**, **Low-Pass (LPF, 5)**, **Notch (NTCH, 6)**, and **Band-Pass (BPF, 7)**.
* Each channel (`PEQ_CH_LEFT`, `PEQ_CH_RIGHT`) scales independently or linked (`PEQ_CH_LINKED`) between 1 and 10 active bands without heap re-allocation.
* Post-EQ processing applies a `2×2` channel matrix (`Stereo`, `Mono Mix`, `L-Only`, `R-Only`, `Swap L/R`, `Mid-Side Width 0–200%`, `L/R Balance`, `Custom Matrix`) and optional 3-level **Chu Moy Crossfeed** (`Off / Mild / Medium`: 700 Hz 1st-order LPF + ~270 µs circular delay buffer `CROSSFEED_MAX_DELAY = 32`).

### 3.3 15 ms Click-Free Crossfader, A/B Toggle & Cross-Core Staging
To eliminate clicks, pops, or zipper noise when dragging sliders or switching presets:
1. Core 0 computes target biquad and mixer coefficients on the stack outside locks, then atomically stages `s_pending_*` under `portMUX_TYPE` spinlock (`s_peq_lock`) with generation validation (`s_state_gen`).
2. Core 1 (`peq_dsp_process_interleaved_s24_to_s32` / `peq_dsp_process_interleaved_s16_to_s32`) consumes staged updates and runs an interpolation loop across $N_{\text{fade}} = \text{round}(0.015 \times f_s)$ samples ($662 / 720 / 1440$ samples):
   $$y[n] = (1.0 - \beta) \cdot y_{\text{active}}[n] + \beta \cdot y_{\text{target}}[n], \quad \beta \in [0.0, 1.0]$$
3. **Preamp-Normalized Instant A/B Bypass:** `peq_dsp_set_ab_bypass()` toggles flat EQ comparison cleanly across the 15 ms crossfade while keeping `preamp_gain_db` active (transient only, never saved to NVS).
4. `biquad_filter_sample()` flushes subnormal delay states ($< 1 \times 10^{-15}$) to zero, and `calculate_biquad()` validates `isfinite()` / non-zero `a0` with safe passthrough fallback.

### 3.4 Saturation Protection & Pre-Gain Headroom
* **Preamp Attenuation:** Provides $0.0\text{ to }-12.0\text{ dB}$ digital pre-attenuation to prevent large positive biquad boosts from exceeding $0\text{ dBFS}$.
* **Hard Saturation Clamping:** Non-finite samples are zeroed and all floating-point audio samples are clamped to signed 24-bit full-scale bounds (`[-8388608.0f, 8388607.0f]`) before MSB-aligned 32-bit I2S slot output (`(int32_t)((uint32_t)(int32_t)lrintf(val) << 8)`).

### 3.5 8-Slot Editable NVS v3 Persistence & Studio Dynamics
All hardcoded factory presets (`Flat`, `Bass Boost`, `Harman`) have been removed in `v0.2.0`. Instead, `peq_dsp_init()` provisions **8 independently editable, custom-named NVS v3 user slots (`Slot 0..7`)** (`"User 1"`..`"User 8"` seeded to Reference Flat on clean boot, with one-way migration from `v0.1.0` `user_cfg_v2` / V1 into `Slot 0`).

* **Per-Slot NVS Keys:** `peq_slot_0`..`peq_slot_7` storing `linked_stereo`, Left/Right profiles, and CRC32.
* **Global Meta & FIR Keys:** `nvs_meta_v3` stores `active_slot`, Lookahead Limiter enable, Equal-Loudness mode (`Off / Mild / Full`), OLED playback view, `auto_mute_sec`, `oled_dim_sec`, FIR enable, and `peq_mixer_config_t`; `fir_taps_l`/`fir_taps_r` store up to 256 float32 impulse taps per channel (`128` active max at `96 kHz`).
* **Zero Flash Wear During Tuning:** Live WebHID / Android Companion slider updates modify SRAM only. Commits (`WEBHID_CMD_SAVE_TO_PRESET` or long-press Save) check `memcmp()` before writing flash.

---

## 4. Audio Source Arbiter & Zero-Pop Handover

Implemented in [`main/audio_source_arbiter.c`](../../main/audio_source_arbiter.c), the arbiter coordinates operational audio sources using the **4-Phase Zero-Pop Protocol**:

```
 Phase 1: Hardware Soft-Mute (Pull GPIO 7 LOW for 15 ms)
   ▼
 Phase 2: Pipeline Teardown & Buffer Quiescence
   ▼
 Phase 3: DMA Flush & Clock Re-lock (10 ms)
   ▼
 Phase 4: Spin up New Source & Unmute (Pull GPIO 7 HIGH after 15 ms)
```

| Source Identifier | Name | Description |
| :--- | :--- | :--- |
| `AUDIO_SOURCE_STANDALONE` | **USB DAC / DSP** *(Default)* | Consumes incoming UAC 1.0 stream from TinyUSB FIFO, runs PEQ biquads, loudness, FIR, mixer, and lookahead limiter, and outputs 32-bit slots to I2S. |
| `AUDIO_SOURCE_TONE_TEST` | **Tone Generator** | Synthesizes an A minor arpeggio (A4, C5, E5, A5) cycling every 250 ms to evaluate EQ curves without host audio. |
| `AUDIO_SOURCE_SWEEP` | **Frequency Sweep** | Generates a continuous sweep from 100 Hz to 4,000 Hz for acoustic diagnostics. |

---

## 5. User Controls & On-Device OLED UI

Implemented in [`main/controls.c`](../../main/controls.c) and [`main/oled_display.c`](../../main/oled_display.c).

### 5.1 Physical Controls Interaction Matrix

The control subsystem operates across three modes (**Default Playback**, **Main Menu**, and **Live EQ Adjust**):

#### 1. Default Playback Mode (Normal Operation)

| Control Input | Action | Function | UI Response | WS2812B aRGB LED Response |
| :--- | :--- | :--- | :--- | :--- |
| **Rotary Turn** | CW / CCW | Adjust master volume ($\pm 2\%$ per detent) | Volume Slider Overlay (1.5s auto-hide) | Dynamic brightness & color scaling ($1\%\dots 100\%$) |
| **Encoder SW** | Short Click | Toggle Master Mute (`audio_pipeline_set_mute`) | `[MUTED]` flag on OLED header | Soft Red alert (when muted) / restores volume color |
| **Encoder SW** | Double Click | Quick Save to NVS (commits active slot & dynamics) | `"Saved to NVS"` popup overlay | Maintained at current volume loudness |
| **Encoder SW** | Long Press ($>800\text{ ms}$) | Cycle Audio Source (USB DAC $\rightarrow$ Tone Test $\rightarrow$ Sweep) | Audio Source popup overlay | Maintained at current volume loudness |
| **Button 1 (◄)** | Short Click | **Open On-Screen Menu** (enters Main Menu Mode) | Switches OLED body to interactive Menu List | Maintained at current volume loudness |
| **Button 1 (◄)** | Long Press ($>800\text{ ms}$) | Cycle User Slot backward (`Slot 7..0`) | Slot overlay popup | Maintained at current volume loudness |
| **Button 2 (►)** | Short Click | Cycle User Slot forward (`Slot 0..7`) | Slot overlay popup | Maintained at current volume loudness |
| **Button 2 (►)** | Double-Click | Cycle OLED Playback Visualizer (`HUD -> Curve -> VU -> Split`) | View popup overlay & live renderer | Maintained at current volume loudness |

#### 2. Main Menu Navigation Mode (Modal Menu Active)

| Control Input | Action | Function | UI Response | WS2812B aRGB LED Response |
| :--- | :--- | :--- | :--- | :--- |
| **Rotary Turn** | CW / CCW | **Scroll Menu Items** up / down | Inverse highlight bar moves across 11 menu options | Maintained at current volume loudness |
| **Button 2 (►)** | Short Click | **Move Down** menu item list | Selection bar advances to next item | Maintained at current volume loudness |
| **Button 1 (◄)** | Short Click | **Exit Menu / Back** to Playback Mode | Reverts immediately to Default Playback display | Maintained at current volume loudness |
| **Encoder SW** | Short Click | **Confirm / Cycle** | Enters Live EQ Editor or cycles parameter | Maintained at current volume loudness |
| **Encoder SW** | Long Press ($>800\text{ ms}$) | **Quick Save to NVS** | `"Saved to NVS"` popup overlay | Maintained at current volume loudness |

#### 3. Menu Architecture (11 Selectable Options)
1. **1. Live EQ Adjust:** Enters on-device digit-by-digit PEQ editor on active slot.
2. **2. Slot:<Name>:** Cycles NVS v3 User Slots `0..7`.
3. **3. A/B:<Active/Bypass>:** Toggles transient preamp-normalized A/B comparison.
4. **4. Xfeed:<Off/Low/High>:** Cycles Chu Moy headphone crossfeed level.
5. **5. Loud:<Off/Mild/Full>:** Cycles Fletcher-Munson equal-loudness mode.
6. **6. Limiter:<On/Off>:** Toggles 1.5 ms lookahead true-peak limiter (`-0.5 dBFS`).
7. **7. OLED:<HUD/Curve/VU/Split>:** Cycles Playback Body visualizer mode.
8. **8. Preamp:<dB>:** Cycles headroom attenuation ($0.0, -2.0, -4.0, -6.0\text{ dB}$).
9. **9. Src:<Source>:** Cycles `USB DAC` $\rightarrow$ `Tone Generator` $\rightarrow$ `Frequency Sweep`.
10. **10. Save to NVS:** Commits active slot & dynamics config to NVS v3 flash.
11. **11. Exit Menu:** Closes menu and returns to Playback Mode.

#### 4. On-Device EQ Adjust Mode (Live Digit-by-Digit PEQ Tuner)
Triggered by selecting **"1. Live EQ Adjust"** from the Main Menu. Allows comprehensive, standalone tuning of the parametric equalizer without needing a PC or WebHID connection:

| Control Input | Action | Function | UI Response | Audio / DSP Response |
| :--- | :--- | :--- | :--- | :--- |
| **Button 1 (◄)** | Short Click | **Move Cursor Left** | Cursor steps left across digits, sign, type, band, and toolbar items | UI highlight update |
| **Button 2 (►)** | Short Click | **Move Cursor Right** | Cursor steps right across digits, sign, type, band, and toolbar items | UI highlight update |
| **Rotary Turn** | CW / CCW | **Adjust Number (0–9)** on active digit / field | Increments / decrements single digit, toggles sign, cycles type, or scrolls toolbar | Real-time biquad recalculation & 15 ms click-free crossfade |
| **Encoder SW** | Short Click | **Execute Action / Cycle Section** | Triggers toolbar item (`+B`, `-B`, `SAVE`, `EXIT`) or rapidly cycles parameter sections | Executes command or jumps cursor |
| **Encoder SW** | Long Press ($>800\text{ ms}$) | **Instant Save to NVS** | Commits modified parameters to flash; displays `"SAVED TO NVS!"` | Zero flash wear during editing; writes only on save |
| **Button 1 (◄)** | Long Press ($>800\text{ ms}$) | **Cancel & Revert** | Discards unsaved session adjustments and restores pre-edit snapshot | Restores original biquads, exits to Playback |
| **Button 2 (►)** | Double Click | Shortcut: **Add Band** (`+1`) | Increments band count (up to 10 bands); initializes new band | Dynamically scales active biquad loop |
| **Button 1 (◄)** | Double Click | Shortcut: **Remove Band** (`-1`)| Decrements band count (minimum 1 band); removes active band | Dynamically scales active biquad loop |

### 5.2 OLED Display Layout (SSD1306 128x64)

#### Playback Mode (Default Dashboard)
```
  0,0 ┌────────────────────────────────────────────────────────┐ 127,0
      │ [USB DAC] 48.0k 24b                             V: 80% │ Header (y=0..10)
   12 ├────────────────────────────────────────────────────────┤
      │                   ESP32-S3 Audio DSP                   │
      │                     DSP Standalone                     │ Dynamic Body
      │                   [═══════════   ]                     │ (y=14..44)
   48 ├────────────────────────────────────────────────────────┤
      │ EQ: Harman                                        PLAY │ Footer (y=52..63)
  127 └────────────────────────────────────────────────────────┘ 127,63
```

#### Menu Navigation Mode
```
  0,0 ┌────────────────────────────────────────────────────────┐ 127,0
      │                    == MAIN MENU ==                     │ Header (y=0..10)
   12 ├────────────────────────────────────────────────────────┤
      │ [1. Live EQ Adjust                                   ] │ Inverted Selection (y=13..21)
      │  2. Preset:Harman                                      │ Item 2 (y=22..30)
      │  3. Src:USB DAC                                        │ Item 3 (y=31..39)
      │  4. Preamp:-3.0dB                                      │ Item 4 (y=40..48)
   50 ├────────────────────────────────────────────────────────┤
      │ Rot:Move  SW:Select                                    │ Navigation hints (y=52..63)
  127 └────────────────────────────────────────────────────────┘ 127,63
```

#### On-Device EQ Adjust Mode (Live Digit-by-Digit Tuner)
```
  0,0 ┌────────────────────────────────────────────────────────┐ 127,0
      │ BND:[3]/5  [PEAK]                                      │ Header (y=0..9)
   10 ├────────────────────────────────────────────────────────┤
      │ FREQ:  0 1 [0] 0 0  Hz                                 │ Inverted active digit [0]
      │ GAIN:  + 0 4 . 5  dB                                   │ Gain sign & digits
      │ Q   :  0 1 . 4 1                                       │ Q factor digits
      │ < >:Move  Rot:0-9                                      │ Status hint (y=42..49)
   51 ├────────────────────────────────────────────────────────┤
      │ [+B]  [-B]  [SAVE] [EXIT]                              │ Action Toolbar (y=53..63)
  127 └────────────────────────────────────────────────────────┘ 127,63
```
*(Note: The `[TYPE]` field cycles across all 8 filter types: `[BYP ]`, `[PEAK]`, `[L-SH]`, `[H-SH]`, `[HPF ]`, `[LPF ]`, `[NTCH]`, `[BPF ]`.)*

---

## 6. Driverless WebHID Browser Tuner & PWA

Location: [`tools/webhid_tuner/`](../../tools/webhid_tuner/) (`index.html`, `manifest.json`, `sw.js`, `icon.svg`).

The live tuner operates without drivers, native software installations, or cloud dependencies, running directly inside Google Chrome, Microsoft Edge, Brave, Opera, or Android Chrome via the standard **WebHID API** (`navigator.hid`), with installable PWA support when served over HTTPS/`localhost`.

```
 ┌─────────────────────────────────────────────────────────────────────────────┐
 │                         WebHID 64-Byte Packet Protocol                      │
 ├──────────┬──────────┬──────────┬──────────┬──────────┬──────────┬───────────┤
 │ Magic    │ Command  │ Band Idx │ Filter   │ Enabled  │ Freq     │ Gain      │
 │ (0x4551) │ (1 Byte) │ (1 Byte) │ (1 Byte) │ (1 Byte) │ (2 Bytes)│ (2 Bytes) │
 │ 2 Bytes  │ 0x01..13 │ 0..9/FE  │ 0..7     │ 0 or 1   │ 20..20k  │ x10 dB    │
 ├──────────┴──────────┴──────────┴──────────┴──────────┴──────────┴───────────┤
 │ Q Factor (2B, x100) │ reserved[0..51] @ byte 12 (num_bands/ch/flags/mixer/fs)│
 └─────────────────────┴───────────────────────────────────────────────────────┘
```

* **Interactive HTML5 Canvas Graph:** Exact RBJ complex transfer evaluation with Magnitude (`dB`), Phase (`-180°..+180°`), Group Delay (`ms`), draggable nodes, and mouse-wheel Q factor adjustment.
* **Dual-Mono L/R, A/B Toggle & Matrix/Crossfeed:** Linked Stereo or independent Left/Right tuning, transient A/B Bypass (`0x08`), `2×2` Channel Matrix (`0x13`), Chu Moy Crossfeed, and Equalizer APO `.txt` / `.json` import & export.
* **Bidirectional Synchronization:** Automatically syncs hardware state (`num_bands`, active preset, Dual-Mono flags, mixer state, sample rate) upon USB connection.

---

## 7. Multi-Core FreeRTOS Task Layout

The firmware utilizes both cores of the ESP32-S3 to separate hard real-time audio DSP from asynchronous USB, I/O, and UI processing:

| Core ID | Task Name | Function | Stack Budget | Priority | Execution Model |
| :---: | :--- | :--- | :---: | :---: | :--- |
| **Core 0** | `tinyusb_task` | TinyUSB device stack event loop | 4,096 B | 5 | Event-driven (blocking on USB D+/D- interrupts) |
| **Core 0** | `ctrl_disp` | User controls dispatcher & NVS commit | 4,096 B | 4 | Event-driven (blocking on `s_ctrl_queue`) |
| **Core 0** | `oled_ui_task` | SSD1306 I2C framebuffer rendering | 4,096 B | 2 | Periodic (10 FPS non-blocking refresh) |
| **Core 0** | `controls_task` | Rotary encoder poll & button debounce | 3,072 B | 3 | Periodic (50 Hz / 20 ms poll loop) |
| **Core 1** | `audio_dsp_task` | **Real-Time DSP audio generator & I2S DMA** | **8,192 B** | **10** | **Continuous real-time stream loop (`CONFIG_FREERTOS_HZ=1000`)** |

---

## 8. Invariants & Rules for AI Agents & Autonomous Coders

When writing or refactoring C code for this repository, AI agents MUST strictly comply with these micro-architectural invariants:

1. **Octal PSRAM Pin Protection (FATAL):** Never allocate, reconfigure, or route signals to `GPIO 33, 34, 35, 36, 37`. These pins are dedicated to the Octal PSRAM bus (`APS6404L`). Accessing them triggers an unrecoverable `Cache disabled but cached memory region accessed` kernel panic.
2. **Core 1 Isolation for Audio DSP:** The real-time audio loop (`audio_dsp_task`, biquad calculations, I2S DMA feeding) must execute strictly on **Core 1** at high priority (Priority 10). Never run non-deterministic tasks (USB stack, NVS flash commits, display I/O) on Core 1.
3. **Zero Dynamic Allocation in Audio Loops:** Never call `malloc()`, `calloc()`, or `realloc()` inside `audio_dsp_task` or `peq_dsp_process_interleaved_s24_to_s32()`. Pre-allocate all buffers in static BSS or during initialization.
4. **Saturation Clamping:** All floating-point DSP output samples must be clamped to signed 24-bit full-scale bounds (`[-8388608.0f, 8388607.0f]`) before shifting MSB-aligned into 32-bit I2S slots (`int32_t`).
5. **Click-Free Coefficient Updates:** Never swap biquad filter coefficients instantaneously during playback. Always invoke the 15 ms double-buffered linear crossfade engine to avoid audible clicks.
6. **Flash Wear Protection:** Never write to NVS flash inside real-time control loops or WebHID slider dragging callbacks. All live adjustments must remain volatile in SRAM, reserving NVS v2 (`user_cfg_v2`) writes for explicit user commit commands.
7. **Hardware Soft-Mute Protocol:** Always pull `GPIO 7 LOW` before altering I2S clock dividers, switching audio sample rates, or transitioning audio sources. Hold mute for at least 15 ms, reconfigure the pipeline, and hold for 15 ms after unmuting.
8. **I2S Master Clock Synthesizer:** Do NOT configure an external MCLK GPIO on the ESP32-S3. The GY-PCM5102A hardware board has its `SCK` jumper tied to `L` (GND) to utilize its internal low-jitter PLL derived directly from BCK (GPIO 6).
