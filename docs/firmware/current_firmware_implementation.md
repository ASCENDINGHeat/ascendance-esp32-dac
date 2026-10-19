# ESP32-S3 High-Fidelity USB DAC & DSP: Current Firmware Implementation

This document provides a comprehensive technical record of the firmware as currently implemented, compiled, and verified on the **YD-ESP32-S3 (N16R8)** hardware platform.

---

## 1. System Overview & Hardware Architecture

The firmware implements **Schematic 2 (`docs/schematics/DSP+DAC+SCREEN.svg`)**, transforming the ESP32-S3 into a driverless, high-resolution **USB Audio Class 1.0 (UAC 1.0) DAC** with a **dynamic 1-to-10 band real-time Parametric Equalizer (PEQ)** and simultaneous **WebHID live tuning** over a single USB-C connection.

```
                           YD-ESP32-S3 (N16R8)
 ┌─────────────────────────────────────────────────────────────────────────────┐
 │ Native USB OTG (GPIO 19: D-, GPIO 20: D+)                                   │
 │   • Interface 0: USB Audio Control (AC) - Mute & Volume Entity              │
 │   • Interface 1: USB Audio Streaming (AS) - 48 kHz / 16-bit Stereo PCM      │
 │   • Interface 2: Vendor HID (WebHID) - 64-Byte Raw Report Bi-Directional    │
 ├─────────────────────────────────────────────────────────────────────────────┤
 │ Core 0 (System & Control Subsystems)                                        │
 │   • TinyUSB Background Task & Event Dispatcher                              │
 │   • Hardware PCNT Rotary Encoder Decoder (GPIO 15/16) + Push Switch (17)    │
 │   • Mode Pushbutton Handler (GPIO 18)                                       │
 │   • SSD1306 128x64 OLED Display Driver (I2C Master: SDA=8, SCL=9)          │
 │   • Onboard WS2812B aRGB LED Volume Loudness Indicator (GPIO 48 via RMT)   │
 │   • Audio Source Arbiter & Zero-Pop Handover Protocol                       │
 │   • NVS Flash Persistence (User Profile & Delta Commit)                     │
 ├─────────────────────────────────────────────────────────────────────────────┤
 │ Core 1 (Real-Time Audio DSP Subsystem)                                      │
 │   • UAC 1.0 FIFO Stream Consumer (`tud_audio_read`)                         │
 │   • 1-to-10 Band Dynamic Parametric Equalizer (Direct Form II Biquads)      │
 │   • 15 ms Click-Free Double-Buffered Linear Crossfader                      │
 │   • Float Saturation Clamping `[-32768, 32767]`                             │
 │   • Master I2S0 DMA Audio Pipeline (48,000 Hz, 16-bit Stereo)               │
 ├─────────────────────────────────────────────────────────────────────────────┤
 │ External Audio Hardware Interconnects                                       │
 │   • GPIO 4: Master I2S Word Select / LCK (WS)  ────────► GY-PCM5102A DAC   │
 │   • GPIO 5: Master I2S Serial Data (DIN/DOUT) ────────► (32-bit / 384kHz)   │
 │   • GPIO 6: Master I2S Bit Clock (BCK)        ────────►                     │
 │   • GPIO 7: Hardware Soft Mute (Active-LOW)   ────────►                     │
 │                                                              │ Analog Out    │
 │                                                              ▼               │
 │                                                       TDA1308 Stereo Amp     │
 │                                                       (3.5mm Headphone Jack) │
 └─────────────────────────────────────────────────────────────────────────────┘
```

---

## 2. Master Pinout & Hardware Mapping

| GPIO Pin | Function | Peripheral | Electrical Characteristics |
| :---: | :--- | :--- | :--- |
| **GPIO 4** | I2S Word Select (`WS` / `LCK`) | Master I2S0 | CMOS Output (48 kHz frame clock) |
| **GPIO 5** | I2S Serial Data Out (`DOUT` / `DIN`) | Master I2S0 | CMOS Output (16-bit interleaved stereo PCM) |
| **GPIO 6** | I2S Continuous Bit Clock (`BCK`) | Master I2S0 | CMOS Output (1.536 MHz: $48000 \times 32$ bits) |
| **GPIO 7** | DAC Hardware Soft Mute (`XMT` / `MUTE`) | GPIO Output | Active-LOW (0 = Mute, 1 = Unmute); Pulled down on boot |
| **GPIO 8** | I2C Serial Data (`SDA`) | I2C Master 0 | Open-drain, 400 kHz fast mode, external 4.7 kΩ pull-up |
| **GPIO 9** | I2C Serial Clock (`SCL`) | I2C Master 0 | Open-drain, 400 kHz fast mode, external 4.7 kΩ pull-up |
| **GPIO 15** | Rotary Encoder Phase A | PCNT Unit 0 | Input, internal pull-up, 1000 ns hardware glitch filter |
| **GPIO 16** | Rotary Encoder Phase B | PCNT Unit 0 | Input, internal pull-up, 1000 ns hardware glitch filter |
| **GPIO 17** | Rotary Push Switch (`SW`) | GPIO Input | Active-LOW, internal pull-up, debounced |
| **GPIO 18** | Button 1 (`BTN 1` / `SW6`) | GPIO Input | Active-LOW, 10k hardware pull-up (`R8`), debounced; Navigation Left (◄) / Menu / Back |
| **GPIO 10** | Button 2 (`BTN 2` / `SW_NAV2`) | GPIO Input | Active-LOW, internal pull-up enabled, debounced; Navigation Right (►) / Next / Preset Cycle |
| **GPIO 19** | Native USB Data Negative (`D-`) | USB OTG PHY | Dedicated USB Full-Speed transceiver differential pair |
| **GPIO 20** | Native USB Data Positive (`D+`) | USB OTG PHY | Dedicated USB Full-Speed transceiver differential pair |
| **GPIO 43** | UART0 RX (Console Debug) | UART0 | Connected to CH343 USB-to-UART bridge on COM port |
| **GPIO 44** | UART0 TX (Console Debug) | UART0 | Connected to CH343 USB-to-UART bridge on COM port |
| **GPIO 48** | Onboard WS2812B aRGB LED | RMT Peripheral | Single-wire 800 kHz NRZ protocol; dynamic volume loudness brightness & mute indicator |

---

## 3. USB Subsystem: Composite UAC 1.0 + WebHID

Implemented in [`main/webhid_peq.c`](../../main/webhid_peq.c).

### 3.1 USB Device & Interface Association Descriptor (IAD)
To ensure driverless plug-and-play operation across Windows 10/11, macOS, Linux, and Android, the device advertises the standard USB Interface Association Descriptor (IAD) class:
* **Vendor ID (VID):** `0x303A` (Espressif Systems)
* **Product ID (PID):** `0x4002` (TinyUSB Audio + HID Composite)
* **Device Class:** `0xEF` (`TUSB_CLASS_MISC`)
* **Device SubClass:** `0x02` (`MISC_SUBCLASS_COMMON`)
* **Device Protocol:** `0x01` (`MISC_PROTOCOL_IAD`)
* **Strings:**
  - `0`: Language English (`0x0409`)
  - `1`: `"Ascension Audio"` (Manufacturer)
  - `2`: `"ESP32-S3 Hi-Fi DSP DAC"` (Product)
  - `3`: `"123456"` (Serial Number)
  - `4`: `"Ascension Hi-Fi Audio"` (Audio Interface)
  - `5`: `"WebHID Live PEQ Interface"` (HID Interface)

### 3.2 Configuration Descriptor Structure (153 Bytes Total)
1. **Config Descriptor Header:** 9 bytes (`TUD_CONFIG_DESCRIPTOR(1, 3, 0, 153, 0x00, 100)`).
2. **IAD (Interface Association Descriptor):** 8 bytes grouping Interface 0 (AC) and Interface 1 (AS).
3. **Interface 0: Audio Control (AC):**
   - Standard AC Interface (9 bytes, 0 endpoints).
   - Class-Specific AC Header (9 bytes, $bcdADC = 0x0100$, links Interface 1).
   - Input Terminal (12 bytes, Terminal ID `0x01`, USB Streaming, 2 channels, Stereo L/R).
   - Output Terminal (9 bytes, Terminal ID `0x03`, Generic Speaker, source Feature Unit `0x02`).
   - Feature Unit (13 bytes, Unit ID `0x02`, Mute + Volume controls on Master, Channel 1, and Channel 2).
4. **Interface 1: Audio Streaming (AS):**
   - Alternate Setting 0: Zero-bandwidth idle interface (9 bytes, 0 endpoints).
   - Alternate Setting 1: Active audio streaming interface (9 bytes, 1 endpoint).
   - Class-Specific AS Header (7 bytes, PCM format, 1 frame delay, links Terminal `0x01`).
   - Type I Format Descriptor (11 bytes, 2 channels, 2 bytes/sample, 16-bit resolution, 48,000 Hz).
   - Standard Isochronous Audio Data Endpoint (9 bytes, `EP 0x01 OUT`, Adaptive, 196 max bytes, 1 ms polling).
   - Class-Specific Isochronous Endpoint (7 bytes, sampling frequency control).
5. **Interface 2: Vendor HID (WebHID):**
   - Standard HID Interface Descriptor (32 bytes, `EP 0x02 OUT`, `EP 0x82 IN`, 64-byte packets, 1 ms polling).
   - 64-byte Generic In/Out Raw Report Descriptor.

### 3.3 UAC 1.0 Callbacks & Host Synchronization
* **`tud_audio_set_itf_cb`:** Tracks when the host activates Alternate Setting 1 (active playback) vs Alternate Setting 0 (idle). Displays `"USB DAC Active"` overlay on the OLED screen.
* **`tud_audio_get_req_entity_cb`:** Responds to host volume queries:
  - `GET_CUR`: Returns current volume in 1/256 dB units.
  - `GET_MIN`: Returns $-90\text{ dB}$ ($-23040$).
  - `GET_MAX`: Returns $0\text{ dB}$ ($0$).
  - `GET_RES`: Returns $1.0\text{ dB}$ ($256$).
* **`tud_audio_set_req_entity_cb`:** Synchronizes host volume slider and mute status directly into the firmware pipeline:
  - Host mute triggers `audio_pipeline_set_mute()` (hardware pin GPIO 7).
  - Host volume slider maps $-90\text{ dB}\dots 0\text{ dB}$ directly to firmware software volume $0\%\dots 100\%$.
* **`tud_audio_get_req_ep_cb` / `tud_audio_set_req_ep_cb`:** Reports and confirms the 48,000 Hz discrete sample rate to host audio engines (PipeWire, ALSA, CoreAudio, Windows WASAPI).

---

## 4. Parametric Equalizer (PEQ) DSP Engine

Implemented in [`main/peq_dsp.c`](../../main/peq_dsp.c).

### 4.1 Filter Topologies & Biquad Formulations
Filters follow Robert Bristow-Johnson (RBJ) Audio EQ Cookbook Direct Form II Transposed biquads:

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

### 4.2 Dynamic Band Count Scaling (1 to 10 Bands)
* Configurable dynamically at runtime via WebHID `CMD_SET_BAND_COUNT` or preset load.
* Execution loop runs strictly over active bands ($k = 0 \dots \text{num\_bands}-1$):
  $$\text{Execution Time (10 Bands)} \approx 2.20\text{ MIPS} \quad (< 4\%\text{ of Core 1 capacity})$$
* Dynamic band memory remains static in internal SRAM (zero heap fragmentation).

### 4.3 Double-Buffered 15 ms Click-Free Crossfader
When parameters change or band counts scale:
1. New biquads are computed into a secondary `target_biquads[]` buffer.
2. An interpolation loop runs across $N_{\text{fade}} = \text{round}(0.015 \times 48000) = 720\text{ samples}$:
   $$y[n] = (1.0 - \beta) \cdot y_{\text{active}}[n] + \beta \cdot y_{\text{target}}[n], \quad \beta \in [0.0, 1.0]$$
3. When $\beta = 1.0$, `target_biquads[]` atomically replaces `active_biquads[]`.

### 4.4 Saturation Protection & Pre-Gain Headroom
* **Pre-Gain Headroom:** Pre-attenuates digital input ($0.0\text{ to }-12.0\text{ dB}$) to prevent positive EQ boost stages from exceeding $0\text{ dBFS}$.
* **Hard Saturation Clamping:** Every output sample is clamped to signed 16-bit integer boundaries before integer conversion:
  $$\text{Sample}_{\text{clamped}} = \max(-32768, \; \min(32767, \; \text{round}(y[n])))$$

### 4.5 Built-In Presets & NVS Flash Persistence
1. **Preset 0 (Reference Flat):** 0.0 dB gain across all frequencies (bit-perfect passthrough).
2. **Preset 1 (Warm Bass Boost):** +4.5 dB Low Shelf @ 105 Hz ($Q = 0.71$).
3. **Preset 2 (Harman OE2018 Target):** 5-band reference target curve matching Harman headphone standards.
4. **Preset 3 (User Custom):** Persisted in NVS namespace `audio_peq`, key `user_profile`. Saved with CRC32 verification and flash delta checks to guarantee 0 unneeded flash wear during live slider dragging.

---

## 5. Audio Pipeline & Master I2S0 Subsystem

Implemented in [`main/audio_pipeline.c`](../../main/audio_pipeline.c).

* **I2S Configuration:**
  - Standard Philips I2S format (`I2S_COMM_MODE_STAND_I2S`).
  - Bit depth: 16-bit stereo (`I2S_DATA_BIT_WIDTH_16BIT`).
  - Clock frequency: **48,000 Hz**.
  - Clock Source: Internal PLL (`I2S_CLK_SRC_DEFAULT`).
  - MCLK: Synthesized internally by PCM5102A's internal PLL from BCK.
* **DMA Buffering:**
  - 4 DMA descriptors $\times$ 512 bytes = 2048 bytes DMA FIFO ($~10.6\text{ ms}$ buffer cushion).
* **Hardware Soft-Mute Protocol (GPIO 7):**
  - Pin pulled LOW on boot to isolate DAC analog output while internal rails charge.
  - Asserted during source handover and rate changes to guarantee zero audible pops or clicks.
* **Master Volume Engine:**
  - Logarithmic / linear perceptually-weighted volume multiplier ($0\%\dots 100\%$).
* **Startup Verification Chime:**
  - Synthesizes an ascending four-note chime (C5 $523.25\text{ Hz}$, E5 $659.25\text{ Hz}$, G5 $783.99\text{ Hz}$, C6 $1046.50\text{ Hz}$) on boot to verify end-to-end analog hardware integrity.

---

## 6. Audio Source Arbiter & Zero-Pop Handover

Implemented in [`main/audio_source_arbiter.c`](../../main/audio_source_arbiter.c).

The system coordinates three operational audio modes using the **4-Phase Zero-Pop Muting Protocol**:

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
| `AUDIO_SOURCE_STANDALONE` | **USB DAC / DSP** *(Default Boot)* | Consumes incoming UAC 1.0 stream from TinyUSB FIFO, runs PEQ biquads, and writes to I2S. Sleeps 2 ms when idle. |
| `AUDIO_SOURCE_TONE_TEST` | **Tone Generator** | Synthesizes an A minor arpeggio (A4, C5, E5, A5 cycling every 250 ms) to audition PEQ curves without an active audio source. |
| `AUDIO_SOURCE_SWEEP` | **Frequency Sweep** | Generates a continuous linear/logarithmic sweep from 100 Hz to 4000 Hz for acoustic diagnostics. |

---

## 7. User Controls & Hardware UI

Implemented in [`main/controls.c`](../../main/controls.c), [`main/oled_display.c`](../../main/oled_display.c), and [`main/main.c`](../../main/main.c).

### 7.1 Physical Controls: 2 Navigation Buttons + EC11 Quadrature Encoder (Core 0)
The control subsystem operates as a clean modal state machine across 3 distinct operating modes (**Default Playback Mode**, **Main Menu Navigation Mode**, and **Live EQ Adjust Mode**):

#### 1. Default Playback Mode (Normal Operation)
* **Rotary Rotation (GPIO 15/16):** Decoded via ESP32-S3 hardware Pulse Counter (`PCNT Unit 0`). Adjusts master volume in $\pm 2\%$ increments with instant on-screen slider popup and dynamic WS2812B brightness scaling.
* **Encoder Push Switch (GPIO 17):**
  - **Short Click:** Toggles Master Mute (`audio_pipeline_set_mute`) and displays soft Red alert on the WS2812B LED.
  - **Double Click:** Quick NVS Save of active PEQ profile to flash (`"Saved to NVS"`).
  - **Long Press ($>800\text{ ms}$):** Cycles audio source (`DSP Standalone / USB DAC` $\rightarrow$ `Tone Test` $\rightarrow$ `Frequency Sweep`).
* **Button 1 (GPIO 18 / ◄ / SW6):**
  - **Short Click:** **Opens Main Menu** (`UI_MODE_MENU`).
  - **Long Press ($>800\text{ ms}$):** Cycles EQ preset backward (Custom $\rightarrow$ Harman $\rightarrow$ Bass Boost $\rightarrow$ Flat).
* **Button 2 (GPIO 10 / ► / SW_NAV2):**
  - **Short Click:** Cycles EQ preset forward (Flat $\rightarrow$ Bass Boost $\rightarrow$ Harman $\rightarrow$ Custom).

#### 2. Main Menu Navigation Mode
* **Rotary Rotation (GPIO 15/16):** Scrolls the cursor through selectable options (`1. Live EQ Adjust`, `2. Preset:<Name>`, `3. Src:<Source>`, `4. Preamp:<dB>`, `5. Save to NVS`, `6. Exit Menu`).
* **Encoder Switch (GPIO 17):**
  - **Short Click:** **Confirms / Enters** the highlighted menu item:
    - `1. Live EQ Adjust`: Enters Live EQ Adjust Mode.
    - `2. Preset`: Cycles EQ presets forward.
    - `3. Src`: Cycles audio input source.
    - `4. Preamp`: Cycles preamp attenuation (0.0 dB $\rightarrow$ -2.0 dB $\rightarrow$ -4.0 dB $\rightarrow$ -6.0 dB).
    - `5. Save to NVS`: Commits active User Custom profile to NVS flash.
    - `6. Exit Menu`: Returns to Playback Mode.
  - **Long Press ($>800\text{ ms}$):** Instant NVS Save.
* **Button 1 (GPIO 18 / ◄):** **Exit Menu / Back** to Playback Mode.
* **Button 2 (GPIO 10 / ►):** Scroll down menu list.

#### 3. On-Device Live EQ Adjust Mode (Digit-by-Digit Laboratory Synthesizer / PEQ Tuner)
Triggered by selecting **"1. Live EQ Adjust"** in the Main Menu:
* **Layout on SSD1306 OLED (128x64):**
  - **Line 0:** `BND:[3]/5  [PEAK]` (Active band index, total band count, filter type).
  - **Line 1:** `FREQ:  0 1 0 0 0  Hz` (5 individual selectable digits: 10k, 1k, 100, 10, 1).
  - **Line 2:** `GAIN:  + 0 4 . 5  dB` (Selectable Sign `+`/`-`, Tens, Ones, Tenths).
  - **Line 3:** `Q   :  0 1 . 4 1` (Selectable Integer, Tenths, Hundredths).
  - **Line 4:** `< >:Move  Rot:0-9` (Status & navigation hint).
  - **Line 5:** Bottom Action Toolbar: `[+B]  [-B]  [SAVE] [EXIT]`
* **Navigation & Adjustment Matrix:**
  - **Button 1 (GPIO 18 / ◄):** Moves cursor Left across fields, digits, and toolbar items.
  - **Button 2 (GPIO 10 / ►):** Moves cursor Right across fields, digits, and toolbar items.
  - **Rotary Rotation (GPIO 15/16):**
    - When cursor is on a **Digit (`0–9`)**: Dials the selected digit up/down with instantaneous biquad recalculation and 15 ms click-free crossfading on Core 1!
    - When on **Band**: Switches active band (`1..N`).
    - When on **Type**: Cycles filter type (`PEAK` $\rightarrow$ `L-SH` $\rightarrow$ `H-SH`).
    - When on **Sign**: Toggles `+` and `-`.
    - When on **Toolbar**: Rotates highlighted toolbar action (`[+B]`, `[-B]`, `[SAVE]`, `[EXIT]`).
  - **Encoder Switch (GPIO 17):**
    - **Short Click:** If on a toolbar action, triggers it (`+B` adds band, `-B` removes band, `SAVE` commits to NVS, `EXIT` returns to playback). If on a parameter, rapidly jumps to the next section (`Band` $\rightarrow$ `Freq` $\rightarrow$ `Gain` $\rightarrow$ `Q` $\rightarrow$ `Toolbar`).
    - **Long Press ($>800\text{ ms}$):** **Instant Save to NVS** (commits to flash with auto-dismissing `"SAVED TO NVS!"` popup).
  - **Button 1 Long Press ($>800\text{ ms}$):** **Cancel & Revert** (discards all unsaved session changes and restores previous profile).
  - **Button 1 Double Click:** Direct shortcut to **Remove Band** (`[-B]`).
  - **Button 2 Double Click:** Direct shortcut to **Add Band** (`[+B]`).
* **Zero Flash Wear Invariant:** Real-time biquad adjustments remain volatile in SRAM until the user explicitly saves.

### 7.2 SSD1306 128x64 Monochrome OLED Display Modes
* Driven over I2C Master (SDA=GPIO 8, SCL=GPIO 9) at 400 kHz with non-blocking 10 FPS rendering task on Core 0.
* **Supported Screen Modes:**
  1. **Playback Mode:** System badge, bit depth/sample rate, track title, subtitle, volume progress bar, volume & preset overlay popups, and footer status.
  2. **Main Menu Mode:** 4-row scrolling viewport with highlighted selection bar, active preset name, current source, preamp gain readout, and navigation footer.
  3. **Live EQ Adjust Mode:** Full-screen digit-by-digit editor with inverted highlight on active field/digit, dynamic parameter indicators, and bottom action buttons.

### 7.3 Onboard WS2812B aRGB LED Volume Loudness Indicator
Implemented in [`main/led_indicator.c`](../../main/led_indicator.c) and [`main/led_indicator.h`](../../main/led_indicator.h).

* **Hardware Mapping:** Onboard `XL-5050RGBC-WS2812B` LED connected to **`GPIO 48`** via factory-bridged `RGB` solder jumper.
* **Driver Architecture:** Uses ESP-IDF v6.1 `esp_driver_rmt` Remote Control peripheral configured at 10 MHz resolution (100 ns tick). A custom bytes encoder drives the 800 kHz NRZ protocol (Bit 0: 300 ns High / 900 ns Low; Bit 1: 900 ns High / 300 ns Low; End-of-transmission Low for latching).
* **Optical Safety Invariant:** WS2812B LEDs at 100% duty cycle produce excessive heat and uncomfortable glare. In accordance with Section 3.5 of `docs/hardware/hardware.md`, maximum brightness is capped in firmware to **~25% duty cycle** (`MAX_BRIGHTNESS = 60` out of 255).
* **Audio-Visual Loudness Mapping:**
  - **Muted State:** Soft Red glow (`R=25, G=0, B=0`), providing immediate visual indication of mute state without checking the OLED screen.
  - **0% Volume:** LED completely OFF (`R=0, G=0, B=0`).
  - **1% – 35% Volume (Subtle Glow):** Deep Blue transitioning into Teal.
  - **36% – 75% Volume (Nominal Listening):** Vibrant Pure Cyan tracking output amplitude.
  - **76% – 100% Volume (Dynamic Peak):** Electric Cyan-Mint with subtle white highlights at maximum output.
* **Global Synchronization:** Updates synchronously upon EC11 rotary knob movement, mute toggles, host OS volume changes via UAC 1.0 Feature Unit, and WebHID tuning events.
* **Boot Animation:** Synchronizes color transitions across the ascending 4-note startup chime (C5 $\rightarrow$ E5 $\rightarrow$ G5 $\rightarrow$ C6) before returning to the active volume level.

---

## 8. Driverless WebHID Live Tuner Client

Location: [`tools/webhid_tuner/index.html`](../../tools/webhid_tuner/index.html).

* **Zero-Driver / Zero-Installation Architecture:** Runs client-side in Google Chrome, Microsoft Edge, Brave, Opera, or Android Chrome via the standard WebHID API (`navigator.hid`).
* **Connection Workflow:**
  - Re-uses paired devices via `navigator.hid.getDevices()` without triggering repetitive browser permission dialogs.
  - Comprehensive Linux `/dev/hidraw` diagnostic logging with udev rule verification tips.
* **Interactive HTML5 Canvas Frequency Response Graph:**
  - **Draggable Nodes:** Directly drag center frequency ($X$-axis, logarithmic 20 Hz to 20 kHz) and gain ($Y$-axis, $-24.0\text{ dB}\dots +12.0\text{ dB}$).
  - **Mouse Wheel / Trackpad Scroll Q Adjustment:** Adjusts Q factor ($0.10\dots 10.00$) in real time with dynamic visual $-3\text{ dB}$ passband bandwidth guides.
  - **Double-Click Actions:** Double-click node to reset to 0.0 dB; double-click empty space to create a new band at that location.
* **Dynamic Band Count Controls:**
  - Add or remove bands dynamically from 1 up to 10 bands.
  - Preset shortcuts for 3-Band (Tone), 5-Band (Standard), 7-Band (Extended), and 10-Band (Mastering).
* **Two-Way Data Binding with Custom Numeric Inputs:**
  - Direct numeric typing alongside range sliders for Freq, Gain, Q, and Preamp.
  - Input validation and bound clamping.
* **Volatile Auditioning:** Streams 60 FPS slider updates directly into volatile SRAM, guaranteeing **0 flash wear**. Committing to NVS requires an explicit "Save to Hardware NVS" button press.

---

## 9. Firmware Build & Verification Specifications

* **Build Tool:** ESP-IDF v6.1 (`idf.py build`).
* **Compiler Flags (`CMakeLists.txt`):**
  ```cmake
  CFG_TUD_AUDIO=1
  CFG_TUD_AUDIO_FUNC_1_SAMPLE_RATE=48000
  CFG_TUD_AUDIO_ENABLE_EP_OUT=1
  CFG_TUD_AUDIO_ENABLE_EP_IN=0
  CFG_TUD_AUDIO_FUNC_1_EP_OUT_SZ_MAX=196
  CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ=2048
  CFG_TUD_AUDIO_FUNC_1_N_BYTES_PER_SAMPLE_RX=2
  CFG_TUD_AUDIO_FUNC_1_RESOLUTION_RX=16
  CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX=2
  ```
* **Memory Partitioning:**
  - App partition size: `0x177000` bytes (1,500 KB).
  - App binary size: `0x4C070` bytes (304 KB) — **80% free partition headroom remaining**.
* **Flash Command (over COM UART port `/dev/ttyACM0`):**
  ```bash
  idf.py -p /dev/ttyACM0 flash monitor
  ```
* **Operational Verification:**
  - Verified clean boot, I2S 48 kHz PLL clock generation, startup verification chime playback, SSD1306 rendering, and USB enumeration as composite `303A:4002` sound card and WebHID vendor interface.
