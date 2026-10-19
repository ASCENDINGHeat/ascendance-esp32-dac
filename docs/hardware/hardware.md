# Hardware Specifications, Component BOM & Physical Routing Guide

> **Note for Agentic AI / AI Assistants:**  
> This document is the definitive hardware specification for this repository. It provides exact component part numbers, breakout PCB variants, pin-to-pin signal mappings, trace/wire routing rules, and RF/analog noise isolation requirements. When designing firmware drivers or diagnosing hardware issues, adhere strictly to the pin allocations and electrical invariants in Section 8.

---

## 1. Comprehensive Bill of Materials (BOM)

### 1.1 Core Processing & Audio Pipeline Components

| Item | Component | Exact Part / Module Model | Key Electrical Specifications | Package / Connector | Functional Role |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **MCU** | **YD-ESP32-S3 Development Board** | **ESP32-S3-WROOM-1-N16R8** (VCC-GND Studio DevKitC-1 clone, [Ref: rtek1000/YD-ESP32-23](https://github.com/rtek1000/YD-ESP32-23)) | • Xtensa® dual-core 32-bit LX7 @ 240 MHz<br>• 16 MB Quad SPI Flash (`W25Q128`)<br>• 8 MB Octal PSRAM (`APS6404L`)<br>• Dual USB Type-C: **COM** (WCH CH343P USB-to-UART) + **USB** (ESP32-S3 Native USB OTG)<br>• Onboard WS2812B RGB LED on `GPIO 48`<br>• Backside solder jumpers: `IN-OUT`, `USB-OTG`, `RGB`<br>• Schematic: `YD-ESP32-S3-SCH-V1.4` | 2 $\times$ 22-pin 2.54mm headers (0.9" spacing), dual USB Type-C female | Master audio streamer, HTTP client, DSP engine, I2S clock master. |
| **DAC** | **GY-PCM5102A Breakout Module** | Texas Instruments **PCM5102APWR** | • 32-bit / 384 kHz stereo audio DAC<br>• Dynamic Range & SNR: 112 dB<br>• THD+N: -93 dB typical<br>• DirectPath™ 2.1 $V_{\text{RMS}}$ ground-centered output<br>• Integrated PLL (synthesizes MCLK from BCK)<br>• Onboard 3.3V LDO regulator (ME6211 / AMS1117-3.3) | TSSOP-20 IC on purple PCB, 3.5mm TRS stereo jack + 6-pin 2.54mm header | High-resolution digital-to-analog audio conversion without DC output caps. |
| **Amp** | **TDA1308 Headphone Amplifier Module** | NXP Semiconductors **TDA1308 / TDA1308T** | • Class-AB stereo headphone driver<br>• Operating Voltage: 3.0 V to 7.0 V<br>• Output Power: 80 mW per channel into 32 $\Omega$ ($V_{DD} = 5\text{ V}$)<br>• THD+N: 0.03%, PSRR: 90 dB<br>• Input DC blocking caps: 10 $\mu$F 16V tantalum/electrolytic<br>• Feedback gain: $\sim$6 dB ($R_f = 20\text{ k}\Omega, R_{in} = 10\text{ k}\Omega$) | SO-8 IC on red/blue PCB, 3.5mm stereo output jack + 5-pin 2.54mm header | Low-impedance headphone driver and analog line-level buffer. |
| **Jack** | **3.5mm Stereo TRS Audio Jack (`J1` / `J2` / `J3`)** | PJ-320A / PJ-324M / **DC_TRS_PADS** | • 3-Pole 3.5mm Stereo Audio Socket<br>• Contact Rating: 30V DC @ 0.5A<br>• Pinout: Tip = Left, Ring = Right, Sleeve = Audio Reference Ground | 3-pin / 5-pin Through-Hole or Solder Pad | Line/Headphone stereo output jack connected directly to TDA1308 audio output stage. |
| **Power** | **5V Low-Noise DC Power Source** | Regulated 5V USB-C Charger or Bench Supply | • Output Voltage: 5.0 V $\pm$ 5% (4.75 V – 5.25 V)<br>• Minimum Output Current: 1.0 A (500 mA typical system draw)<br>• Ripple & Noise: $<50\text{ mV}_{\text{p-p}}$<br>• Note: Requires `IN-OUT` solder jumper bridged to power 5V pin via USB | USB-C Male Cable | Powers the ESP32-S3, GY-PCM5102A, and TDA1308 power rails. |

---

### 1.2 Hardware Extension Modules (UI, Storage & Controls)

| Item | Component | Exact Part / Module Model | Key Electrical Specifications | Package / Connector | Functional Role |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **Storage** | **MicroSD Card Socket Breakout (`SD1`)** | Push-Push MicroSD (TF) Breakout Module | • Voltage: 3.3V VCC (or 5V if onboard LDO present)<br>• Bus Width: 1-Bit (or 4-Bit) SDMMC<br>• Clock Frequency: 20 MHz to 40 MHz<br>• Built-in 10 kΩ pull-up resistor network on DAT0–DAT3 & CMD<br>• **Note:** Omitted in Variant 2 (`DSP+DAC+SCREEN.svg`), freeing `GPIO 13, 14, 21` | 9-pin MicroSD push-push slot to 6-pin 2.54mm header | High-speed local audio storage for FLAC, WAV, and MP3 files. |
| **Display** | **0.96" or 1.3" I2C Monochrome OLED (`OLED1` / `OLED2`)** | SSD1306 (0.96") or SH1106 (1.3") OLED | • Resolution: 128 $\times$ 64 pixels<br>• Interface: Standard 4-Pin I2C (`GND, VCC, SCL, SDA`)<br>• Supply Voltage: 3.3 V to 5.0 V<br>• Default I2C Address: **`0x3C`** | 4-pin 2.54mm header (`GND, VCC, SCL, SDA`) | Displays playback status, track metadata, bit depth/rate, volume bar, and mode menu. |
| **Rotary** | **EC11 Incremental Rotary Encoder (`SW7` / `SW5` / `SW2`)** | Alps Alpine / Bourns **EC11E15244G1** | • 20 pulses per revolution (PPR), 20 detents<br>• Integrated momentary SPST push switch<br>• Discrete 10 kΩ pull-ups (`R9..R11` in Rev 1.0/1.1, `R5..R7` in Rev 1.2) on Phase A, Phase B, and SW to 3.3V | 5-pin PCB module (A, B, C/GND, SW1, SW2) with 6mm D-shaft | Digital volume control, menu navigation, track skipping, and mode selection. |
| **Buttons** | **Tactile Pushbuttons** | Omron B3F-1000 or 6 $\times$ 6 $\times$ 5 mm SPST | • Contact Resistance: $<100\text{ m}\Omega$<br>• Actuation force: 160 gf<br>• Voltage rating: 12V DC, 50 mA | 4-pin through-hole tactile switch | Dual navigation buttons (`SW6` on GPIO 18 with pull-up `R8`, `SW8` on GPIO 10 with pull-up `R12` in Rev 1.1), mode switching (`SW4` in Rev 1.2), or 3 Preset selection buttons (`SW1..SW3` / `SW3..SW5` on GPIO 15, 16, 17 in DSP/DAC builds). |

---

### 1.3 Passive Filtering & Decoupling Components (Including Schematic Pi-Filter Network)

| Quantity | Component / Designator | Value / Rating | Recommended Part / Dielectric | Placement / Purpose |
| :--- | :--- | :--- | :--- | :--- |
| **1** | **`L1` / `L2` / `L3` (Pi-Filter Series Inductor)** | **10 $\mu$H, $\ge 300\text{ mA}$, $\text{DCR} < 0.5\,\Omega$** | SMD Power Inductor (Bourns SRR0604 / CD54 or Radial Choke) | In series between ESP32 5Vin and Audio +5V rail; blocks high-frequency RF/Wi-Fi ripple. |
| **2** | **Pi-Filter Electrolytic Caps** (`C2, C3` / `C10, C11` / `C6, C7`)| **10 $\mu$F, 16V or 25V** | Polarized Radial Aluminum Electrolytic or Tantalum | Input shunt and output shunt of the Pi-filter network. |
| **2** | **Pi-Filter Ceramic Caps** (`C1, C4` / `C9, C12` / `C5, C8`)| **0.1 $\mu$F (100 nF), 50V** | 0805 SMD / Radial MLCC (X7R dielectric) | Placed in parallel with 10µF caps for ultra-high-frequency decoupling (>1 MHz). |
| **1** | **Bulk Output Decoupling Capacitor** | 220 $\mu$F, 16V or 25V | Radial Low-ESR Aluminum Electrolytic (Panasonic FC / Nichicon PW) | Connected across 5V and GND at the audio amplifier stage for deep bass peak reserves. |
| **3** | **Preset Pull-Up Resistors (`R1..R3` / `R2..R4`)** | 10 k$\Omega$, 1/4W, 1% | Metal Film Through-Hole or 0805 SMD | Discrete pull-ups from GPIO 15, 16, 17 to 3.3V for SW1..SW3 / SW3..SW5 preset buttons. |
| **3** | **Rotary Encoder Pull-Up Resistors (`R9..R11` / `R5..R7`)** | 10 k$\Omega$, 1/4W, 1% | Metal Film Through-Hole or 0805 SMD | Discrete pull-ups from GPIO 15, 16, 17 to 3.3V for EC11 rotary encoder. |
| **1** | **Mode Button Pull-Up Resistor (`R8` / `R4` / `R1`)** | 10 k$\Omega$, 1/4W, 1% | Metal Film Through-Hole or 0805 SMD | Pulls Mode Button (`SW6` / `SW4` / `SW1` on GPIO 18) to 3.3V. |
| **2** | **SDMMC Bus Pull-Up Resistors** | 10 k$\Omega$, 1/4W, 1% | Metal Film Through-Hole or 0805 SMD | Pulls SDMMC `CMD` (GPIO 13) and `D0` (GPIO 21) to 3.3V if breakout lacks them. |
| **1** | **Star-Ground Bus Terminal** | 0.1" Male Header Strip / Solder Lug | Gold-plated brass | Central grounding node for clean analog audio return. |

---

### 1.4 No-SMD / 100% Through-Hole (THT) Build Guide & Parts Recommendation

For hobbyists, audio enthusiasts, students, and bench engineers who prefer **not to solder surface-mount components (SMD)**, the entire audio streamer and DAP can be assembled cleanly using **100% Through-Hole Technology (THT)** on a standard 2.54mm (0.1") pitch solderless breadboard, stripboard (Veroboard), or double-sided plated through-hole (PTH) perfboard.

#### The "Zero-SMD Soldering" Strategy
By taking advantage of pre-assembled, factory-tested breakout modules for the complex micro-pitch ICs (ESP32-S3 QFN, PCM5102A TSSOP-20, and TDA1308 SO-8), all builder soldering is strictly confined to standard **2.54mm (0.1") header pins, radial leaded capacitors, radial power inductors, axial metal film resistors, and through-hole electromechanical controls**. No hot-air station, solder paste stencil, or microscope is required—only a basic 25W–40W soldering iron.

```
                   MODULAR NO-SMD ARCHITECTURE (SOCKETED PERFBOARD)
     ┌────────────────────────────────────────────────────────────────────────┐
     │  [ 2x22-pin Female Sockets ]    [ 1x6-pin Socket ]   [ 1x5-pin Socket ]│
     │      YD-ESP32-S3 Board            GY-PCM5102A DAC      TDA1308 Amp     │
     │     (Pre-soldered headers)        (Pre-soldered)     (Pre-soldered)    │
     │               │                         │                   │          │
     │               │  I2S Bus (GPIO 4, 5, 6) │                   │          │
     │               ├─────────────────────────►                   │          │
     │               │                               Line Out      │          │
     │               │                             (Twisted Trio)  │          │
     │               │                             ────────────────►          │
     │               │                                                        │
     │  [ 100% Through-Hole Discrete Passives on 0.1" Grid ]                   │
     │   • L1: Radial Drum Choke (Bourns RLB0914-100KL, 10µH, 5mm pitch)      │
     │   • C1–C4: Radial Dipped MLCCs (KEMET GoldMax, 100nF, 2.54mm pitch)    │
     │   • C5–C8: Radial Low-ESR Electrolytics (Nichicon PW, 10µF, 2.5mm pitch)│
     │   • C_bulk: Radial 220µF 25V Low-ESR Reservoir Cap (5mm pitch)         │
     │   • R1–R7: Axial 1/4W 1% Metal Film Resistors (10 kΩ, 7.62mm pitch)    │
     │   • SW1, SW3–SW5: Omron B3F-1000 6x6mm Tactile Buttons (4-pin THT)     │
     │   • SW2: Bourns PEC11R / Alps EC11 THT Rotary Encoder (5-pin THT)      │
     │   • J1/J2: CUI SJ1-3523N / PJ-320A 3.5mm TRS Stereo Jack (THT)         │
     │   • Display: 0.96" I2C OLED with pre-soldered 4-pin male header        │
     │   • Storage: MicroSD Breakout with pre-soldered 6-pin male header      │
     └────────────────────────────────────────────────────────────────────────┘
```

#### 1.4.1 Complete No-SMD Bill of Materials & Part Numbers

| Subsystem / Role | Schematic Designator | Recommended Through-Hole (THT) Part | Manufacturer & Series | Lead Pitch / Package | Key Electrical Specifications | Supplier Part Number (Digi-Key / Mouser / LCSC) | Prototyping Suitability |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| **Microcontroller** | `U3` / `MCU` | **YD-ESP32-S3 DevKit** (with factory-soldered 2.54mm male pin headers) | VCC-GND Studio / Espressif | 2 $\times$ 22-pin, 0.1" pitch, 0.9" row spacing | 16MB Flash, 8MB Octal PSRAM, dual USB-C | LCSC: `C2913202` / AliExpress | Plugs into 2.54mm breadboard or dual 22-pin female header strips. |
| **I2S Audio DAC** | `U2` / `DAC` | **GY-PCM5102A Breakout Module** (purple PCB) | Texas Instruments PCM5102A on breakout | 1 $\times$ 6-pin 0.1" header + onboard 3.5mm jack | 32-bit / 384 kHz, SNR 112 dB, integrated PLL, onboard 3.3V LDO | LCSC: `C2890035` / Amazon / eBay | Plugs into breadboard or 6-pin female header strip. Zero SMD soldering. |
| **Headphone Amp** | `U1` / `U4` | **TDA1308 Stereo Amp Breakout Module** (red/blue PCB) | NXP TDA1308 on breakout | 1 $\times$ 5-pin 0.1" header + onboard 3.5mm jack | 80 mW into 32 $\Omega$, Class-AB, 3.0V–7.0V operating range | Amazon / AliExpress / eBay | Plugs into breadboard or 5-pin female header strip. Zero SMD soldering. |
| **Pi-Filter Series Inductor** | `L1` (or `L2`) | **Bourns RLB0914-100KL** *(or RLB0812-100KL)* | Bourns RLB Series | **Radial Drum Core Choke**, 5.0 mm lead spacing | **10 $\mu$H $\pm 10\%$, $I_{dc} = 1.6\text{ A}$, $\text{DCR} = 0.044\,\Omega$**, $f_{res} = 22\text{ MHz}$ | Digi-Key: `RLB0914-100KL-ND`<br>Mouser: `652-RLB0914-100KL` | **Crucial:** Never use tiny 0.1W axial color-ring inductors; they saturate $<80\text{ mA}$. This radial drum choke handles $>1.5\text{ A}$ without saturation. |
| **Pi-Filter Ceramic Caps** | `C1`, `C4` (Rev 1.3) / `C1`–`C4` (Rev 1.2) | **KEMET GoldMax C315C104K5R5TA** *(or Murata RPE / Vishay K104)* | KEMET GoldMax 300 Series | **Radial Dipped MLCC**, 2.54 mm (0.1") lead pitch | **0.1 $\mu$F (100 nF), 50V, X7R dielectric, $\pm 10\%$** | Digi-Key: `399-4329-ND`<br>Mouser: `80-C315C104K5R` | Fits directly into adjacent 0.1" breadboard/perfboard holes. High-frequency RF decoupling. |
| **Audiophile Film Alternative (Optional)** | `C1`, `C4` | **WIMA MKS2B031001B00** | WIMA MKS2 Series | **Radial Box Film**, 5.0 mm lead spacing | **0.1 $\mu$F (100 nF), 50V/63V, Metallized Polyester (PET)** | Mouser: `505-MKS20.1/63/10`<br>Digi-Key: `1928-1014-ND` | Lowest dielectric absorption and zero piezoelectric distortion; ideal for analog decoupling. |
| **Pi-Filter Electrolytic Caps** | `C2`, `C3` (Rev 1.3) / `C5`, `C7` (Rev 1.2) | **Nichicon UPW1E100MDD** *(or Panasonic EEU-FC1E100)* | Nichicon PW Series / Panasonic FC | **Radial Leaded Aluminum Electrolytic**, 2.0 mm / 2.5 mm lead pitch, 5 mm dia. | **10 $\mu$F, 25V, 105°C Low-ESR**, Ripple current: 95 mA | Digi-Key: `493-1815-ND`<br>Mouser: `647-UPW1E100MDD` | Compact low-ESR smoothing caps for the Pi-filter input and output shunts. |
| **Bulk Decoupling Reservoir Cap** | `C_bulk` | **Nichicon UPW1E221MPD** *(or Panasonic EEU-FR1E221)* | Nichicon PW / Panasonic FR | **Radial Leaded Aluminum Electrolytic**, 5.0 mm lead pitch, 8 mm dia. | **220 $\mu$F, 25V, 105°C Ultra-Low ESR**, Ripple current: 450 mA | Digi-Key: `493-1833-ND`<br>Mouser: `647-UPW1E221MPD` | Buffers high-amplitude headphone bass transients; keeps 5V power rail rock-solid. |
| **Pull-Up Resistors** | `R1` through `R7` | **Yageo MF0207FTE52-10K** *(or Vishay Dale CMF5510K000FHEK)* | Yageo MF0207 / Vishay Dale | **Axial Leaded Metal Film**, 7.62 mm standard horizontal breadboard pitch | **10 k$\Omega$, 1/4W (0.25W), $\pm 1\%$, 50 ppm/°C**, Low thermal noise | Digi-Key: `10.0KXBK-ND`<br>Mouser: `603-MF0207FTE52-10K` | Used for Mode Switch (`R1`), Presets (`R2–R4`), Encoder (`R5–R7`), and SDMMC bus pull-ups. |
| **Tactile Pushbuttons** | `SW1`, `SW3`, `SW4`, `SW5` | **Omron B3F-1000** *(or B3F-1020 with 7.3mm plunger)* | Omron B3F Series | **4-Pin Through-Hole Tactile Switch**, 6.0 $\times$ 6.0 mm, 0.1" $\times$ 0.2" pin grid | SPST-NO, 12V DC @ 50 mA, 100 m$\Omega$ max, 160 gf actuation force | Digi-Key: `SW400-ND`<br>Mouser: `653-B3F-1000` | Pins snap firmly into breadboards and 0.1" perfboards without tipping over. |
| **Rotary Encoder** | `SW2` | **Bourns PEC11R-4220F-S0024** *(or Alps EC11E15244G1)* | Bourns PEC11R Series / Alps EC11 | **5-Pin Through-Hole** with side retention brackets, 6mm D-shaft | 24 PPR, 24 detents, momentary SPST push switch, 2-channel quadrature | Digi-Key: `PEC11R-4220F-S0024-ND`<br>Mouser: `652-PEC11R-4220FS24` | 3 pins for encoder quadrature (A, C, B) + 2 pins for push button (SW1, SW2). Breadboard/perfboard mountable. |
| **Encoder Alternative (Breakout)** | `SW2` | **KY-040 Rotary Encoder Module** | Generic Hobbyist | 5-pin 2.54mm male header (`GND, +5V, SW, DT, CLK`) | 20 PPR with built-in 10 kΩ pull-up resistors | Amazon / AliExpress / LCSC | 100% solderless; connects via standard female DuPont jumper wires. |
| **OLED Display** | `OLED1` | **0.96" or 1.3" I2C OLED Module** (SSD1306 / SH1106) | Generic / Heltec / DIYmall | 4-pin 2.54mm (0.1") male header (`GND, VCC, SCL, SDA`) | 128 $\times$ 64 monochrome, default I2C address **`0x3C`**, 3.3V–5V compatible | Amazon / AliExpress / Adafruit `938` | Plugs directly into female socket or solderless breadboard. Zero SMD work. |
| **MicroSD Breakout** | `SD1` | **MicroSD (TF) Card Breakout Board** | Adafruit `254` / SparkFun `DEV-544` / Generic | 6-pin or 8-pin 2.54mm male header (`3V3, GND, CLK, CMD, D0, CD`) | Native 3.3V logic, push-push card socket, onboard 10k pull-up resistors | Digi-Key: `1528-1144-ND`<br>Mouser: `485-254` | Accepts standard MicroSD cards. 1-bit SDMMC bus wiring. |
| **Headphone Jack** | `J1` / `J2` | **CUI Devices SJ1-3523N** *(or PJ-320A / PJ-324M)* | CUI Devices / Generic | **Through-Hole 3.5mm Stereo Jack**, 3-pole TRS, 0.1" pitch compatible | 30V DC @ 0.5A, Tip = Left, Ring = Right, Sleeve = Ground | Digi-Key: `CP1-3523N-ND`<br>Mouser: `490-SJ1-3523N` | Rigid solder pins for perfboard; mates with all standard 3.5mm stereo headphones. |
| **Socket Strips** | — | **2.54mm (0.1") Single-Row Female Socket Headers** | Sullins Connector / Samtec | Standard 0.1" (2.54mm) pitch through-hole socket strip | 1 $\times$ 40-pin breakable strip (cut to: 2x 22-pin, 1x 6-pin, 1x 5-pin, 1x 4-pin) | Digi-Key: `S7000-ND`<br>Mouser: `855-M20-9994046` | Allows socketing all boards so modules can be removed or swapped without de-soldering. |
| **Hookup Wire** | — | **22 AWG or 24 AWG Solid-Core Tinned Copper Wire** | Remington Industries / Adafruit | Solid core, PVC/PTFE insulated | Rated 300V, 3.5A; bends and holds shape cleanly | Digi-Key: `1528-1111-ND` | Best wire for solderless breadboards and point-to-point underside perfboard tracks. |
| **Prototyping Board** | — | **Double-Sided Plated Through-Hole (PTH) FR-4 Perfboard** | Generic / ElectroCookie / Adafruit | 70 $\times$ 90 mm or 80 $\times$ 120 mm, standard 2.54mm (0.1") grid | High-quality fiberglass FR-4 with individual tinned through-hole solder pads | Amazon / AliExpress / Digi-Key | Provides high mechanical strength and reliable solder joints for point-to-point wiring. |

---

#### 1.4.2 Engineering Tips for Assembling a No-SMD Audio Player

##### 1. Choosing the Right Inductor (Avoiding Audio Pitfalls)
* **DO NOT use standard axial "color-ring" chokes (0.25W resistor shape):** Miniature axial RF chokes usually have a saturation current of only 50 mA to 80 mA and a very high internal DC resistance ($\text{DCR} > 5\,\Omega$). Under the ESP32-S3's periodic 300 mA Wi-Fi bursts, they will saturate, cause significant voltage drop (dropping the 5V rail below 4.0V), overheat, and produce severe audio distortion.
* **DO use radial drum core power inductors (such as Bourns RLB0914-100KL):** These components have a heavy ferrite core and thick copper windings rated for **$\ge 1.6\text{ A}$ saturation current and $\text{DCR} < 0.05\,\Omega$**. They drop virtually zero voltage while effectively blocking 100 kHz – 100 MHz RF noise.

##### 2. Low-ESR Capacitor Selection
* Standard cheap general-purpose electrolytic capacitors have high Equivalent Series Resistance (ESR) and poor high-frequency ripple filtering.
* Always select **105°C Low-ESR radial electrolytics** (such as **Nichicon PW/HE** or **Panasonic FC/FR**). These series are specifically engineered for switching power supplies and high-fidelity audio circuits.

##### 3. How to Handle the Solder Jumpers Without SMD Equipment
* **Bridging the YD-ESP32-S3 `IN-OUT` Jumper:**
  The `IN-OUT` jumper on the back of the board is a 2-pad jumper. You do **not** need SMD tweezers or solder paste. Simply clean the pads, apply a tiny drop of rosin flux, tin your regular soldering iron tip with standard 60/40 or lead-free solder wire, and bridge the two pads with a small solder blob. Alternatively, lay a tiny 2mm clipping from a through-hole resistor lead across the two pads and touch it with your iron.
* **Configuring the GY-PCM5102A Underside Bridges (1, 2, 3, 4):**
  The four solder jumpers (`1: SCK->L`, `2: FMT->L`, `3: FLT->L`, `4: DMP->L`) can be bridged using the exact same solder-blob technique. A standard 1.6mm chisel tip easily spans the gap between the middle pad and the "L" pad.

##### 4. Perfboard Wiring & Ground Plane Practice
* **Power & Ground Rails:** Run thick 22 AWG solid-core wire along the perimeter of the perfboard for the +5V and GND power busses.
* **Star-Ground Implementation:** Pick one solid solder pad near the power input as the central "Star Ground" hub. Solder all ground returns (digital MCU ground, DAC ground, and Amp ground) directly back to this hub rather than daisy-chaining them together.
* **Analog Audio Lines:** Route the three wires from the PCM5102A line out (`L_OUT`, `R_OUT`, `AGND`) to the TDA1308 inputs by **tightly twisting them together**. This creates a balanced rejection effect against Wi-Fi radiated magnetic fields.

---

## 2. Complete Interconnection & Pin Assignment Matrix

```
                          YD-ESP32-S3 Dev Board
        ┌────────────────────────────────────────────────────────┐
        │                                                        │
        │  [Audio DAC - I2S Master Out]                          │
        │  GPIO 4  ──────────────────────────────────────────────┼──► PCM5102A LCK (WS)
        │  GPIO 5  ──────────────────────────────────────────────┼──► PCM5102A DIN (Data)
        │  GPIO 6  ──────────────────────────────────────────────┼──► PCM5102A BCK (Clock)
        │  GPIO 7  ──────────────────────────────────────────────┼──► PCM5102A XMT (Soft Mute)
        │                                                        │
        │  [I2C Display: SSD1306 / SH1106]                       │
        │  GPIO 8  ──────────────────────────────────────────────┼──► OLED SDA
        │  GPIO 9  ──────────────────────────────────────────────┼──► OLED SCL
        │                                                        │
        │  [Rotary Encoder & User Controls]                      │
        │  GPIO 15 ──────────────────────────────────────────────┼──► Encoder Phase A (CLK)
        │  GPIO 16 ──────────────────────────────────────────────┼──► Encoder Phase B (DT)
        │  GPIO 17 ──────────────────────────────────────────────┼──► Encoder Push Switch (SW)
        │  GPIO 18 ──────────────────────────────────────────────┼──► Dedicated Mode Button
        │                                                        │
        │  [MicroSD Storage - SDMMC Slot 1]                      │
        │  GPIO 14 ──────────────────────────────────────────────┼──► SD CLK
        │  GPIO 13 ──────────────────────────────────────────────┼──► SD CMD (10k pull-up)
        │  GPIO 21 ──────────────────────────────────────────────┼──► SD D0  (10k pull-up)
        │                                                        │
        │  [Native USB OTG - UAC DAC Port]                       │
        │  GPIO 19 ──────────────────────────────────────────────┼──► USB D- (USB Type-C)
        │  GPIO 20 ──────────────────────────────────────────────┼──► USB D+ (USB Type-C)
        │                                                        │
        │  [Inter-Chip Header - Dual MCU Upgrade]                │
        │  GPIO 1  ──────────────────────────────────────────────┼──► INTER_I2S_WS  (RX)
        │  GPIO 2  ──────────────────────────────────────────────┼──► INTER_I2S_DIN (RX)
        │  GPIO 3  ──────────────────────────────────────────────┼──► INTER_I2S_BCK (RX)
        │  GPIO 11 ──────────────────────────────────────────────┼──► UART1 RX (from Hub)
        │  GPIO 12 ──────────────────────────────────────────────┼──► UART1 TX (to Hub)
        │                                                        │
        │  [UART Console & Flashing Port]                        │
        │  GPIO 43 ──────────────────────────────────────────────┼──► TXD0 (CH343 Bridge)
        │  GPIO 44 ──────────────────────────────────────────────┼──► RXD0 (CH343 Bridge)
        └────────────────────────────────────────────────────────┘
```

### 2.1 Master Interconnection Pin Table

| Subsystem | Signal Function | Source Pin (YD-ESP32-S3) | Physical Header Pin | Destination Module | Destination Pin Label | Electrical Characteristics / Signal Type | Required External Components |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| **I2S Audio** | Word Select / Frame Sync | **GPIO 4** | **Left Pin 4** | GY-PCM5102A | **LCK** | Output, 44.1 kHz / 48 kHz / 96 kHz square wave | None |
| **I2S Audio** | Serial Data Stream | **GPIO 5** | **Left Pin 5** | GY-PCM5102A | **DIN** | Output, 16-bit / 32-bit Philips format serial PCM | None |
| **I2S Audio** | Continuous Bit Clock | **GPIO 6** | **Left Pin 6** | GY-PCM5102A | **BCK** | Output, 1.411 MHz (44.1k $\times$ 32) to 3.072 MHz | Keep wire $<10\text{ cm}$ |
| **I2S Audio** | Hardware Soft Mute | **GPIO 7** | **Left Pin 7** | GY-PCM5102A | **XMT** | Output, Active-LOW (HIGH = Play, LOW = Mute) | Internal pulldown enabled |
| **Audio Line**| Left Channel Analog | GY-PCM5102A | — | TDA1308 Module | **IN_L** | Analog audio, line-level 2.1 $V_{\text{RMS}}$ | Twisted with AGND |
| **Audio Line**| Right Channel Analog | GY-PCM5102A | — | TDA1308 Module | **IN_R** | Analog audio, line-level 2.1 $V_{\text{RMS}}$ | Twisted with AGND |
| **Audio Line**| Analog Audio Ground | GY-PCM5102A | — | TDA1308 Module | **GND / AGND** | Dedicated analog audio return reference | Isolated from digital GND |
| **I2C Display**| Serial Data Line | **GPIO 8** | **Left Pin 12** | SSD1306 / SH1106 | **SDA** | Open-drain bi-directional, 400 kHz max | 4.7 kΩ pull-up on breakout |
| **I2C Display**| Serial Clock Line | **GPIO 9** | **Left Pin 15** | SSD1306 / SH1106 | **SCL** | Open-drain output, 400 kHz square wave | 4.7 kΩ pull-up on breakout |
| **Rotary / BTN** | Quadrature Phase A / Preset 1 | **GPIO 15** | **Left Pin 8** | EC11 / SW1 / SW3 | **A / Pin 1** | Input, digital pulse stream / active-low | 10 kΩ pull-up to 3.3V (`R9` / `R5` / `R2`) |
| **Rotary / BTN** | Quadrature Phase B / Preset 2 | **GPIO 16** | **Left Pin 9** | EC11 / SW2 / SW5 | **B / Pin 1** | Input, 90° shifted pulse stream / active-low | 10 kΩ pull-up to 3.3V (`R10` / `R7` / `R3`) |
| **Rotary / BTN** | Switch (SW) / Preset 3 | **GPIO 17** | **Left Pin 10** | EC11 / SW3 / SW4 | **SW / Pin 1** | Input, momentary active-low | 10 kΩ pull-up to 3.3V (`R11` / `R6` / `R4`) |
| **Controls** | Button 1 (◄ / Menu / Back)| **GPIO 18** | **Left Pin 11** | Tactile Switch | **SW6 / SW4 / SW1** | Input, momentary active-low | 10 kΩ pull-up to 3.3V (`R8` / `R4` / `R1`) |
| **Controls** | Button 2 (► / Next / Preset)| **GPIO 10** | **Left Pin 16** | Tactile Switch | **SW_NAV2** | Input, momentary active-low | Internal pull-up enabled (direct to GND) |
| **SDMMC** | SD Clock | **GPIO 14** | **Left Pin 20** | MicroSD (`SD1`) | **CLK** | Output, 20 MHz to 40 MHz (free in Variant 2) | Keep wire $<10\text{ cm}$ |
| **SDMMC** | Command & Response | **GPIO 13** | **Left Pin 19** | MicroSD (`SD1`) | **CMD** | Bi-directional open-drain (free in Variant 2) | **10 kΩ pull-up to 3.3V** |
| **SDMMC** | Data Line 0 | **GPIO 21** | **Right Pin 18** | MicroSD (`SD1`) | **D0 / DAT0** | Bi-directional serial data (free in Variant 2) | **10 kΩ pull-up to 3.3V** |
| **USB DAC** | USB Data Negative | **GPIO 19** | **Right Pin 20** | Native USB Port | **D-** | Full-Speed USB (12 Mbps differential) | Internal USB PHY |
| **USB DAC** | USB Data Positive | **GPIO 20** | **Right Pin 19** | Native USB Port | **D+** | Full-Speed USB (12 Mbps differential) | Internal USB PHY |
| **Visual Indicator**| WS2812B aRGB Data | **GPIO 48** | **Right Pin 16** | Onboard LED | **DIN** | RMT Master, single-wire 800 kHz NRZ | Bridged `RGB` jumper |
| **Inter-Chip** | Audio Word Select RX | **GPIO 1** | **Right Pin 4** | ESP32 Classic | **GPIO 25** | Input, Inter-chip I2S Frame Sync | Optional Dual-MCU link |
| **Inter-Chip** | Audio Serial Data RX | **GPIO 2** | **Right Pin 5** | ESP32 Classic | **GPIO 26** | Input, Inter-chip I2S Serial PCM Stream | Optional Dual-MCU link |
| **Inter-Chip** | Audio Bit Clock RX | **GPIO 3** | **Left Pin 13** | ESP32 Classic | **GPIO 22** | Input, Inter-chip I2S Bit Clock | Optional Dual-MCU link |
| **Inter-Chip** | Control UART RX | **GPIO 11** | **Left Pin 17** | ESP32 Classic | **GPIO 17 (TX)** | Input, 921,600 baud asynchronous serial | Optional Dual-MCU link |
| **Inter-Chip** | Control UART TX | **GPIO 12** | **Left Pin 18** | ESP32 Classic | **GPIO 16 (RX)** | Output, 921,600 baud asynchronous serial | Optional Dual-MCU link |
| **Power 5V** | 5V DC Power Rail | **5Vin / 5V** | **Left Pin 21** | DAC & Amp | **VCC** | Regulated 4.75V – 5.25V DC | **Requires IN-OUT bridged** |
| **Power 3.3V**| 3.3V Logic Rail | **3.3V** | **Left Pin 1 & 2**| OLED & MicroSD | **VCC** | Regulated 3.3V DC from ESP32 LDO | 100 nF bypass cap |
| **Ground** | System Ground | **GND** | **Left Pin 22, Right Pins 1, 21, 22** | All Modules | **GND** | 0V System reference | Star-ground topology |

---

## 3. YD-ESP32-S3 Board Architecture, Schematics & Physical Layout (Ref: rtek1000/YD-ESP32-23)

The **YD-ESP32-S3** (also referenced as **YD-ESP32-23** or VCC-GND Studio DevKitC-1 clone) is documented extensively in the community repository [rtek1000/YD-ESP32-23](https://github.com/rtek1000/YD-ESP32-23) and Renzo Mischianti's hardware analysis.

### 3.1 Physical Pin Layout Diagram

The board features two 22-pin headers spaced 0.9" (22.86 mm) apart. The following physical diagram represents the board viewed from the **top component side**, oriented with the **PCB Wi-Fi antenna at the TOP** and the **dual USB-C connectors at the BOTTOM**:

```
                       ┌──────────────────────────┐
                       │  ESP32-S3-WROOM-1 Module │
                       │    (PCB Meander Antenna) │
                       └──────────────────────────┘
                  Left Header (J1)            Right Header (J2)
               ┌─────────────────────────────────────────────────┐
  3.3V Output  │ [ 1] 3V3                     GND [ 1]           │  Power Return (GND)
  3.3V Output  │ [ 2] 3V3                    GPIO 43 [ 2] (TX)   │  U0TXD (CH343 Console TX)
  Reset (EN)   │ [ 3] RST                    GPIO 44 [ 3] (RX)   │  U0RXD (CH343 Console RX)
  I2S LCK (WS) │ [ 4] GPIO 4                  GPIO 1  [ 4]       │  Inter-Chip I2S WS RX
  I2S DIN      │ [ 5] GPIO 5                  GPIO 2  [ 5]       │  Inter-Chip I2S DIN RX
  I2S BCK      │ [ 6] GPIO 6                  GPIO 42 [ 6]       │  Auxiliary GPIO
  DAC Mute XMT │ [ 7] GPIO 7                  GPIO 41 [ 7]       │  Auxiliary GPIO
  Rotary Enc A │ [ 8] GPIO 15                 GPIO 40 [ 8]       │  SD Card Detect (CD)
  Rotary Enc B │ [ 9] GPIO 16                 GPIO 39 [ 9]       │  SDMMC D3 (4-bit mode)
  Rotary Push  │ [10] GPIO 17                 GPIO 38 [10]       │  SDMMC D2 (4-bit mode)
  Btn 1 (◄/Menu)│ [11] GPIO 18                 GPIO 37 [11]       │  [FORBIDDEN: OCTAL PSRAM]
  Display SDA  │ [12] GPIO 8                  GPIO 36 [12]       │  [FORBIDDEN: OCTAL PSRAM]
  Inter-Chip BCK│ [13] GPIO 3                 GPIO 35 [13]       │  [FORBIDDEN: OCTAL PSRAM]
  Log RX/Boot  │ [14] GPIO 46                 GPIO 0  [14]       │  BOOT Button (IO0)
  Display SCL  │ [15] GPIO 9                  GPIO 45 [15]       │  Auxiliary GPIO
  Btn 2 (►/Next)│ [16] GPIO 10                 GPIO 48 [16]       │  Onboard WS2812B RGB LED
  UART1 RX     │ [17] GPIO 11                 GPIO 47 [17]       │  SDMMC D1 (4-bit mode)
  UART1 TX     │ [18] GPIO 12                 GPIO 21 [18]       │  SDMMC D0 (10k pull-up)
  SDMMC CMD    │ [19] GPIO 13                 GPIO 20 [19]       │  Native USB D+ (USB OTG)
  SDMMC CLK    │ [20] GPIO 14                 GPIO 19 [20]       │  Native USB D- (USB OTG)
  5V In / Out  │ [21] 5Vin                    GND     [21]       │  Power Return (GND)
  Power Return │ [22] GND                     GND     [22]       │  Power Return (GND)
               └─────────────────────────────────────────────────┘
                   ┌──────────────┐             ┌──────────────┐
                   │  USB (TypeC) │             │  COM (TypeC) │
                   │  Native OTG  │             │  CH343 UART  │
                   └──────────────┘             └──────────────┘
                     (Left Port)                  (Right Port)
```

---

### 3.2 Dual USB Type-C Architecture

The YD-ESP32-S3 PCB houses two independent USB Type-C female receptacles with distinct electrical paths (documented in schematic `YD-ESP32-S3-SCH-V1.4`):

1. **COM Port (Right USB-C Receptacle):**
   * **Bridge Controller:** WCH **CH343P** USB-to-UART converter (`U2`).
   * **Serial Routing:** Connected to ESP32-S3 hardware UART0 (`GPIO 43: U0TXD` and `GPIO 44: U0RXD`).
   * **Status LEDs:** Onboard `TX` and `RX` SMD indicator LEDs illuminate during data transmission.
   * **Auto-Flashing Subsystem:** NPN transistor pair `Q1` and `Q2` (SS8050) assert `DTR` and `RTS` control lines to automatically pull `CHIP_PU` (RST) and `GPIO 0` (BOOT) low for hands-free flashing via `idf.py flash`.
2. **USB Port (Left USB-C Receptacle):**
   * **Controller:** Direct differential connection to the ESP32-S3 internal USB PHY (`GPIO 19: D-` and `GPIO 20: D+`).
   * **Functional Role:** Used for **USB Audio Class 1.0 (UAC DAC)**, native JTAG debugging, and USB OTG host/device modes.

---

### 3.3 Onboard Solder Jumpers: `IN-OUT`, `USB-OTG`, and `RGB`

The PCB incorporates three surface-mount solder jumpers (0603 footprint pads) that configure power routing and onboard peripherals:

```
           Onboard Solder Jumpers (PCB Front & Back)
   ┌────────────────────────────────────────────────────────┐
   │                                                        │
   │   [ IN-OUT ]       [ USB-OTG ]             [ RGB ]     │
   │    ○───○            ○───○                   ○───○      │
   │  (Near 5Vin)     (Between USB Ports)     (Near RGB LED)│
   │                                                        │
   └────────────────────────────────────────────────────────┘
```

#### 1. `IN-OUT` Jumper (Diode Bypass — CRITICAL FOR AUDIO MODULES):
* **Default State:** **OPEN (Unbridged from factory)**.
* **Electrical Behavior:**
  * When OPEN, a Schottky protection diode (`D3`, marked D26 / SS14) is in series between USB VBUS and the `5Vin` header pin (Pin 21). External power fed into `5Vin` can power the board through the diode, but **USB power does NOT flow out to the `5Vin` header pin** (the diode is reverse-biased from USB VBUS to the header).
  * Consequently, with `IN-OUT` open, the `5Vin` pin reads floating or ~3.3V leakage when the board is powered solely through USB.
* **Bridged State (Soldered):**
  * Bridges across diode `D3`, shorting USB VBUS directly to header pin `5Vin` (Pin 21).
  * **MANDATORY CONFIGURATION:** To power the **GY-PCM5102A DAC** and **TDA1308 headphone amplifier** from the board's `5Vin` pin via USB power, you **MUST bridge the `IN-OUT` jumper with a bead of solder**.
* **CRITICAL SAFETY WARNING:**
  * If `IN-OUT` is bridged, **NEVER** apply an external 5V power supply to the `5Vin` pin while the USB-C cable is plugged into a PC or charger. Because the diode is bypassed, power will back-feed into your computer's USB port and could cause hardware damage.

#### 2. `USB-OTG` Jumper (VBUS Paralleling):
* **Default State:** **OPEN (Unbridged)**.
* **Electrical Behavior:**
  * Connects the VBUS power rails of both USB-C connectors (`COM` and `USB`) together.
  * When bridged, plugging a 5V USB supply into either port supplies power to both ports simultaneously, allowing the board to provide 5V VBUS to a downstream USB device when operating in USB Host (OTG) mode.

#### 3. `RGB` Jumper (WS2812B Power / Data Gate):
* **Default State:** **CLOSED (Bridged with factory trace/solder)**.
* **Electrical Behavior:**
  * Connects `GPIO 48` to the DIN (Data In) pin of the onboard **XL-5050RGBC-WS2812B** smart LED (`U1`).
  * If the jumper is cut or de-soldered, it isolates `GPIO 48` for general-purpose GPIO use and completely eliminates the ~1 mA quiescent standby current of the WS2812B IC for ultra-low-power battery applications.

---

### 3.4 5V Rail Hardware Warning & Multimeter Verification (Issue #3)

> [!CAUTION]
> **Issue #3 in rtek1000/YD-ESP32-23 (5V Pin Floating or Reading 3.3V):**  
> In community testing (e.g. GitHub issue [#3](https://github.com/rtek1000/YD-ESP32-23/issues/3) on board revision V1120 / V1.3 / V1.4), users observed that the `5Vin` pin does not output 5V when the board is powered via USB-C unless the `IN-OUT` jumper is bridged.
> 
> **Mandatory Pre-Flight Multimeter Procedure:**
> 1. Plug the YD-ESP32-S3 into a 5V USB-C power source.
> 2. Set your digital multimeter to DC Volts ($20\text{ V}$ range).
> 3. Place the black probe on `GND` (Left Pin 22) and red probe on `5Vin` (Left Pin 21).
> 4. **If voltage reads $\approx 0\text{ V}$ to $3.3\text{ V}$:** The `IN-OUT` jumper is open. Solder bridge the `IN-OUT` jumper pads.
> 5. **If voltage reads $4.8\text{ V} - 5.2\text{ V}$:** The 5V rail is active and ready to power the GY-PCM5102A DAC and TDA1308 amplifier.

---

### 3.5 Onboard WS2812B Addressable RGB LED (`GPIO 48`) Protocol Details

* **LED Part:** Worldsemi **WS2812B** / XL-5050RGBC-WS2812B (`U1`).
* **Control Pin:** **`GPIO 48`** (routed through the `RGB` jumper).
* **Protocol Requirement:**
  * The WS2812B uses a high-speed single-wire Non-Return-to-Zero (NRZ) protocol operating at 800 kHz ($1.25\text{ }\mu\text{s}$ bit period, $0.4\text{ }\mu\text{s}$ / $0.8\text{ }\mu\text{s}$ duty cycle pulses).
  * **DO NOT** attempt to control `GPIO 48` with standard digital GPIO commands (`gpio_set_level()` or `digitalWrite()`). This will fail and leave the LED unlit or stuck on white.
  * In **ESP-IDF v6.1**, the firmware drives the RGB LED using the native `esp_driver_rmt` (Remote Control peripheral) configured at 10 MHz resolution (100 ns/tick) and a custom bytes encoder transmitting GRB frames.
* **Firmware Loudness & Mute Indicator Integration:**
  * **Volume Loudness Indicator:** The LED's brightness dynamically tracks the master playback volume ($0\%\dots 100\%$).
  * **Optical Safety Limit:** WS2812B LEDs at 100% duty cycle draw up to 60 mA and cause visual discomfort. Software limits the maximum brightness to **~25% duty cycle** (`MAX_BRIGHTNESS = 60` out of 255), keeping the LED completely cool and glare-free without needing an external diffuser.
  * **Mute Alert Mode:** When audio is muted, the LED switches to a gentle Red glow (`R=25, G=0, B=0`), providing immediate visual status across the room.
  * **Color Temperature Mapping:**
    - `0% Volume`: LED completely off.
    - `1% - 35% Volume`: Subtle Deep Blue / Teal glow.
    - `36% - 75% Volume`: Pure Cyan optimal listening indicator.
    - `76% - 100% Volume`: Dynamic Mint / White-Cyan peak loudness indicator.
* **Jumper Configuration:** The backside `RGB` solder jumper must remain closed (factory default) for `GPIO 48` to drive the LED.

---

## 4. GY-PCM5102A Hardware Configuration & Jumper Solder Bridges

The GY-PCM5102A module features four configuration mode pins. On standard purple breakout boards, solder bridges (`1, 2, 3, 4`) on the underside control these pins:

```
          GY-PCM5102A Breakout Backside
         ┌──────────────────────────────┐
         │   [ 1 ]    [ 2 ]    [ 3 ]    [ 4 ]
         │   SCK      FMT      FLT      DMP
         │  H  L     H  L     H  L     H  L
         │  ○──●     ○──●     ○──●     ○──●
         └──────────────────────────────┘
         (Bridge to 'L' grounds the signal)
```

1. **Bridge 1 (SCK $\rightarrow$ GND / "L"):** Enables the internal integrated Phase-Locked Loop (PLL). The DAC detects `BCK` on GPIO 6 and synthesizes high-frequency Master Clock (MCLK) internally. If open (`H`), DAC will produce complete silence.
2. **Bridge 2 (FMT $\rightarrow$ GND / "L"):** Selects standard **Philips I2S format** (MSB delayed 1 bit clock from WS edge). If bridged to `H`, expects Left-Justified (LJ) audio, causing static noise.
3. **Bridge 3 (FLT $\rightarrow$ GND / "L"):** Selects **Normal Latency (FIR) Digital Filter** (flattest phase and frequency response).
4. **Bridge 4 (DMP $\rightarrow$ GND / "L"):** Disables analog de-emphasis attenuation.
5. **Front Header Pin XMT (Soft Mute):**
   * Connect to **GPIO 7** on ESP32-S3 for software-controlled zero-pop muting.
   * If GPIO 7 is not used, wire permanently to **3.3V** to unmute. *(Never leave XMT floating).*

---

## 5. Signal Routing, EMI Mitigation & Audiophile Grounding Architecture

Wi-Fi transmission bursts draw periodic 300 mA current spikes. Without careful routing, these spikes inject high-frequency RF ripple into the audio line, creating audible **"Wi-Fi chirp" or 50/60 Hz hum**.

### 5.1 The Star-Grounding Rule (Absolute Requirement)

```
                              5V Clean Supply
                                     │
                    ┌────────────────┴────────────────┐
                    │                                 │
                    ▼                                 ▼
            [ YD-ESP32-S3 5V ]              [ Bulk 220µF + 100nF ]
                    │                                 │
                    │ Digital 3.3V                    ├───────────────┐
                    ▼                                 │ 5V Clean      │ 5V Clean
          [ OLED & SDMMC VCC ]                        ▼               ▼
                                              [ PCM5102A VCC ]  [ TDA1308 VCC ]
                                                      │               │
  DIGITAL GROUND DOMAIN                               │ Analog Audio  │
  [ ESP32 GND ]──[ OLED GND ]──[ SD GND ]             │ Signals       │
        │                                             ▼               ▼
        │                                     [ L_OUT, R_OUT, AGND ]──► [ IN_L, IN_R, GND ]
        │                                                             │
        └───────────────────────┬─────────────────────────────────────┘
                                │ (Single-Point Star Ground Return)
                                ▼
                       [ SYSTEM POWER GND ]
```

1. **Dedicated Analog Return:** Connect the `AGND` pin of the PCM5102A directly to the `GND` input pin of the TDA1308. Do **not** route this ground through the ESP32-S3 board or daisy-chain it through digital ground lines.
2. **Star-Point Connection:** Bring the power ground of the ESP32-S3, the power ground of the TDA1308, and the supply ground together at a single physical junction (star ground).
3. **No Ground Loops:** Never create closed circular ground paths between the DAC, amplifier, and microcontroller.

---

### 5.2 High-Speed Digital Routing Guidelines

1. **I2S Bus Routing (GPIO 4, 5, 6):**
   * Maximum jumper wire length: **10 cm to 12 cm**.
   * Run a dedicated **GND ground wire twisted or parallel** alongside the I2S lines (`GPIO 4: WS`, `GPIO 5: DIN`, `GPIO 6: BCK`) to provide a low-inductance return path and shield neighboring analog wires from digital clock radiation.
   * Never route I2S lines directly under or parallel to the onboard Wi-Fi meander PCB antenna.
2. **MicroSD SDMMC Bus Routing (GPIO 14, 13, 21):**
   * The `CLK` line (GPIO 14) toggles at 20 MHz to 40 MHz with fast edge rates (<2 ns). Keep wire length under **8 cm to 10 cm**.
   * Place 10 kΩ pull-up resistors within **2 cm** of the MicroSD card socket pins.
3. **Analog Audio Interconnect (PCM5102A to TDA1308):**
   * Twist `L_OUT`, `R_OUT`, and `AGND` together into a tight twisted trio (or use a shielded miniature coaxial cable).
   * Maintain at least **25 mm (1 inch)** physical separation between analog audio lines and the digital MicroSD / I2C / Wi-Fi antenna traces.

---

### 5.3 Rotary Encoder RC Debounce Filter Circuit

Mechanical switches bounce electrically for 2 ms to 5 ms upon actuation. Install this hardware RC low-pass filter on both Channel A (`GPIO 15`) and Channel B (`GPIO 16`) to completely eliminate contact chatter:

```
             3.3V Logic Rail
                   │
                   │  10 kΩ 1/4W Resistor
                  [R]
                   │
  GPIO 15 (CLK) ───┼──────────────────────────► Encoder Pin A
                   │
                  ===  100 nF Ceramic MLCC
                   │
                  GND
```

* **Cutoff Frequency:**
  $$f_c = \frac{1}{2\pi R C} = \frac{1}{2\pi \times 10,000 \times 100 \times 10^{-9}} \approx 159\text{ Hz}$$
* This hardware filter rejects mechanical contact spikes narrower than 1 ms while cleanly passing user rotations up to 1,000 RPM.

---

## 6. Power Distribution & Current Consumption Budget

| Subsystem | Operating Voltage | Average Current (Idle) | Peak Current (Active) | Power Rail Source |
| :--- | :--- | :--- | :--- | :--- |
| **ESP32-S3 CPU & Memory** | 3.3 V (Internal LDO) | 70 mA | 120 mA | Onboard 3.3V LDO |
| **Wi-Fi Radio (Active TX)** | 3.3 V (Internal LDO) | 0 mA (when off) | 310 mA (bursts) | Onboard 3.3V LDO |
| **GY-PCM5102A DAC** | 5.0 V (Onboard 3.3V LDO)| 25 mA | 35 mA | Main 5V Rail (`5Vin`, Left Pin 21, requires `IN-OUT` bridged) |
| **TDA1308 Amp (32Ω load)** | 5.0 V | 15 mA | 110 mA (loud peaks) | Main 5V Rail (`5Vin`, Left Pin 21, requires `IN-OUT` bridged) |
| **0.96" SSD1306 OLED** | 3.3 V | 10 mA | 25 mA (100% white) | ESP32-S3 3.3V Pin (Left Pin 1 or 2) |
| **MicroSD Card (SDMMC)** | 3.3 V | 5 mA | 80 mA (burst write) | ESP32-S3 3.3V Pin (Left Pin 1 or 2) |
| **Total System Draw** | **5.0 V DC** | **$\sim$125 mA** | **$\sim$680 mA** | **USB-C (1.0A rating min)** |

> [!IMPORTANT]
> **5V Rail Power Source & IN-OUT Jumper Requirement:**  
> Always power the **TDA1308** and **GY-PCM5102A** from the `5Vin` pin (Left Header Pin 21), **NOT** from the MCU's `3.3V` pin. The onboard 3.3V LDO (CJ6107A33GW) on the YD-ESP32-S3 is rated for only 500–800 mA; powering external audio amplifiers from it causes thermal throttling, rail sagging, and severe clipping distortion.  
> **Crucial Note:** When powering via USB-C, you **must solder-bridge the `IN-OUT` jumper** on the YD-ESP32-S3 PCB to connect USB VBUS to `5Vin` across diode D3. Otherwise, `5Vin` will read floating or ~3.3V and external modules will not power up (Ref: [rtek1000/YD-ESP32-23](https://github.com/rtek1000/YD-ESP32-23) Issue #3).

---

## 7. Dual-MCU Inter-Chip Expansion Header

When implementing the **Dual-MCU upgrade** (offloading Wi-Fi, Bluetooth A2DP, and MicroSD to an auxiliary ESP32 Classic):

```
     ESP32 Classic (Media Hub)                   YD-ESP32-S3 (DSP Engine)
   ┌───────────────────────────┐               ┌───────────────────────────┐
   │ GPIO 25 (I2S_WS Output)   ├──────────────►│ GPIO 1 (I2S_WS Input)     │
   │ GPIO 26 (I2S_DATA Output) ├──────────────►│ GPIO 2 (I2S_DATA Input)   │
   │ GPIO 22 (I2S_BCK Output)  ├──────────────►│ GPIO 3 (I2S_BCK Input)    │
   │ GPIO 17 (UART2 TXD)       ├──────────────►│ GPIO 11 (UART1 RXD)       │
   │ GPIO 16 (UART2 RXD)       │◄──────────────┤ GPIO 12 (UART1 TXD)       │
   │ GND                       ├───────────────┤ GND (Common Digital GND)  │
   └───────────────────────────┘               └───────────────────────────┘
```

---

## 8. Reference for AI Agents & Autonomous Coders

When generating hardware initialization code, device trees, or pin definition headers, AI agents must enforce these rules:

### 8.1 Forbidden GPIOs (Catastrophic Kernel Panic Prevention)
* **NEVER USE `GPIO 33, 34, 35, 36, 37`:** These pins are bonded directly to the internal 8MB Octal PSRAM. Initializing them or configuring them as GPIOs corrupts memory caching and triggers an unrecoverable `Cache error: Cache access while memory is disabled`.
* **NEVER USE `GPIO 26, 27, 28, 29, 30, 31, 32`:** Dedicated to Quad SPI Flash memory.

### 8.2 Canonical Header Definitions (`board_pins.h`)
```c
#pragma once
#include "driver/gpio.h"

// =============================================================================
// YD-ESP32-S3 (YD-ESP32-23) Canonical Pin Definitions
// Ref: https://github.com/rtek1000/YD-ESP32-23 (Schematic V1.4)
// =============================================================================

// --- Primary I2S Audio Output (Master to PCM5102A DAC) ---
#define BOARD_I2S_WS_PIN         GPIO_NUM_4   // Word Select / LCK (Left Header Pin 4)
#define BOARD_I2S_DATA_OUT_PIN   GPIO_NUM_5   // Serial Audio Data / DIN (Left Header Pin 5)
#define BOARD_I2S_BCK_PIN        GPIO_NUM_6   // Continuous Bit Clock / BCK (Left Header Pin 6)
#define BOARD_DAC_MUTE_PIN       GPIO_NUM_7   // Soft Mute XMT (Left Header Pin 7, HIGH = Play, LOW = Mute)

// --- I2C Monochrome Display (SSD1306 / SH1106) ---
#define BOARD_I2C_SDA_PIN        GPIO_NUM_8   // I2C Data Line (Left Header Pin 12)
#define BOARD_I2C_SCL_PIN        GPIO_NUM_9   // I2C Clock Line (Left Header Pin 15)
#define I2C_OLED_ADDR_DEFAULT    0x3C         // Standard 4-pin I2C OLED (OLED1 in Rev 1.2)
#define I2C_OLED_ADDR_ALT        0x3D         // Legacy 7-pin SPI module with DC tied to 3V3

// --- Rotary Encoder & User Inputs (EC11 Quadrature & Dual Navigation Buttons) ---
#define BOARD_ENC_A_PIN          GPIO_NUM_15  // Phase A (Left Header Pin 8, PCNT filtered)
#define BOARD_ENC_B_PIN          GPIO_NUM_16  // Phase B (Left Header Pin 9, PCNT filtered)
#define BOARD_ENC_SW_PIN         GPIO_NUM_17  // Integrated Push Switch (Left Header Pin 10, Active-Low)
#define BOARD_BTN_MODE_PIN       GPIO_NUM_18  // Dedicated Mode Pushbutton (Left Header Pin 11, Active-Low)
#define BOARD_BTN_1_PIN          GPIO_NUM_18  // Navigation Button 1 / Left (Left Header Pin 11, ◄ / Menu)
#define BOARD_BTN_2_PIN          GPIO_NUM_10  // Navigation Button 2 / Right (Left Header Pin 16, ► / Next)

// --- Alternative 3-Button Preset Controller (Schematic 1, Rev 1.3) ---
// When EC11 Rotary Encoder is omitted, GPIO 15, 16, 17 are used for instant preset switching:
#define BOARD_BTN_PRESET_1_PIN   GPIO_NUM_15  // SW3: Preset 1 (e.g., Reference Flat / Web Stream 1)
#define BOARD_BTN_PRESET_2_PIN   GPIO_NUM_16  // SW5: Preset 2 (e.g., Bass Boost / Web Stream 2)
#define BOARD_BTN_PRESET_3_PIN   GPIO_NUM_17  // SW4: Preset 3 (e.g., AutoEQ Harman / Web Stream 3)

// --- MicroSD Storage (SDMMC Host Slot 1) ---
#define BOARD_SDMMC_CLK_PIN      GPIO_NUM_14  // SD Clock Line (Left Header Pin 20)
#define BOARD_SDMMC_CMD_PIN      GPIO_NUM_13  // SD Command Line (Left Header Pin 19, 10k pull-up required)
#define BOARD_SDMMC_D0_PIN       GPIO_NUM_21  // SD Data 0 Line (Right Header Pin 18, 10k pull-up required)
#define BOARD_SDMMC_D1_PIN       GPIO_NUM_47  // SD Data 1 (Right Header Pin 17, 4-bit mode)
#define BOARD_SDMMC_D2_PIN       GPIO_NUM_38  // SD Data 2 (Right Header Pin 10, 4-bit mode)
#define BOARD_SDMMC_D3_PIN       GPIO_NUM_39  // SD Data 3 (Right Header Pin 9, 4-bit mode)
#define BOARD_SDMMC_CD_PIN       GPIO_NUM_40  // Optional Card Detect Switch (Right Header Pin 8)

// --- Native USB OTG Port (USB Audio Class 1.0) ---
#define BOARD_USB_DM_PIN         GPIO_NUM_19  // USB D- (Right Header Pin 20 / Left Type-C Receptacle)
#define BOARD_USB_DP_PIN         GPIO_NUM_20  // USB D+ (Right Header Pin 19 / Left Type-C Receptacle)

// --- UART Console & Flashing Port (WCH CH343P Bridge) ---
#define BOARD_UART0_TX_PIN       GPIO_NUM_43  // Console TX (Right Header Pin 2 / Right Type-C Receptacle)
#define BOARD_UART0_RX_PIN       GPIO_NUM_44  // Console RX (Right Header Pin 3 / Right Type-C Receptacle)

// --- Onboard Peripherals ---
#define BOARD_BOOT_BTN_PIN       GPIO_NUM_0   // Onboard BOOT Button (Right Header Pin 14)
#define BOARD_RGB_LED_PIN        GPIO_NUM_48  // WS2812B RGB LED (Right Header Pin 16 / RMT driver)

// --- Inter-Chip Dual-MCU Bus (Optional Upgrade Link) ---
#define BOARD_INTER_I2S_WS_PIN   GPIO_NUM_1   // Inter-Chip I2S Word Select RX (Right Header Pin 4)
#define BOARD_INTER_I2S_DIN_PIN  GPIO_NUM_2   // Inter-Chip I2S Data RX (Right Header Pin 5)
#define BOARD_INTER_I2S_BCK_PIN  GPIO_NUM_3   // Inter-Chip I2S Bit Clock RX (Left Header Pin 13)
#define BOARD_INTER_UART_RX_PIN  GPIO_NUM_11  // UART1 RX from Hub (Left Header Pin 17)
#define BOARD_INTER_UART_TX_PIN  GPIO_NUM_12  // UART1 TX to Hub (Left Header Pin 18)
```

---

## 9. Hardware Schematics & Engineering Verification Guide

Detailed circuit schematics, vector diagrams, comprehensive net interconnection matrices, CLC Pi-filter filter calculations, and pre-assembly commissioning sign-off checklists are maintained in the dedicated companion document:

👉 **[Hardware Schematics & Engineering Verification Guide](schematics.md)**

### Quick Summary of Available Schematics:
* **[Dedicated ESP32-S3 DSP/DAC Pipeline with Preset Buttons (`DSP+DAC.svg` / `ESP32-S3-DSP-DAC.svg`)](schematics.md#2-schematic-1-verification-dedicated-esp32-s3-dspdac-pipeline-with-preset-buttons-esp32-s3-dsp-dacsvg):** Verified Rev 1.3 standalone streaming schematic including YD-ESP32-S3 (`U1`), GY-PCM5102A (`U2`), TDA1308 (`U3`), 5V CLC Pi-filter network (`C1..C4, L1`), 3 tactile preset buttons (`SW1..SW3`) with 10k pull-ups (`R1..R3`), and 3.5mm stereo headphone output jack (`J1`).
* **[ESP32-S3 DSP/DAC with Screen & Rotary (No Local Player) (`DSP+DAC+SCREEN.svg`)](schematics.md#3-schematic-2-verification-dedicated-esp32-s3-dspdac-with-screen--rotary-encoder-no-local-player-dspdacscreensvg):** Verified Rev 1.1 desktop streamer schematic including YD-ESP32-S3 (`U7`), GY-PCM5102A (`U8`), TDA1308 (`U9`), 5V CLC Pi-filter network (`C9..C12, L3`), standard 4-pin I2C OLED display (`OLED2` @ `0x3C`), EC11 rotary encoder (`SW7`) with 10k pull-ups (`R9..R11`), dual tactile navigation buttons (`SW6` on GPIO 18 with pull-up `R8`, `SW8` on GPIO 10 with pull-up `R12`), and 3.5mm headphone jack (`J3`). *MicroSD socket omitted; GPIO 13, 14, 21 left free for user expansions!*
* **[Complete DSP/DAC with Local Player, Display & Controls (`DSP+DAC+LOCAL.svg` / `ESP32-S3-DSP-DAC with local player.svg`)](schematics.md#4-schematic-3-verification-complete-dspdac-with-local-player-display--controls-dspdaclocalsvg--esp32-s3-dsp-dac-with-local-playersvg):** Verified Rev 1.2 full DAP schematic with 1-bit SDMMC MicroSD (`SD1`), standard 4-pin I2C OLED display (`OLED1` @ standard address `0x3C`), EC11 rotary encoder (`SW5`) with discrete 10k pull-ups (`R5..R7`), tactile mode button (`SW4`) with pull-up `R4`, and 3.5mm stereo headphone output jack (`J2`).
* **[EasyEDA Master Archive & Layout Bitmap](schematics.md#1-overview-of-available-schematics):** Multi-sheet vector archive **[`Schematic_esp32-dsp_2026-09-19.zip`](../schematics/Schematic_esp32-dsp_2026-09-19.zip)** and high-resolution layout export **[`ESP32-S3-DSP-DAC.png`](../schematics/ESP32-S3-DSP-DAC.png)** for bench assembly.


