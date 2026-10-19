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
 │   • Interface 1: USB Audio Streaming (AS) - 48 kHz / 16-bit Stereo PCM      │
 │   • Interface 2: Vendor HID (WebHID) - 64-Byte Bidirectional Reports        │
 ├─────────────────────────────────────────────────────────────────────────────┤
 │ Core 0 (System, Control & Visual Subsystems)                                │
 │   • TinyUSB Device Stack Task (`tinyusb_task`, Priority 5)                  │
 │   • Hardware PCNT Rotary Encoder (GPIO 15/16) + Push Switch (GPIO 17)       │
 │   • Mode Pushbutton Event Poller (GPIO 18)                                  │
 │   • Control Dispatcher Task (`ctrl_disp`, Priority 4)                       │
 │   • SSD1306 128x64 OLED Display Driver (I2C Master: SDA=8, SCL=9, Prio 2)  │
 │   • Onboard WS2812B aRGB LED Volume Loudness Driver (GPIO 48 via RMT)       │
 │   • Audio Source Arbiter State Machine & 4-Phase Mute Protocol              │
 │   • NVS Flash Persistence (Delta-checked Wear Leveling)                     │
 ├─────────────────────────────────────────────────────────────────────────────┤
 │ Core 1 (Hard Real-Time Audio DSP Subsystem)                                 │
 │   • High-Priority Audio DSP Task (`audio_dsp_task`, Priority 10)            │
 │   • UAC 1.0 Isochronous FIFO Stream Consumer (`webhid_peq_audio_read`)      │
 │   • 1-to-10 Band Dynamic PEQ (Direct Form II Transposed Biquads)            │
 │   • 15 ms Click-Free Double-Buffered Linear Crossfader                      │
 │   • Preamp Gain Headroom & Saturation Clamping `[-32768, 32767]`            │
 │   • Master I2S0 DMA Streamer (48,000 Hz, 16-bit Stereo)                     │
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
├── CMakeLists.txt                         # Top-level ESP-IDF project build script
├── sdkconfig.defaults                     # Platform, Flash (QIO 80MHz), and PSRAM configurations
├── docs/                                  # Complete technical specifications & schematics
│   ├── current_firmware_implementation.md # Exhaustive firmware technical record
│   ├── future_enhancements.md             # Roadmap (Matrix Mixer, 24-bit, OLED curves, PWA)
│   ├── hardware.md                        # Electrical pinouts, schematics & jumper configurations
│   ├── parametric_eq_guide.md             # DSP biquad mathematics & filter design manual
│   ├── software.md                        # (This document) Software architecture & HAL reference
│   ├── usb_dac_guide.md                   # USB UAC 1.0 + WebHID stack architecture
│   └── software_functions/                # Modular subsystem function manuals
├── main/
│   ├── CMakeLists.txt                     # Component registration & TinyUSB compile definitions
│   ├── idf_component.yml                  # ESP Component Manager dependencies (`espressif/tinyusb`)
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
│   └── minimp3.h                          # Public-domain zero-dependency MP3 decoder
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
chan_cfg.dma_desc_num  = 4;
chan_cfg.dma_frame_num = 128; // 512 bytes per descriptor

i2s_std_config_t std_cfg = {
    .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(48000),
    .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
    .gpio_cfg = {
        .mclk = I2S_GPIO_UNUSED,
        .bclk = (gpio_num_t)BOARD_I2S_BCK_PIN,       // GPIO 6
        .ws   = (gpio_num_t)BOARD_I2S_WS_PIN,        // GPIO 4
        .dout = (gpio_num_t)BOARD_I2S_DATA_OUT_PIN,  // GPIO 5
        .din  = I2S_GPIO_UNUSED,
    },
};
```
* **DMA FIFO Sizing:** 4 descriptors $\times$ 512 bytes = 2,048 bytes total. At 48 kHz stereo 16-bit ($192\text{ bytes/ms}$), this buffers **10.6 ms** of PCM audio, absorbing FreeRTOS thread scheduling jitter while maintaining low host-to-ear latency.
* **Master Clock Generation:** The Texas Instruments **GY-PCM5102A** DAC generates its internal high-frequency Master Clock (MCLK) via an integrated PLL directly from the Bit Clock (BCK, 1.536 MHz). `mclk` is designated `I2S_GPIO_UNUSED`, conserving GPIOs.
* **Hardware Soft-Mute (`GPIO 7`):** Actively asserted LOW during boot, frequency switches, and source transitions to ensure zero audible pops or clicks.

### 2.2 Native USB OTG Composite Stack ([`main/webhid_peq.c`](../../main/webhid_peq.c))
Operating over Native USB Full-Speed (12.0 Mbps) on GPIO 19 (`D-`) and GPIO 20 (`D+`):
* **Interface Association Descriptor (IAD):** Groups Interface 0 (Audio Control) and Interface 1 (Audio Streaming) as a compliant UAC 1.0 speaker.
* **Interface 0 (Audio Control):** Feature Unit provides Master Volume and Mute control integrated directly into the OS volume mixer.
* **Interface 1 (Audio Streaming):** Isochronous OUT endpoint (`0x01`, 196 max bytes, 1 ms polling) delivering 48 kHz / 16-bit stereo PCM.
* **Interface 2 (Vendor WebHID):** Interrupt endpoints (`EP 0x02 OUT`, `EP 0x82 IN`, 64 bytes) for bidirectional communication with the WebHID browser tuner.

### 2.3 Hardware Pulse Counter (PCNT) Driver ([`main/controls.c`](../../main/controls.c))
Quadrature decoding of the EC11 rotary encoder uses the ESP32-S3 dedicated hardware PCNT peripheral:
* **Glitch Filter:** Hardware filter suppresses contact bounce under $1{,}000\text{ ns}$.
* **Offloaded Decoding:** Core 0/1 CPUs are not interrupted on rotary state transitions; pulses accumulate in hardware counter registers and are polled efficiently at 20 Hz.

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

### 3.2 Dynamic Band Scaling (1 to 10 Bands)
* The engine scales dynamically between 1 and 10 active bands without heap re-allocation.
* The inner processing loop executes strictly across active bands ($k = 0 \dots \text{num\_bands}-1$), consuming $< 2.20\text{ MIPS}$ ($< 4\%$ of Core 1 capacity) at 10 bands.

### 3.3 15 ms Click-Free Double-Buffered Crossfader
To eliminate clicks, pops, or zipper noise when dragging sliders or switching presets:
1. Target biquad coefficients are synthesized into a staging buffer (`target_biquads[]`).
2. An interpolation loop runs across $N_{\text{fade}} = \text{round}(0.015 \times 48000) = 720\text{ samples}$:
   $$y[n] = (1.0 - \beta) \cdot y_{\text{active}}[n] + \beta \cdot y_{\text{target}}[n], \quad \beta \in [0.0, 1.0]$$
3. Upon completion ($\beta = 1.0$), `target_biquads[]` atomically replaces `active_biquads[]`.

### 3.4 Saturation Protection & Pre-Gain Headroom
* **Preamp Attenuation:** Provides $0.0\text{ to }-12.0\text{ dB}$ digital pre-attenuation to prevent large positive biquad boosts from exceeding $0\text{ dBFS}$.
* **Hard Saturation Clamping:** Clamps all floating-point audio samples to signed 16-bit integer boundaries before integer conversion:
  $$\text{Sample}_{\text{clamped}} = \max(-32768, \; \min(32767, \; \text{round}(y[n])))$$

### 3.5 Factory Presets & NVS Flash Persistence
| Preset ID | Name | Configuration |
| :---: | :--- | :--- |
| `0` | **Reference Flat** | 0.0 dB across all bands (bit-perfect passthrough) |
| `1` | **Warm Bass Boost** | +4.5 dB Low Shelf @ 105 Hz ($Q = 0.71$) |
| `2` | **Harman OE2018** | 5-band reference target matching Harman headphone curves |
| `3` | **User Custom** | Persisted in NVS namespace `audio_peq`, key `user_profile` |

* **Zero Flash Wear During Tuning:** Real-time WebHID slider adjustments stream exclusively into volatile SRAM. Committing to NVS requires an explicit user command and performs a CRC32 delta check to skip redundant flash writes.

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
| `AUDIO_SOURCE_STANDALONE` | **USB DAC / DSP** *(Default)* | Consumes incoming UAC 1.0 stream from TinyUSB FIFO, runs PEQ biquads, and outputs to I2S. |
| `AUDIO_SOURCE_TONE_TEST` | **Tone Generator** | Synthesizes an A minor arpeggio (A4, C5, E5, A5) cycling every 250 ms to evaluate EQ curves without host audio. |
| `AUDIO_SOURCE_SWEEP` | **Frequency Sweep** | Generates a continuous linear/logarithmic sweep from 100 Hz to 4,000 Hz for acoustic diagnostics. |

---

## 5. User Controls & On-Device OLED UI

Implemented in [`main/controls.c`](../../main/controls.c) and [`main/oled_display.c`](../../main/oled_display.c).

### 5.1 Physical Controls Interaction Matrix

The control subsystem operates as a clean modal state machine across three distinct modes (**Default Playback Mode**, **Main Menu Navigation Mode**, and **On-Device Live EQ Adjust Mode**):

#### 1. Default Playback Mode (Normal Operation)
In the default playback state, the rotary encoder and switch dedicatedly serve real-time listening controls:

| Control Input | Action | Function | UI Response | WS2812B aRGB LED Response |
| :--- | :--- | :--- | :--- | :--- |
| **Rotary Turn** | CW / CCW | Adjust master volume ($\pm 2\%$ per detent) | Volume Slider Overlay (1.5s auto-hide) | Dynamic brightness & color scaling ($1\%\dots 100\%$) |
| **Encoder SW** | Short Click | Toggle Master Mute (`audio_pipeline_set_mute`) | `[MUTED]` flag on OLED header | Soft Red alert (when muted) / restores volume color |
| **Encoder SW** | Double Click | Quick Save to NVS (commits active profile to flash) | `"Saved to NVS"` popup overlay | Maintained at current volume loudness |
| **Encoder SW** | Long Press ($>800\text{ ms}$) | Cycle Audio Source (USB DAC $\rightarrow$ Tone Test $\rightarrow$ Sweep) | Audio Source popup overlay | Maintained at current volume loudness |
| **Button 1 (◄)** | Short Click | **Open On-Screen Menu** (enters Main Menu Mode) | Switches OLED body to interactive Menu List | Maintained at current volume loudness |
| **Button 1 (◄)** | Long Press ($>800\text{ ms}$) | Cycle EQ Preset backward | Preset overlay popup | Maintained at current volume loudness |
| **Button 2 (►)** | Short Click | Cycle EQ Preset forward (Flat $\rightarrow$ Bass Boost $\rightarrow$ Harman $\rightarrow$ Custom) | Preset overlay popup | Maintained at current volume loudness |

#### 2. Main Menu Navigation Mode (Modal Menu Active)
Triggered by clicking **Button 1 (◄)** (GPIO 18). In this mode, the rotary knob and buttons navigate system parameters:

| Control Input | Action | Function | UI Response | WS2812B aRGB LED Response |
| :--- | :--- | :--- | :--- | :--- |
| **Rotary Turn** | CW / CCW | **Scroll Menu Items** up / down | Inverse highlight bar moves across menu options | Maintained at current volume loudness |
| **Button 2 (►)** | Short Click | **Move Down** menu item list | Selection bar advances to next item | Maintained at current volume loudness |
| **Button 1 (◄)** | Short Click | **Exit Menu / Back** to Playback Mode | Reverts immediately to Default Playback display | Maintained at current volume loudness |
| **Encoder SW** | Short Click | **Confirm / Enter Selection** | Enters editor, cycles parameter, or exits | Maintained at current volume loudness |
| **Encoder SW** | Long Press ($>800\text{ ms}$) | **Quick Save to NVS** | `"Saved to NVS"` popup overlay | Maintained at current volume loudness |

#### 3. Menu Architecture & Selectable Options
When the Menu is opened, the EC11 rotary knob scrolls through the following options:
1. **1. Live EQ Adjust:**
   - Enters on-device live digit-by-digit PEQ editor.
2. **2. Preset:<Name>:**
   - Cycles: `Flat` $\rightarrow$ `Bass Boost` $\rightarrow$ `Harman` $\rightarrow$ `User Custom`.
3. **3. Src:<Source>:**
   - Cycles: `USB DAC` $\rightarrow$ `Tone Test` $\rightarrow$ `Frequency Sweep`.
4. **4. Preamp:<dB>:**
   - Cycles headroom attenuation: $0.0\text{ dB} \rightarrow -2.0\text{ dB} \rightarrow -4.0\text{ dB} \rightarrow -6.0\text{ dB}$.
5. **5. Save to NVS:**
   - Commits active User Custom PEQ profile to persistent flash.
6. **6. Exit Menu:**
   - Closes the menu and returns to the standard playback dashboard.

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
      │ [USB DAC] 48.0k 16b                             V: 80% │ Header (y=0..10)
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
  127 └────────────────────────────────────────────────────────┘ 127,63
```



---

## 6. Driverless WebHID Browser Tuner

Location: [`tools/webhid_tuner/index.html`](../../tools/webhid_tuner/index.html).

The live tuner operates without drivers, native software installations, or cloud dependencies, running directly inside Google Chrome, Microsoft Edge, Brave, Opera, or Android Chrome via the standard **WebHID API** (`navigator.hid`).

```
 ┌─────────────────────────────────────────────────────────────────────────────┐
 │                         WebHID 64-Byte Packet Protocol                      │
 ├──────────┬──────────┬──────────┬──────────┬──────────┬──────────┬───────────┤
 │ Magic    │ Command  │ Band Idx │ Filter   │ Enabled  │ Freq     │ Gain      │
 │ (0x4551) │ (1 Byte) │ (1 Byte) │ (1 Byte) │ (1 Byte) │ (2 Bytes)│ (2 Bytes) │
 │ 2 Bytes  │ 0x01..07 │ 0..9     │ 0..3     │ 0 or 1   │ 20..20k  │ x10 dB    │
 ├──────────┴──────────┴──────────┴──────────┴──────────┴──────────┴───────────┤
 │  Q Factor (2 Bytes, x100)  │  Reserved / Payload (52 Bytes, num_bands)      │
 └────────────────────────────┴────────────────────────────────────────────────┘
```

* **Interactive HTML5 Canvas Graph:** Real-time curve rendering with draggable frequency/gain nodes and mouse-wheel Q factor adjustment.
* **Dynamic Band Scaling:** Add/remove bands (1 to 10) on the fly with presets for 3-band, 5-band, 7-band, and 10-band setups.
* **Bidirectional Synchronization:** Automatically syncs hardware state upon USB connection.

---

## 7. Multi-Core FreeRTOS Task Layout

The firmware utilizes both cores of the ESP32-S3 to separate hard real-time audio DSP from asynchronous USB, I/O, and UI processing:

| Core ID | Task Name | Function | Stack Budget | Priority | Execution Model |
| :---: | :--- | :--- | :---: | :---: | :--- |
| **Core 0** | `tinyusb_task` | TinyUSB device stack event loop | 4,096 B | 5 | Event-driven (blocking on USB D+/D- interrupts) |
| **Core 0** | `ctrl_disp` | User controls dispatcher & NVS commit | 3,072 B | 4 | Event-driven (blocking on `s_ctrl_queue`) |
| **Core 0** | `oled_task` | SSD1306 I2C framebuffer rendering | 3,072 B | 2 | Periodic (10 FPS non-blocking refresh) |
| **Core 0** | `pcnt_worker` | Rotary encoder poll & button debounce | 2,048 B | 3 | Periodic (20 Hz timer) |
| **Core 1** | `audio_dsp_task` | **Real-Time DSP audio generator & I2S DMA** | **4,096 B** | **10** | **Continuous real-time stream loop** |

---

## 8. Invariants & Rules for AI Agents & Autonomous Coders

When writing or refactoring C code for this repository, AI agents MUST strictly comply with these micro-architectural invariants:

1. **Octal PSRAM Pin Protection (FATAL):** Never allocate, reconfigure, or route signals to `GPIO 33, 34, 35, 36, 37`. These pins are dedicated to the Octal PSRAM bus (`APS6404L`). Accessing them triggers an unrecoverable `Cache disabled but cached memory region accessed` kernel panic.
2. **Core 1 Isolation for Audio DSP:** The real-time audio loop (`audio_dsp_task`, biquad calculations, I2S DMA feeding) must execute strictly on **Core 1** at high priority (Priority 10). Never run non-deterministic tasks (USB stack, NVS flash commits, display I/O) on Core 1.
3. **Zero Dynamic Allocation in Audio Loops:** Never call `malloc()`, `calloc()`, or `realloc()` inside `audio_dsp_task` or `peq_dsp_process_interleaved_s16()`. Pre-allocate all buffers in static BSS or during initialization.
4. **Saturation Clamping:** All floating-point DSP output samples must be clamped to signed 16-bit integer bounds (`[-32768, 32767]`) before casting to `int16_t` to prevent catastrophic numerical rollover distortion.
5. **Click-Free Coefficient Updates:** Never swap biquad filter coefficients instantaneously during playback. Always invoke the 15 ms double-buffered linear crossfade engine to avoid audible clicks.
6. **Flash Wear Protection:** Never write to NVS flash inside real-time control loops or WebHID slider dragging callbacks. All slider adjustments must be applied volatilely in SRAM, reserving flash writes for explicit user commit commands.
7. **Hardware Soft-Mute Protocol:** Always pull `GPIO 7 LOW` before altering I2S clock dividers, switching audio sample rates, or transitioning audio sources. Hold mute for at least 15 ms, reconfigure the pipeline, and hold for 15 ms after unmuting.
8. **I2S Master Clock Synthesizer:** Do NOT configure an external MCLK GPIO on the ESP32-S3. The GY-PCM5102A hardware board has its `SCK` jumper tied to `L` (GND) to utilize its internal low-jitter PLL derived directly from BCK (GPIO 6).
