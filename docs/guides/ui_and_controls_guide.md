# User Interface, Display & Hardware Controls Guide

This technical guide defines the hardware integration, peripheral drivers, and software architecture for adding an **OLED/TFT Display**, a **Rotary Encoder**, and **Tactile Buttons** to the ESP32-S3 high-fidelity audio system.

> **Note for Agentic AI / AI Assistants:**  
> This specification details GPIO mappings, I2C/SPI display driver configurations, PCNT quadrature rotary decoding, debouncing routines, and a modular UI state machine.

---

## 1. Hardware Selection & Comparison

### 1.1 Display Options

| Display Module | Interface | Resolution | Color | Best Suited For | Pins Required |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **SSD1306 OLED (0.96")** | I2C | 128 $\times$ 64 | Monochrome (White/Blue/Yellow) | Low pin count, compact builds, minimal CPU/RAM | 2 (SDA, SCL) |
| **SH1106 OLED (1.3")** | I2C | 128 $\times$ 64 | Monochrome | Slightly larger viewing area than SSD1306 | 2 (SDA, SCL) |
| **ST7789 IPS TFT (1.3" / 1.54")** | SPI | 240 $\times$ 240 | 16-bit RGB565 | Full-color album art, smooth spectrum bars, graphical EQ curves | 4–6 (MOSI, SCLK, CS, DC, [RST, BL]) |

* **Verified Schematic References:** See [`docs/DSP+DAC+SCREEN.svg`](../schematics/DSP+DAC+SCREEN.svg) (Rev 1.1, Desktop Streamer) and [`docs/DSP+DAC+LOCAL.svg`](../schematics/DSP+DAC+LOCAL.svg) (Rev 1.2, Full DAP), and [Hardware Schematics Guide Sections 3 & 4](../hardware/schematics.md#3-schematic-2-verification-dedicated-esp32-s3-dspdac-with-screen--rotary-encoder-no-local-player-dspdacscreensvg). Standard 4-pin I2C OLED modules (`OLED1` / `OLED2`) default to slave address **`0x3C`**.

---

## 2. Pinout & Interconnection Table

### 2.1 Complete Peripheral Interconnection

```
                       YD-ESP32-S3 Microcontroller
          ┌────────────────────────────────────────────────────┐
          │                                                    │
          │  [I2C Display: SSD1306 / SH1106]                   │
          │  GPIO 8  ──────────────────────────────────────────┼──► SDA
          │  GPIO 9  ──────────────────────────────────────────┼──► SCL
          │  3.3V    ──────────────────────────────────────────┼──► VCC
          │  GND     ──────────────────────────────────────────┼──► GND
          │                                                    │
          │  [Rotary Encoder: EC11 with Push Switch]           │
          │  GPIO 15 ──────────────────────────────────────────┼──► Channel A (CLK)
          │  GPIO 16 ──────────────────────────────────────────┼──► Channel B (DT)
          │  GPIO 17 ──────────────────────────────────────────┼──► Push Switch (SW)
          │  GND     ──────────────────────────────────────────┼──► Common Ground
          │                                                    │
          │  [Tactile Navigation Buttons: SW6 & SW8]           │
          │  GPIO 18 ──────────────────────────────────────────┼──► Button 1 / Left (10k pull-up R8)
          │  GPIO 10 ──────────────────────────────────────────┼──► Button 2 / Right (10k pull-up R12)
          │  GPIO 0  ──────────────────────────────────────────┼──► Onboard Boot / Mute Button
          │                                                    │
          │  [Audio DAC: GY-PCM5102A]                          │
          │  GPIO 4  ──────────────────────────────────────────┼──► LCK (WS)
          │  GPIO 5  ──────────────────────────────────────────┼──► DIN (Data)
          │  GPIO 6  ──────────────────────────────────────────┼──► BCK (Bit Clock)
          │  GPIO 7  ──────────────────────────────────────────┼──► XMT (Soft Mute Line)
          │                                                    │
          │  [SDMMC MicroSD Card]                              │
          │  GPIO 14 ──────────────────────────────────────────┼──► CLK
          │  GPIO 13 ──────────────────────────────────────────┼──► CMD
          │  GPIO 21 ──────────────────────────────────────────┼──► D0
          │                                                    │
          │  [USB OTG Port]                                    │
          │  GPIO 19 ──────────────────────────────────────────┼──► USB D-
          │  GPIO 20 ──────────────────────────────────────────┼──► USB D+
          └────────────────────────────────────────────────────┘
```

| Component | Component Pin | ESP32-S3 Pin | Function / Logic Level | Circuit Protection / Wiring Details |
| :--- | :--- | :--- | :--- | :--- |
| **OLED (`OLED1` 4-pin I2C)** | **SDA (Pin 4)** | **GPIO 8** | I2C Data Line | 4.7 kΩ pull-up to 3.3V (built into module), Address **`0x3C`** |
| **OLED (`OLED1` 4-pin I2C)** | **SCL (Pin 3)** | **GPIO 9** | I2C Clock Line | 4.7 kΩ pull-up to 3.3V (built into module), Address **`0x3C`** |
| **OLED (Legacy 7-pin SPI)**| **CS $\rightarrow$ GND, RES $\rightarrow$ 3V3, DC $\rightarrow$ 3V3** | **GPIO 8/9** | 2-Wire I2C Conversion | **Tying DC to 3.3V sets I2C address to `0x3D` (Alternative fallback)** |
| **Encoder (`SW2` EC11)** | **Channel A (CLK, Pin 3)** | **GPIO 15** | Quadrature Phase A | **Hardware 10 kΩ pull-up `R5` to 3.3V** + optional 100 nF debounce cap |
| **Encoder (`SW2` EC11)** | **Channel B (DT, Pin 4)** | **GPIO 16** | Quadrature Phase B | **Hardware 10 kΩ pull-up `R7` to 3.3V** + optional 100 nF debounce cap |
| **Encoder (`SW2` EC11)** | **Switch (SW, Pin 5)** | **GPIO 17** | Short/Long Press Action | **Hardware 10 kΩ pull-up `R6` to 3.3V** (Active-Low) |
| **Preset 1 Button** | **SW3** | **GPIO 15** | Preset 1 (e.g., Flat PEQ) | **Hardware 10 kΩ pull-up `R2` to 3.3V** (Standalone mode without encoder) |
| **Preset 2 Button** | **SW5** | **GPIO 16** | Preset 2 (e.g., Bass Boost)| **Hardware 10 kΩ pull-up `R3` to 3.3V** (Standalone mode without encoder) |
| **Preset 3 Button** | **SW4** | **GPIO 17** | Preset 3 (e.g., AutoEQ)    | **Hardware 10 kΩ pull-up `R4` to 3.3V** (Standalone mode without encoder) |
| **Navigation Button 1** | **SW6 / SW4 / SW1** | **GPIO 18** | Nav 1 (◄ / Menu / Back / Mode) | **Hardware 10 kΩ pull-up `R8` / `R4` / `R1` to 3.3V** (Active-Low) |
| **Navigation Button 2** | **SW8 (Tactile)** | **GPIO 10** | Nav 2 (► / Next / Preset / Band) | **Hardware 10 kΩ pull-up `R12` to 3.3V** (Active-Low, Rev 1.1) |
| **DAC Mute** | **XMT** | **GPIO 7** | Hardware Pop Prevention | Active-HIGH = Play, LOW = Mute |
| **Headphone Jack** | **J1 / J2 (`DC_TRS_PADS`)** | **U1 / U4 Amp Pins 4, 5, 6** | 3.5mm Stereo TRS Out | Tip = L_OUT, Ring = R_OUT, Sleeve = G_OUT (AGND) |

---

## 3. Rotary Encoder Input Engine

### 3.1 Hardware Debouncing Circuit (Recommended)
Mechanical contacts produce electrical bounce for 2–5 ms. Adding an RC filter to Channel A and B eliminates jitter before it reaches the MCU:

```
  3.3V ──[ 10 kΩ ]──┬──────────► ESP32 GPIO 15 (CLK)
                    │
  Encoder Pin A ────┤
                    │
                   === 100 nF Ceramic Capacitor
                    │
  GND ──────────────┴──────────► GND
```

### 3.2 ESP32-S3 Hardware Pulse Counter (PCNT) Decoding
The ESP32-S3 includes a dedicated hardware **Pulse Counter (PCNT)** peripheral. It decodes quadrature Gray code pulses entirely in silicon without waking or interrupting the CPU:

```c
#include "driver/pulse_cnt.h"

pcnt_unit_handle_t pcnt_unit = NULL;

void init_rotary_encoder(int pin_a, int pin_b)
{
    pcnt_unit_config_t unit_config = {
        .high_limit = 100,
        .low_limit = -100,
        .flags.accum_count = true,
    };
    pcnt_new_unit(&unit_config, &pcnt_unit);

    // Glitch filter: rejects pulses narrower than 1000 ns (1 µs)
    pcnt_glitch_filter_config_t filter_config = {
        .max_glitch_ns = 1000,
    };
    pcnt_unit_set_glitch_filter(pcnt_unit, &filter_config);

    pcnt_chan_config_t chan_a_config = {
        .edge_gpio_num = pin_a,
        .level_gpio_num = pin_b,
    };
    pcnt_channel_handle_t pcnt_chan_a = NULL;
    pcnt_new_channel(pcnt_unit, &chan_a_config, &pcnt_chan_a);

    // Configure quadrature decoding state transitions
    pcnt_channel_set_edge_action(pcnt_chan_a, PCNT_CHANNEL_EDGE_ACTION_DECREASE, PCNT_CHANNEL_EDGE_ACTION_INCREASE);
    pcnt_channel_set_level_action(pcnt_chan_a, PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_INVERSE);

    pcnt_unit_enable(pcnt_unit);
    pcnt_unit_clear_count(pcnt_unit);
    pcnt_unit_start(pcnt_unit);
}
```

### 3.3 Button Event Engine (Short Press, Double Click, Long Press)
The push-button on `GPIO 17` uses an interrupt-driven timer state machine:
* **Short Press (<400 ms):** Play / Pause toggling (or "Enter / Select" in menus).
* **Double Click (<300 ms between presses):** Skip to Next Track (or open SD Browser in SD mode).
* **Long Press (>800 ms):** Open the Audio Mode Selection Menu (Wi-Fi ↔ USB DAC ↔ SD Card).
* **Triple Click (<600 ms total) or Dedicated Mode Button Hold:** Open the **EQ Profile Selection Menu** (Preset 1 Flat ↔ Preset 2 Bass Boost ↔ Preset 3 Harman ↔ **User Custom NVS**).

---

## 4. UI Architecture & Screen Layouts

The user interface is powered by **LVGL v9** or **U8g2** running on **Core 0** at 30 FPS.

```
┌────────────────────────────────────────────────────────┐
│ [Wi-Fi] 44.1kHz 16b          VOL: [████████░░] 80%    │ <- Status Header
│                                                        │
│ Radio Paradise - Mellow Mix                            │ <- Scrolling Title (Marquee)
│ Track: Norah Jones - Come Away With Me                 │ <- Artist / Track info
│                                                        │
│ [PEQ: Custom (NVS)]           || Playback: PLAYING    │ <- Footer / DSP Profile
└────────────────────────────────────────────────────────┘
```

### 4.1 UI State Machine States
```
                      ┌────────────────────────┐
                      │    BOOT / SPLASH       │
                      │  "ESP32-S3 Audio DAC"  │
                      └───────────┬────────────┘
                                  │ (Init Complete)
                                  ▼
                      ┌────────────────────────┐
          ┌──────────►│    NOW PLAYING SCREEN  │◄─────────┐
          │           │  (Status, Vol, Track)  │          │
          │           └───────┬────────────┬───┘          │
(Timeout) │                   │            │              │ (Selection Made)
          │   (Long Press SW) │            │ (Triple Click)
          │                   ▼            ▼              │
          │      ┌─────────────────┐  ┌─────────────────┐ │
          └──────┤  MODE SELECTOR  │  │   EQ SELECTOR   ├─┘
                 │  [Wi-Fi/USB/SD] │  │ [P1/P2/P3/Custom│
                 └────────┬────────┘  │      (NVS)]     │
                          │           └─────────────────┘
                          │ (Double Click in SD Mode)
                          ▼
                 ┌─────────────────┐
                 │ SD CARD BROWSER │
                 │ 📁 /music/rock/ │
                 │ 🎵 01-track.flac│
                 └─────────────────┘
```

#### 4.2 EQ Profile Selector Screen Layout
When the EQ Selector is engaged:
```
┌────────────────────────────────────────────────────────┐
│ << EQUALIZER PROFILE >>      [Slot: USER (NVS)]*      │ <- Asterisk indicates unsaved tweaks
│                                                        │
│   ○ 1. Flat / Bypass (0 dB)                            │
│   ○ 2. Warm Bass Boost (+4.5 dB)                       │
│   ○ 3. Harman OE2018 Target                            │
│   ● 4. Custom: Sennheiser HD600 (NVS)                  │
│  ►💾 5. [Save Active to User Preset]                   │ <- Explicit Flash Commit
│                                                        │
│ [Press = Select/Save]         [Web/USB = Live Audition]│
└────────────────────────────────────────────────────────┘
```
* **Persistent Storage & Explicit Save:**
  * Selecting profiles 1–4 immediately loads parameters and triggers a click-free 15 ms crossfade on Core 1 without stopping audio playback.
  * While live tuning via Web/USB WebHID, changes are auditioned in volatile SRAM (`s_active_peq_bank`), completely avoiding SPI flash wear.
  * Selecting **`[Save Active to User Preset]`** explicitly executes `peq_storage_save_active_to_preset(0, "User Custom")`. It performs a hardware Delta comparison (`memcmp`), writes the binary blob with CRC32 to NVS, and briefly displays `[SAVED TO NVS]`.

## 5. Reference for AI Agents: Implementation Checklist & UI Invariants

### 5.1 UI & Controls Invariants
1. **Never Render UI on Core 1:** All LVGL / U8g2 display rendering and I2C/SPI transactions MUST execute on **Core 0**. Core 1 is reserved strictly for real-time audio decoding, PEQ filtering, and I2S DMA transfers. Display rendering on Core 1 will cause buffer underruns and audible clicks.
2. **PCNT Hardware Peripheral Priority:** Always prefer the hardware Pulse Counter (PCNT) peripheral over raw GPIO edge interrupts for rotary encoders. High-speed rotation can generate thousands of mechanical contact bounces per second; PCNT filters and accumulates Gray code pulses directly in silicon with zero CPU overhead.

### 5.2 Implementation Checklist
Autonomous agents adding display and rotary controls should execute this workflow:

- [ ] **Step 1: Add Display Component to `main/idf_component.yml`**
  ```yaml
  dependencies:
    espressif/esp_lcd_sh1107: "^1.0.0" # or espressif/esp_lcd_ssd1306
    lvgl/lvgl: "^9.1.0"
  ```
- [ ] **Step 2: Initialize I2C Bus on GPIO 8 (SDA) and GPIO 9 (SCL)**
  * Set clock speed to 400 kHz (`I2C_MASTER_FREQ_HZ = 400000`).
  * Attach SSD1306 panel via `esp_lcd_new_panel_ssd1306()` (default standard I2C address `0x3C`, or `0x3D` if using legacy 7-pin SPI module).
- [ ] **Step 3: Setup PCNT Hardware Rotary Driver on GPIO 15/16**
  * Implement delta read helper: `int delta = read_encoder_delta();`.
  * Adjust master volume logarithmic curve or menu selection index based on `delta`.
- [ ] **Step 4: Create UI Dispatch Task (`ui_task`)**
  * Pin to **Core 0**, priority **3**, stack size **4096 bytes**.
  * Use FreeRTOS queues (`ui_event_queue`) to receive events from Audio, Wi-Fi, and Rotary tasks.
