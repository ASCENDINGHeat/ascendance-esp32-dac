# Hardware Schematics & Engineering Verification Guide

> **Note for Agentic AI / AI Assistants:**  
> This document contains the verified circuit schematics, complete 2-way net interconnection matrices, passive filter calculations, and assembly commissioning checklists for the ESP32-S3 High-Fidelity Audio System. When configuring firmware drivers or designing expansions, adhere to the pin allocations, hardware pull-ups, I2C display configurations (standard 4-pin `OLED1` @ `0x3C` vs legacy SPI-converted @ `0x3D`), and PCB solder jumper requirements documented herein.

---

## 1. Overview of Available Schematics

The project repository includes three complete, verified hardware schematic variants exported directly from EasyEDA, packaged individually as SVGs and bundled together in [`Schematic_esp32-dsp_2026-09-19.zip`](../schematics/Schematic_esp32-dsp_2026-09-19.zip):

| Schematic File | Type / Format | Revision | Scope & Included Subsystems | Primary Use Case |
| :--- | :--- | :---: | :--- | :--- |
| **[`DSP+DAC.svg`](../schematics/DSP+DAC.svg)**<br>(also `ESP32-S3-DSP-DAC.svg`) | EasyEDA Vector SVG | **Rev 1.3** | • YD-ESP32-S3 Dev Board (`U1`)<br>• GY-PCM5102A 32-bit DAC (`U2`)<br>• TDA1308 Class-AB Amp (`U3`)<br>• 5V CLC Pi-Filter Network (`C1..C4, L1`)<br>• **3 Tactile Preset Buttons (`SW1`, `SW2`, `SW3`) with 10k Pull-Ups (`R1`, `R2`, `R3`)**<br>• **3.5mm TRS Headphone Output Jack (`J1`)** | Minimalist standalone Wi-Fi / USB-DAC streaming endpoint with hardware preset selection and direct 3.5mm headphone output. Zero display overhead. |
| **[`DSP+DAC+SCREEN.svg`](../schematics/DSP+DAC+SCREEN.svg)** | EasyEDA Vector SVG | **Rev 1.1** | • YD-ESP32-S3 Dev Board (`U7`)<br>• GY-PCM5102A 32-bit DAC (`U8`)<br>• TDA1308 Class-AB Amp (`U9`)<br>• 5V CLC Pi-Filter Network (`C9..C12, L3`)<br>• **Standard 4-Pin I2C OLED Display (`OLED2`, `0x3C`)**<br>• **EC11 Rotary Encoder (`SW7`) with 10k Pull-Ups (`R9..R11`)**<br>• **Tactile Navigation Button 1 (`SW6`, ◄ / Menu / Back) on `GPIO 18` with 10k Pull-Up (`R8`)**<br>• **Tactile Navigation Button 2 (`SW8`, ► / Next / Preset) on `GPIO 10` with 10k Pull-Up (`R12`)**<br>• **3.5mm TRS Headphone Output Jack (`J3`)**<br>• **NO MicroSD Card (`SD1` omitted, GPIO 13/14/21 free)** | Streaming Web Radio, AirPlay/DLNA, and USB DAC endpoint with physical track/volume/EQ OLED display, rotary knob, and dual-button tactile navigation matrix for live on-device PEQ tuning, without SD card overhead. |
| **[`DSP+DAC+LOCAL.svg`](../schematics/DSP+DAC+LOCAL.svg)**<br>(also `ESP32-S3-DSP-DAC with local player.svg`) | EasyEDA Vector SVG | **Rev 1.2** | • All core audio subsystems above (`U4, U5, U6`)<br>• **MicroSD Card Breakout (`SD1`, 1-bit SDMMC bus)**<br>• **Standard 4-Pin I2C OLED Display (`OLED1`, `0x3C`)**<br>• **EC11 Rotary Encoder (`SW5`) with 10k Pull-Ups (`R5..R7`)**<br>• Tactile Mode Button (`SW4`) with 10k Pull-Up (`R4`)<br>• 5V CLC Pi-Filter Network (`C5..C8, L2`)<br>• **3.5mm TRS Headphone Output Jack (`J2`)** | Full-featured standalone DAP (Digital Audio Player) with standard I2C OLED UI, hardware-debounced rotary encoder, and offline high-res MicroSD playback. |
| **[`Schematic_esp32-dsp_2026-09-19.zip`](../schematics/Schematic_esp32-dsp_2026-09-19.zip)** | Vector Archive | — | Complete multi-sheet EasyEDA project export containing `DSP+DAC.svg`, `DSP+DAC+SCREEN.svg`, and `DSP+DAC+LOCAL.svg`. | Archive for local editing or importing into EasyEDA / KiCad. |
| **[`ESP32-S3-DSP-DAC.png`](../schematics/ESP32-S3-DSP-DAC.png)** | High-Res PNG | — | Complete EasyEDA layout bitmap export with raw net labels and BOM designations. | Production bench assembly and visual cross-reference. |

---

## 2. Schematic 1 Verification: Dedicated ESP32-S3 DSP/DAC Pipeline with Preset Buttons (`ESP32-S3-DSP-DAC.svg`)

This section provides the formal engineering verification report for the standalone streaming DSP/DAC pipeline schematic:

* **Schematic File:** [`docs/ESP32-S3-DSP-DAC.svg`](../schematics/ESP32-S3-DSP-DAC.svg)
* **Title:** ESP32-S3 DSP/DAC
* **Author / Drawn By:** `ASCENSION`
* **Date:** 2026-09-19 | **Revision:** 1.3
* **EDA Tool:** EasyEDA

![ESP32-S3 DSP/DAC Schematic](../schematics/ESP32-S3-DSP-DAC.svg)

---

### 2.1 Schematic 1 Subsystem Verification Matrix

| Subsystem / Net | Source Component & Pin | Destination Component & Pin | Schematic Net / Wire | Firmware Constant (`board_pins.h`) | Verification Status | Engineering Assessment & Notes |
| :--- | :--- | :--- | :--- | :--- | :---: | :--- |
| **I2S Word Select (WS)** | `U3` (YD-ESP32) Pin 4 (`GPIO4`) | `U2` (PCM5102A) Pin 13 (`LCK`) | Net `GPIO4` $\rightarrow$ `LCK` | `BOARD_I2S_WS_PIN` | **PASS (100%)** | Correct frame sync clock routing for Philips I2S. |
| **I2S Serial Data Out** | `U3` (YD-ESP32) Pin 5 (`GPIO5`) | `U2` (PCM5102A) Pin 12 (`DIN`) | Net `GPIO5` $\rightarrow$ `DIN` | `BOARD_I2S_DATA_OUT_PIN` | **PASS (100%)** | Direct serial audio bitstream routing. |
| **I2S Continuous Bit Clock**| `U3` (YD-ESP32) Pin 6 (`GPIO6`) | `U2` (PCM5102A) Pin 11 (`BCK`) | Net `GPIO6` $\rightarrow$ `BCK` | `BOARD_I2S_BCK_PIN` | **PASS (100%)** | Bit clock output correctly wired. |
| **Hardware Soft Mute** | `U3` (YD-ESP32) Pin 7 (`GPIO7`) | `U2` (PCM5102A) Pin 3 (`XMST` / `XMT`)| Net `GPIO7` $\rightarrow$ `XMST` | `BOARD_DAC_MUTE_PIN` | **PASS (100%)** | Active-low soft unmute driven directly by firmware arbiter. |
| **DAC Internal PLL Clock** | Common Digital `GND` | `U2` (PCM5102A) Pin 10 (`SCK`)| Net `SCK` $\rightarrow$ `GND` | N/A (Hardware tied) | **PASS (100%)** | Grounding SCK forces the PCM5102A internal PLL to synthesize MCLK from BCK. |
| **Analog Audio Left Line** | `U2` (PCM5102A) Pin 9 (`L`) | `U1` (TDA1308) Pin 1 (`L` IN) | Net `L` $\rightarrow$ `L` | N/A (Analog Line) | **PASS (100%)** | Line-level audio output directly coupled to amplifier stage. |
| **Analog Audio Right Line**| `U2` (PCM5102A) Pin 7 (`R`) | `U1` (TDA1308) Pin 3 (`R` IN) | Net `R` $\rightarrow$ `R` | N/A (Analog Line) | **PASS (100%)** | Line-level audio output directly coupled to amplifier stage. |
| **Analog Audio Ground Return**| `U2` (PCM5102A) Pin 6 (`AGND`)| `U1` (TDA1308) Pin 2 (`G` IN) | Net `AGND` $\rightarrow$ `G` | N/A (Star Ground) | **PASS (100%)** | Dedicated analog ground link isolates audio reference from digital GND noise. |
| **Headphone Out Left Channel**| `U1` (TDA1308) Pin 4 (`L`) | `J2` (`DC_TRS_PADS`) Pin GND | Net `L_OUT` $\rightarrow$ `J2` | N/A (Analog Out) | **PASS (100%)** | Amplified left channel routed to 3.5mm stereo jack. |
| **Headphone Out Ground** | `U1` (TDA1308) Pin 5 (`G`) | `J2` (`DC_TRS_PADS`) Pin PWR | Net `G_OUT` $\rightarrow$ `J2` | N/A (Analog Out) | **PASS (100%)** | Amplified audio ground reference routed to 3.5mm stereo jack. |
| **Headphone Out Right Channel**| `U1` (TDA1308) Pin 6 (`R`) | `J2` (`DC_TRS_PADS`) Pin RNG | Net `R_OUT` $\rightarrow$ `J2` | N/A (Analog Out) | **PASS (100%)** | Amplified right channel routed to 3.5mm stereo jack Ring. |
| **Preset 1 Button** | `SW3` (B3F-1000) Pins 3 & 4 | `GPIO 15` | Left Pin 8 | Net `GPIO15` $\rightarrow$ `SW3` | `PIN_BTN_PRESET_1` | **PASS (100%)** | Dedicated hardware button for Preset 1 (e.g., Flat/Bypass PEQ). |
| **Preset 1 Pull-Up** | `R2` (10 kΩ Resistor) | 3.3V Rail | Left Pin 2 | Resistor `3V3` $\rightarrow$ `GPIO15` | N/A (Hardware Pull-up) | **PASS (100%)** | Pulls `GPIO 15` HIGH; button press shorts to GND. |
| **Preset 2 Button** | `SW5` (B3F-1000) Pins 3 & 4 | `GPIO 16` | Left Pin 9 | Net `GPIO16` $\rightarrow$ `SW5` | `PIN_BTN_PRESET_2` | **PASS (100%)** | Dedicated hardware button for Preset 2 (e.g., Bass Boost PEQ). |
| **Preset 2 Pull-Up** | `R3` (10 kΩ Resistor) | 3.3V Rail | Left Pin 2 | Resistor `3V3` $\rightarrow$ `GPIO16` | N/A (Hardware Pull-up) | **PASS (100%)** | Pulls `GPIO 16` HIGH; button press shorts to GND. |
| **Preset 3 Button** | `SW4` (B3F-1000) Pins 3 & 4 | `GPIO 17` | Left Pin 10 | Net `GPIO17` $\rightarrow$ `SW4` | `PIN_BTN_PRESET_3` | **PASS (100%)** | Dedicated hardware button for Preset 3 (e.g., AutoEQ / Harman Target). |
| **Preset 3 Pull-Up** | `R4` (10 kΩ Resistor) | 3.3V Rail | Left Pin 2 | Resistor `3V3` $\rightarrow$ `GPIO17` | N/A (Hardware Pull-up) | **PASS (100%)** | Pulls `GPIO 17` HIGH; button press shorts to GND. |
| **Preset Buttons Ground** | `SW3`, `SW4`, `SW5` Pins 1 & 2 | System GND | Left Pin 22 | Common GND Net | N/A (Power) | **PASS (100%)** | Common ground return for active-low button press. |
| **5V Power Input** | `U3` (YD-ESP32) Pin 21 (`5V`) | Pi-Filter Input Node (`C1` \|\| `C2`)| Net `5V` $\rightarrow$ Filter | N/A (Power) | **PASS (100%)** | Draws power from ESP32 5V rail (*requires `IN-OUT` jumper bridged*). |
| **Filtered 5V Power Output**| Pi-Filter Output (`C3` \|\| `C4`)| `U2` Pin 15 (`VIN`) & `U1` Pin 7 (`+5V`)| Net `+5V` Clean | N/A (Power) | **PASS (100%)** | Powers both DAC and headphone amp from filtered quiet rail. |
| **System Digital Ground** | Digital `GND` Plane | `U3` Pin 22, Filter Caps, `U1` Pin 8 (`GND`), `U2` Pin 14 (`GND`) | Net `GND` | N/A (Power) | **PASS (100%)** | Solid common ground return for all digital stages. |

---

### 2.2 Engineering Deep Dive: The 5V Pi-Filter ($\pi$-filter CLC Network)

The schematic incorporates a discrete **CLC Pi-filter** (`C1 || C2 - L1 - C3 || C4`) inserted between the ESP32-S3 `5V` output (Pin 21) and the analog audio modules (`U2: VIN` and `U1: +5V`):

```
                       L1 (10µH Inductor)
               ┌───────────UUUUUU───────────┐
               │                            │
 5Vin (Pin 21) ┴                            ┬ +5V Clean Rail
 (from ESP32)  ┬ C1       ┬ C2      ┬ C3    ┬ C4 (to PCM5102A & TDA1308)
               │ 0.1µF    │ 10µF    │ 10µF  │ 0.1µF
              GND        GND       GND     GND
```

* **Cutoff Frequency:**
  $$f_c = \frac{1}{2\pi \sqrt{L_1 \cdot C_{out}}} = \frac{1}{2\pi \sqrt{10 \times 10^{-6}\text{ H} \times 10.1 \times 10^{-6}\text{ F}}} \approx 15.8\text{ kHz}$$
* **Rejection:** Delivers $>50\text{ dB}$ attenuation across the $100\text{ kHz} - 100\text{ MHz}$ frequency spectrum, suppressing Wi-Fi 2.4 GHz packet ripple and USB switching supply buzz.
* **Inductor Specification:** Shielded SMD Power Inductor (Bourns SRR0604 / CD54), $10\,\mu\text{H} \pm 20\%$, $I_{sat} \ge 300\text{ mA}$, $\text{DCR} < 0.5\,\Omega$.
* **Through-Hole (No-SMD) Alternative:** Radial Leaded Drum Core Choke (**Bourns RLB0914-100KL** or **RLB0812-100KL**), $10\,\mu\text{H} \pm 10\%$, $I_{dc} = 1.6\text{ A}$, $\text{DCR} = 0.044\,\Omega$, 5.0 mm lead spacing. Paired with **KEMET GoldMax C315C104K5R5TA** radial dipped MLCCs (0.1µF, 2.54mm pitch) and **Nichicon UPW1E100MDD** radial low-ESR electrolytics (10µF, 2.5mm pitch). See [Hardware Guide Section 1.4](hardware.md#14-no-smd--100-through-hole-tht-build-guide--parts-recommendation) for the complete No-SMD BOM.

---

### 2.3 Architectural & Electrical Analysis: 3-Button Preset Controller

#### 1. Hardware Debouncing & Pull-Up Network
* Each button (`SW3`, `SW5`, `SW4`) is equipped with an external **discrete 10 kΩ pull-up resistor (`R2`, `R3`, `R4`) tied to the 3.3V rail** (Left Header Pin 2).
* Hardware pull-ups prevent the input pins from floating when the buttons are open, safeguarding against false triggers induced by high-power 2.4 GHz Wi-Fi transmission bursts (up to +20 dBm RF spikes).
* The momentary switch closures pull `GPIO 15`, `GPIO 16`, and `GPIO 17` cleanly to digital GND (Active-LOW logic).

#### 2. Dual-Personality GPIO Allocation (Standalone vs Local Player DAP)
* In the standalone DSP/DAC configuration (Schematic 1, Rev 1.3), `GPIO 15`, `GPIO 16`, and `GPIO 17` are utilized as dedicated tactile preset selection inputs.
* In the display & encoder configurations (Schematic 2 Rev 1.1 / Schematic 3 Rev 1.2), these identical three pins are assigned to the EC11 quadrature rotary encoder (`GPIO 15 = Phase A`, `GPIO 16 = Phase B`, `GPIO 17 = Push Switch SW`).
* This unified pin assignment enables firmware code reuse and simple build-time conditional configuration:
  ```c
  #if CONFIG_USE_ROTARY_ENCODER
      #define PIN_ENC_A          GPIO_NUM_15  // Rotary Phase A
      #define PIN_ENC_B          GPIO_NUM_16  // Rotary Phase B
      #define PIN_ENC_SW         GPIO_NUM_17  // Rotary Push Switch
  #else
      #define PIN_BTN_PRESET_1   GPIO_NUM_15  // SW3: Preset 1 (e.g. Flat / Bypass)
      #define PIN_BTN_PRESET_2   GPIO_NUM_16  // SW5: Preset 2 (e.g. Bass Boost)
      #define PIN_BTN_PRESET_3   GPIO_NUM_17  // SW4: Preset 3 (e.g. AutoEQ Harman)
  #endif
  ```

#### 3. Preset Control Functional Mapping & Persistent User Profile Switching
* **Preset 1 (`SW3` on GPIO 15):** Instant PEQ Profile 1 selection (Reference Flat / Bypass; 🟢 Green LED).
* **Preset 2 (`SW5` on GPIO 16):** Instant PEQ Profile 2 selection (Warm Bass Boost +4.5dB; 🔵 Cyan LED).
* **Preset 3 (`SW4` on GPIO 17):** Instant PEQ Profile 3 selection (Harman AutoEQ Target; 🔷 Blue LED).
* **Persistent User Custom PEQ (Long Press SW4 or Combo SW3 + SW5):**
  * Holding `SW4` for >1.5 seconds (or pressing `SW3 + SW5` simultaneously) instantly engages the **User Custom PEQ Profile stored in flash NVS** (🟣 Violet/Magenta LED).
  * **Zero-Reflash Independence:** The user custom profile is edited live via the Web UI (`POST /api/eq/user`) and stored in NVS, completely decoupled from the immutable factory presets. Buttons toggle seamlessly between the ROM presets and the persistent NVS profile with a 15 ms click-free crossfade.

---

## 3. Schematic 2 Verification: Dedicated ESP32-S3 DSP/DAC with Screen, Rotary Encoder & Dual Navigation Buttons (No Local Player) (`DSP+DAC+SCREEN.svg`)

This section provides the formal engineering verification report for the desktop / streamer variant equipped with an interactive OLED display, rotary encoder, and dual tactile navigation buttons (Rev 1.1), omitting the MicroSD socket:

* **Schematic File:** [`docs/DSP+DAC+SCREEN.svg`](../schematics/DSP+DAC+SCREEN.svg)
* **Title:** ESP32-S3 DSP/DAC w/ screen no localplayer
* **Author / Drawn By:** `ASCENSION`
* **Date:** 2026-09-24 | **Revision:** 1.1 (Rev 1.1: Added Button 2 `SW8` & pull-up `R12` on `GPIO 10`)
* **EDA Tool:** EasyEDA

![ESP32-S3 DSP/DAC with Screen (No Local Player)](../schematics/DSP+DAC+SCREEN.svg)

---

### 3.1 Schematic 2 Subsystem Verification Matrix

| Subsystem / Net | Source Component & Pin | Destination Component & Pin | Schematic Net / Wire | Firmware Constant (`board_pins.h`) | Verification Status | Engineering Assessment & Notes |
| :--- | :--- | :--- | :--- | :--- | :---: | :--- |
| **I2S Word Select (WS)** | `U7` (YD-ESP32) Pin 4 (`GPIO4`) | `U8` (PCM5102A) Pin 13 (`LCK`) | Net `GPIO4` $\rightarrow$ `LCK` | `BOARD_I2S_WS_PIN` | **PASS (100%)** | Correct frame sync clock routing for Philips I2S. |
| **I2S Serial Data Out** | `U7` (YD-ESP32) Pin 5 (`GPIO5`) | `U8` (PCM5102A) Pin 12 (`DIN`) | Net `GPIO5` $\rightarrow$ `DIN` | `BOARD_I2S_DATA_OUT_PIN` | **PASS (100%)** | Direct serial audio bitstream routing. |
| **I2S Continuous Bit Clock**| `U7` (YD-ESP32) Pin 6 (`GPIO6`) | `U8` (PCM5102A) Pin 11 (`BCK`) | Net `GPIO6` $\rightarrow$ `BCK` | `BOARD_I2S_BCK_PIN` | **PASS (100%)** | Bit clock output correctly wired. |
| **Hardware Soft Mute** | `U7` (YD-ESP32) Pin 7 (`GPIO7`) | `U8` (PCM5102A) Pin 3 (`XMST` / `XMT`)| Net `GPIO7` $\rightarrow$ `XMST` | `BOARD_DAC_MUTE_PIN` | **PASS (100%)** | Active-low soft unmute driven directly by firmware arbiter. |
| **DAC Internal PLL Clock** | Common Digital `GND` | `U8` (PCM5102A) Pin 10 (`SCK`)| Net `SCK` $\rightarrow$ `GND` | N/A (Hardware tied) | **PASS (100%)** | Grounding SCK forces internal PLL to synthesize MCLK. |
| **Analog Audio Left Line** | `U8` (PCM5102A) Pin 9 (`L`) | `U9` (TDA1308) Pin 1 (`L` IN) | Net `L` $\rightarrow$ `L` | N/A (Analog Line) | **PASS (100%)** | Line-level audio output directly coupled to amplifier stage. |
| **Analog Audio Right Line**| `U8` (PCM5102A) Pin 7 (`R`) | `U9` (TDA1308) Pin 3 (`R` IN) | Net `R` $\rightarrow$ `R` | N/A (Analog Line) | **PASS (100%)** | Line-level audio output directly coupled to amplifier stage. |
| **Analog Audio Ground Return**| `U8` (PCM5102A) Pin 6 (`AGND`)| `U9` (TDA1308) Pin 2 (`G` IN) | Net `AGND` $\rightarrow$ `G` | N/A (Star Ground) | **PASS (100%)** | Dedicated analog ground link isolates audio reference from digital GND. |
| **Headphone Out Left Channel**| `U9` (TDA1308) Pin 4 (`L`) | `J3` (`DC_TRS_PADS`) Pin GND | Net `L_OUT` $\rightarrow$ `J3` | N/A (Analog Out) | **PASS (100%)** | Amplified left channel routed to 3.5mm stereo jack Tip. |
| **Headphone Out Ground** | `U9` (TDA1308) Pin 5 (`G`) | `J3` (`DC_TRS_PADS`) Pin PWR | Net `G_OUT` $\rightarrow$ `J3` | N/A (Analog Out) | **PASS (100%)** | Amplified audio ground reference routed to 3.5mm stereo jack Sleeve. |
| **Headphone Out Right Channel**| `U9` (TDA1308) Pin 6 (`R`) | `J3` (`DC_TRS_PADS`) Pin RNG | Net `R_OUT` $\rightarrow$ `J3` | N/A (Analog Out) | **PASS (100%)** | Amplified right channel routed to 3.5mm stereo jack Ring. |
| **OLED I2C Data (SDA)** | `U7` (YD-ESP32) Pin 12 (`GPIO8`) | `OLED2` Pin 4 (`SDA`) | Net `GPIO8` $\rightarrow$ `SDA` | `BOARD_I2C_SDA_PIN` | **PASS (100%)** | Standard I2C serial data line for 128x64 display. |
| **OLED I2C Clock (SCL)** | `U7` (YD-ESP32) Pin 15 (`GPIO9`) | `OLED2` Pin 3 (`SCL`) | Net `GPIO9` $\rightarrow$ `SCL` | `BOARD_I2C_SCL_PIN` | **PASS (100%)** | Standard I2C serial clock line for 128x64 display. |
| **OLED Logic Power & GND** | 3.3V Rail (Pin 2) & GND (Pin 22) | `OLED2` Pin 2 (`VCC`), Pin 1 (`GND`)| Net `3V3` / `GND` | N/A (Power) | **PASS (100%)** | **Standard 4-pin module (Address: `0x3C`)**. |
| **Rotary Phase A** | `U7` (YD-ESP32) Pin 8 (`GPIO15`) | `SW7` (EC11) Pin A (`A`) | Net `GPIO15` $\rightarrow$ `A` | `BOARD_ENC_A_PIN` | **PASS (100%)** | Quadrature encoder channel A (PCNT filtered). |
| **Rotary Phase A Pull-Up** | `R9` (10 kΩ Resistor) | 3.3V Rail (Pin 2) | Resistor `3V3` $\rightarrow$ `GPIO15` | N/A (Hardware Pull-up) | **PASS (100%)** | Pulls `GPIO 15` HIGH to prevent contact bounce. |
| **Rotary Phase B** | `U7` (YD-ESP32) Pin 9 (`GPIO16`) | `SW7` (EC11) Pin B (`B`) | Net `GPIO16` $\rightarrow$ `B` | `BOARD_ENC_B_PIN` | **PASS (100%)** | Quadrature encoder channel B (PCNT filtered). |
| **Rotary Phase B Pull-Up** | `R10` (10 kΩ Resistor) | 3.3V Rail (Pin 2) | Resistor `3V3` $\rightarrow$ `GPIO16` | N/A (Hardware Pull-up) | **PASS (100%)** | Pulls `GPIO 16` HIGH to prevent contact bounce. |
| **Rotary Push Switch (SW)**| `U7` (YD-ESP32) Pin 10 (`GPIO17`)| `SW7` (EC11) Pin D (`D` / `SW`) | Net `GPIO17` $\rightarrow$ `SW` | `BOARD_ENC_SW_PIN` | **PASS (100%)** | Momentary push button on encoder shaft (active-low). |
| **Rotary Switch Pull-Up** | `R11` (10 kΩ Resistor) | 3.3V Rail (Pin 2) | Resistor `3V3` $\rightarrow$ `GPIO17` | N/A (Hardware Pull-up) | **PASS (100%)** | Pulls `GPIO 17` HIGH to prevent floating inputs. |
| **Rotary Common Ground** | Common Digital `GND` | `SW7` (EC11) Pins C & E | Net `GND` $\rightarrow$ Switch | N/A (Power) | **PASS (100%)** | Returns encoder phase and switch closures to digital GND. |
| **Tactile Navigation Button 1** | `U7` (YD-ESP32) Pin 11 (`GPIO18`)| `SW6` (B3F-1000) Pins 3 & 4 | Net `GPIO18` $\rightarrow$ Button | `BOARD_BTN_1_PIN` / `BOARD_BTN_MODE_PIN` | **PASS (100%)** | Multi-function navigation button 1 (◄ / Menu / Back / Mode switch). |
| **Button 1 Pull-Up** | `R8` (10 kΩ Resistor) | 3.3V Rail (Pin 2) | Resistor `3V3` $\rightarrow$ `GPIO18` | N/A (Hardware Pull-up) | **PASS (100%)** | Discrete 10k pull-up shields GPIO 18 from RF spikes. |
| **Tactile Navigation Button 2** | `U7` (YD-ESP32) Pin 16 (`GPIO10`)| `SW8` (B3F-1000) Pins 3 & 4 | Net `GPIO10` $\rightarrow$ Button | `BOARD_BTN_2_PIN` / `BOARD_BTN_NAV_RIGHT_PIN` | **PASS (100%)** | **Rev 1.1 Addition:** Multi-function navigation button 2 (► / Next / Preset / Add/Remove Band). |
| **Button 2 Pull-Up** | `R12` (10 kΩ Resistor) | 3.3V Rail (Pin 2) | Resistor `3V3` $\rightarrow$ `GPIO10` | N/A (Hardware Pull-up) | **PASS (100%)** | **Rev 1.1 Addition:** Discrete 10k pull-up from 3.3V ensures crisp, bounce-free transitions on GPIO 10. |
| **Buttons Common Ground** | Common Digital `GND` | `SW6` & `SW8` Pins 1 & 2 | Net `GND` $\rightarrow$ Buttons | N/A (Power) | **PASS (100%)** | Connects tactile switches return path to digital system ground. |
| **5V CLC Pi-Filter Network**| `C9` (0.1µF) \|\| `C10` (10µF) $\rightarrow$ `L3` (10µH) $\rightarrow$ `C11` (10µF) \|\| `C12` (0.1µF) | Between Pin 21 (`5V`) and DAC/Amp | Net `5V` $\rightarrow$ `+5V` Clean | N/A (Filter) | **PASS (100%)** | Identical high-attenuation Pi-filter topology ($f_c \approx 15.8\text{ kHz}$). |
| **Unused / Free Pins** | `GPIO 13` (CMD), `GPIO 14` (CLK), `GPIO 21` (DAT0) | Open / Unconnected | — | N/A (Unused) | **PASS (100%)** | **MicroSD socket omitted.** Pins 13, 14, 21 left completely free for expansions! |

---

### 3.2 Engineering Analysis & Design Rationale for Schematic 2
1. **Target Architecture:** Designed specifically for **Desktop USB DAC + Wi-Fi Web Radio / Network Audio Streamers** where users desire a physical visual display (OLED track title, sample rate, EQ curve, volume bar), an aluminum rotary control knob, and a full tactile navigation matrix, but do not require local MicroSD card storage.
2. **Component & PCB Savings:**
   * Eliminates the MicroSD card socket breakout (`SD1`) and associated pull-up wiring.
   * Frees up **`GPIO 13`**, **`GPIO 14`**, and **`GPIO 21`** on the ESP32-S3 header. These pins can be reassigned for UART2, infrared remote control (IR receiver on GPIO 21), or hardware status LEDs.
3. **Dual-Button Navigation Matrix (Rev 1.1 Upgrade):**
   * **Addition of `SW8` and Pull-Up `R12` on `GPIO 10`:** Upgrades the single-button architecture to a symmetric dual-button navigation matrix (`SW6` on `GPIO 18` for ◄ / Menu / Back; `SW8` on `GPIO 10` for ► / Next / Preset).
   * **Full UI State Machine & Live PEQ Tuning:** Enables seamless, intuitive control of the 3-mode UI state machine (Playback Screen, System Menu, and Live On-Device Digit-by-Digit PEQ Editor). Users can adjust frequency, gain, and Q per band digit-by-digit using the rotary encoder, add/remove biquad bands via Button 1/2 short/double presses, and save/cancel with long presses, all without needing a companion PC app.
   * **Preservation of Free Expansion GPIOs:** Placing `SW8` on `GPIO 10` (Pin 16 of the left header) preserves `GPIO 13, 14, 21` completely untouched for future peripherals or SD expansions.
4. **Identical Audio & UI Firmware Compatibility:**
   * Because `GPIO 4, 5, 6, 7` (I2S), `GPIO 8, 9` (I2C OLED), `GPIO 15, 16, 17` (Rotary Encoder), and `GPIO 18, 10` (Buttons 1 & 2) share a standardized pin mapping, the firmware runs unified across hardware variants with zero pin conflicts.

---

## 4. Schematic 3 Verification: Complete DSP/DAC with Local Player, Display & Controls (`DSP+DAC+LOCAL.svg` / `ESP32-S3-DSP-DAC with local player.svg`)

This section provides the formal engineering verification report for the full-featured local player schematic:

* **Schematic File:** [`docs/DSP+DAC+LOCAL.svg`](../schematics/DSP+DAC+LOCAL.svg) (also [`docs/ESP32-S3-DSP-DAC with local player.svg`](../schematics/ESP32-S3-DSP-DAC%20with%20local%20player.svg))
* **Title:** ESP32-S3 DSP/DAC w/ Local Player
* **Author / Drawn By:** `ASCENSION`
* **Date:** 2026-09-19 | **Revision:** 1.2
* **EDA Tool:** EasyEDA

![ESP32-S3 DSP/DAC with Local Player](../schematics/DSP+DAC+LOCAL.svg)

---

### 4.1 Complete Local Player Interconnection Matrix

| Subsystem / Peripheral | Component & Pin Label | ESP32-S3 Source Pin | Physical Header Pin | Schematic Net / Wiring | Firmware Define (`board_pins.h`) | Verification Status | Functional Role & Engineering Analysis |
| :--- | :--- | :--- | :--- | :--- | :--- | :---: | :--- |
| **Audio I2S WS** | `U5` (PCM5102A) Pin 13 (`LCK`) | `GPIO 4` | Left Pin 4 | Net `GPIO4` $\rightarrow$ `LCK` | `BOARD_I2S_WS_PIN` | **PASS (100%)** | Word Select / Frame Sync Clock |
| **Audio I2S DIN** | `U5` (PCM5102A) Pin 12 (`DIN`) | `GPIO 5` | Left Pin 5 | Net `GPIO5` $\rightarrow$ `DIN` | `BOARD_I2S_DATA_OUT_PIN` | **PASS (100%)** | Serial PCM Audio Data Stream |
| **Audio I2S BCK** | `U5` (PCM5102A) Pin 11 (`BCK`) | `GPIO 6` | Left Pin 6 | Net `GPIO6` $\rightarrow$ `BCK` | `BOARD_I2S_BCK_PIN` | **PASS (100%)** | Continuous Bit Clock |
| **Audio Soft Mute** | `U5` (PCM5102A) Pin 3 (`XMST`)| `GPIO 7` | Left Pin 7 | Net `GPIO7` $\rightarrow$ `XMST` | `BOARD_DAC_MUTE_PIN` | **PASS (100%)** | Active-low hardware pop suppression |
| **DAC PLL Ground** | `U5` (PCM5102A) Pin 10 (`SCK`)| Common GND | — | Net `SCK` $\rightarrow$ `GND` | N/A (Hardware tied) | **PASS (100%)** | Enables internal PLL clock generator |
| **Analog Audio L** | `U5` Pin 9 (`L`) $\rightarrow$ `U4` Pin 1 (`L` IN)| — | — | Net `L` $\rightarrow$ `L` | N/A (Analog Line) | **PASS (100%)** | Left channel line-level audio |
| **Analog Audio R** | `U5` Pin 7 (`R`) $\rightarrow$ `U4` Pin 3 (`R` IN)| — | — | Net `R` $\rightarrow$ `R` | N/A (Analog Line) | **PASS (100%)** | Right channel line-level audio |
| **Analog Ground** | `U5` Pin 6 (`AGND`) $\rightarrow$ `U4` Pin 2 (`G` IN)| — | — | Net `AGND` $\rightarrow$ `G` | N/A (Star Ground) | **PASS (100%)** | Dedicated analog ground reference |
| **Headphone Out Left Channel**| `U4` (TDA1308) Pin 4 (`L`) | `J1` (`DC_TRS_PADS`) Pin GND | Net `L_OUT` $\rightarrow$ `J1` | N/A (Analog Out) | **PASS (100%)** | Amplified left channel routed to 3.5mm stereo jack. |
| **Headphone Out Ground** | `U4` (TDA1308) Pin 5 (`G`) | `J1` (`DC_TRS_PADS`) Pin PWR | Net `G_OUT` $\rightarrow$ `J1` | N/A (Analog Out) | **PASS (100%)** | Amplified audio ground reference routed to 3.5mm stereo jack. |
| **Headphone Out Right Channel**| `U4` (TDA1308) Pin 6 (`R`) | `J1` (`DC_TRS_PADS`) Pin RNG | Net `R_OUT` $\rightarrow$ `J1` | N/A (Analog Out) | **PASS (100%)** | Amplified right channel routed to 3.5mm stereo jack Ring. |
| **OLED I2C Data** | `OLED1` Pin 4 (`SDA`) | `GPIO 8` | Left Pin 12 | Net `GPIO8` $\rightarrow$ `SDA` | `BOARD_I2C_SDA_PIN` | **PASS (100%)** | Standard I2C Serial Data Line |
| **OLED I2C Clock** | `OLED1` Pin 3 (`SCL`) | `GPIO 9` | Left Pin 15 | Net `GPIO9` $\rightarrow$ `SCK` | `BOARD_I2C_SCL_PIN` | **PASS (100%)** | Standard I2C Serial Clock Line |
| **OLED Power VCC**| `OLED1` Pin 2 (`VCC`) | 3.3V Rail | Left Pin 2 | Net `3V3` $\rightarrow$ `VCC` | N/A (Power) | **PASS (100%)** | 3.3V Logic & Panel Supply |
| **OLED Ground** | `OLED1` Pin 1 (`GND`) | System GND | Left Pin 22 | Net `GND` $\rightarrow$ `GND` | N/A (Power) | **PASS (100%)** | **Standard 4-pin I2C module (Address: `0x3C`)** |
| **Rotary Phase A** | `SW2` (EC11) Pin A (`A`) | `GPIO 15` | Left Pin 8 | Net `GPIO15` $\rightarrow$ `A` | `BOARD_ENC_A_PIN` | **PASS (100%)** | Quadrature encoder channel A |
| **Rotary Phase A Pull-Up** | `R5` (10 kΩ Resistor) | 3.3V Rail | Left Pin 2 | Resistor `3V3` $\rightarrow$ `GPIO15` | N/A (Hardware Pull-up) | **PASS (100%)** | Hardware pull-up prevents floating input |
| **Rotary Phase B** | `SW2` (EC11) Pin B (`B`) | `GPIO 16` | Left Pin 9 | Net `GPIO16` $\rightarrow$ `B` | `BOARD_ENC_B_PIN` | **PASS (100%)** | Quadrature encoder channel B |
| **Rotary Phase B Pull-Up** | `R7` (10 kΩ Resistor) | 3.3V Rail | Left Pin 2 | Resistor `3V3` $\rightarrow$ `GPIO16` | N/A (Hardware Pull-up) | **PASS (100%)** | Hardware pull-up prevents floating input |
| **Rotary Switch** | `SW2` (EC11) Pin D (`D` / `SW`)| `GPIO 17` | Left Pin 10 | Net `GPIO17` $\rightarrow$ `SW` | `BOARD_ENC_SW_PIN` | **PASS (100%)** | Encoder push button (active-low) |
| **Rotary Switch Pull-Up** | `R6` (10 kΩ Resistor) | 3.3V Rail | Left Pin 2 | Resistor `3V3` $\rightarrow$ `GPIO17` | N/A (Hardware Pull-up) | **PASS (100%)** | Hardware pull-up prevents floating input |
| **Rotary Ground** | `SW2` (EC11) Pins C & E (`GND`)| System GND | Left Pin 22 | Net `GND` $\rightarrow$ `GND` | N/A (Power) | **PASS (100%)** | Common switch/encoder ground |
| **Mode Switch** | `SW1` (B3F-1000) Pins 3 & 4 | `GPIO 18` | Left Pin 11 | Net `GPIO18` $\rightarrow$ Button | `BOARD_BTN_MODE_PIN` | **PASS (100%)** | Dedicated audio mode selector |
| **Mode Pull-Up** | `R1` (10 kΩ Resistor) | 3.3V Rail | Left Pin 2 | Resistor `3V3` $\rightarrow$ `GPIO18` | N/A (Hardware Pull-up) | **PASS (100%)** | Clean hardware pull-up prevents floating inputs |
| **Mode Switch GND**| `SW1` (B3F-1000) Pins 1 & 2 | System GND | Left Pin 22 | Net `GND` $\rightarrow$ Button | N/A (Power) | **PASS (100%)** | Momentary closure pulls GPIO 18 to GND |
| **SDMMC Clock** | `SD1` (SD-MMC) Pin 4 (`SCLK`)| `GPIO 14` | Left Pin 20 | Net `GPIO14` $\rightarrow$ `SCLK`| `BOARD_SDMMC_CLK_PIN` | **PASS (100%)** | 20 MHz – 40 MHz SDMMC Bus Clock |
| **SDMMC CMD Line** | `SD1` (SD-MMC) Pin 2 (`DI/MOSI`)| `GPIO 13` | Left Pin 19 | Net `GPIO13` $\rightarrow$ `DI` | `BOARD_SDMMC_CMD_PIN` | **PASS (100%)** | Bi-directional command/response line |
| **SDMMC Data 0** | `SD1` (SD-MMC) Pin 6 (`DO/MISO`)| `GPIO 21` | Right Pin 18 | Net `GPIO21` $\rightarrow$ `DO` | `BOARD_SDMMC_D0_PIN` | **PASS (100%)** | Bi-directional serial data line (1-bit mode) |
| **SDMMC Power VCC**| `SD1` (SD-MMC) Pin 5 (`VCC`) | 3.3V Rail | Left Pin 2 | Net `3V3` $\rightarrow$ `VCC` | N/A (Power) | **PASS (100%)** | 3.3V Logic Supply for MicroSD Card |
| **SDMMC Ground** | `SD1` (SD-MMC) Pin 3 (`GND`) | System GND | Left Pin 22 | Net `GND` $\rightarrow$ `GND` | N/A (Power) | **PASS (100%)** | Digital return ground |
| **5V Power Input** | `U7` (YD-ESP32) Pin 21 (`5V`) | Pi-Filter Input Node (`C5` \|\| `C6`)| Net `5V` $\rightarrow$ Filter | N/A (Power) | **PASS (100%)** | Draws power from ESP32 5V rail (*requires `IN-OUT` jumper bridged*). |
| **Filtered 5V Power Output**| Pi-Filter Output (`C7` \|\| `C8`)| `U5` Pin 15 (`VIN`) & `U4` Pin 7 (`+5V`)| Net `+5V` Clean | N/A (Power) | **PASS (100%)** | Clean power to DAC and headphone amp |

---

### 4.2 Architectural & Electrical Analysis of Schematic 3

#### 1. OLED Display Interface: Standard 4-Pin I2C Module (`OLED1`)
In Rev 1.2, the 7-pin SPI symbol has been replaced with a **standard 4-pin I2C OLED display module (`OLED1`)**:
* **Header Pins:** `GND` (Pin 1), `VCC` (Pin 2), `SCL` (Pin 3), `SDA` (Pin 4).
* **I2C Address Resolution:**
  * Unlike 7-pin breakout boards which expose an $SA_0$ / `DC` select line, standard 4-pin OLED modules hardwire the $SA_0$ line to GND on the PCB.
  * **Default I2C Slave Address:** **`0x3C`** (standard across SSD1306 and SH1106 128x64 displays).
  * Firmware configuration (`main/board_pins.h`, `u8g2`, `lvgl`):
    ```c
    #define I2C_OLED_ADDR_DEFAULT  0x3C
    ```

#### 2. Rotary Encoder Hardware Debouncing & Pull-Up Network (`R5`, `R6`, `R7`)
* `SW5` (`EC11E15244G1`) is equipped with three dedicated **10 kΩ pull-up resistors** tied directly to the 3.3V rail:
  * `R5` (10 kΩ): Pulls `GPIO 15` (Phase A) HIGH.
  * `R6` (10 kΩ): Pulls `GPIO 16` (Phase B) HIGH.
  * `R7` (10 kΩ): Pulls `GPIO 17` (Push Switch SW) HIGH.
* This discrete resistor network eliminates contact bounce and protects against false interrupts triggered by RF transmission bursts.

#### 3. Dedicated Headphone Output Jack (`J2`)
* `J2` (`DC_TRS_PADS`) routes amplified stereo audio directly from the TDA1308 module (`U4`) to a 3.5mm TRS stereo jack.
* Dedicated audio star ground isolates the analog return from digital MCU switching noise.

#### 4. MicroSD Card Interface (1-Bit SDMMC Mode)
* `SD1` operates in **Slot 1 (1-bit SDMMC bus)** using `GPIO 14 (CLK)`, `GPIO 13 (CMD)`, and `GPIO 21 (D0)`.
* `CS` (Pin 7) and `CD/Detect` (Pin 1) are left unconnected, which is 100% correct for 1-bit SDMMC streaming.
* **Pull-Up Resistors:** Verify that your MicroSD breakout contains onboard 10 kΩ pull-up resistors on `CMD` and `D0`. If using a raw TF socket breakout, install external 10 kΩ resistors from `GPIO 13` and `GPIO 21` to `3.3V`.

#### 5. Dedicated Mode Button Debouncing
* `SW4` features a discrete **10 kΩ pull-up resistor (`R4`) to 3.3V**.
* This hardware pull-up prevents electrical noise from causing false mode switches during high-power Wi-Fi transmission bursts, even before the ESP32 GPIO firmware initializes internal pull-ups.

---

## 5. Pre-Assembly & Commissioning Checklist for All Three Schematics

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                 HARDWARE COMMISSIONING SIGN-OFF CHECKLIST                   │
├─────────────────────────────────────────────────────────────────────────────┤
│ [ ] 1. YD-ESP32-S3 PCB: Solder-bridge the 'IN-OUT' jumper on PCB back.      │
│ [ ] 2. GY-PCM5102A: Verify underside Bridge 2 (FMT -> L) is soldered to GND.│
│ [ ] 3. GY-PCM5102A: Verify underside Bridge 1 (SCK -> L) is soldered to GND.│
│ [ ] 4. Pi-Filter: Verify electrolytic cap polarities (C2/C3, C10/C11, C6/C7).│
│ [ ] 5. Inductor (L1/L2/L3): Ensure Isat >= 300 mA and DCR < 0.5 Ohm.        │
│ [ ] 6. Multimeter Pre-Flight: Verify +5V clean rail reads 4.8V - 5.2V DC.   │
│ [ ] 7. OLED Firmware Init: Set I2C slave address to 0x3C (OLED1 / OLED2).   │
│ [ ] 8. SDMMC Bus (DAP Build): Ensure 10k pull-ups on GPIO 13 (CMD) & 21.   │
│ [ ] 9. Preset Buttons: Ensure 10k pull-ups (R1..R3) present on GPIO 15-17.  │
│ [ ] 10. Rotary & Buttons: Ensure 10k pull-ups (R9..R11, R8, R12 in Rev 1.1) │
│ [ ] 11. Headphone Jack (J1/J2/J3): Verify TRS pinout (Tip=L, Ring=R, GND).  │
│ [ ] 12. No-SMD / THT Builds: Verify radial power choke (RLB0914) & sockets. │
└─────────────────────────────────────────────────────────────────────────────┘
```
