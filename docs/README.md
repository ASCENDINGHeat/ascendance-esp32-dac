# High-Fidelity ESP32-S3 Audio DSP & USB DAC: Documentation Portal

> **Authoritative Documentation Directory & Engineering Reference**  
> **Active Target Platform:** YD-ESP32-S3 (Xtensa Dual-Core LX7 @ 240 MHz, 16 MB Flash, 8 MB Octal PSRAM)  
> **Production Hardware Baseline:** Schematic 2 (`docs/schematics/DSP+DAC+SCREEN.svg` Rev 1.1)  
> **DAC & Headphone Output:** Texas Instruments GY-PCM5102A (32-bit / 384 kHz) + NXP/Philips TDA1308 Class-AB Amp  
> **Firmware Framework:** ESP-IDF v6.1 (C / FreeRTOS SMP / TinyUSB / esp-dsp)  
> **Control Interfaces:** EC11 Hardware Rotary Encoder + Dual Navigation Buttons (`SW6` & `SW8`) + Driverless WebHID Browser Tuner (Chrome / Edge)

---

## 1. Documentation Organization & Taxonomy

To ensure rapid navigation for embedded engineers, system architects, and autonomous AI agents, the documentation is organized into six structured tiers:

```
                                  DOCUMENTATION PORTAL
 ┌────────────────────────────────────────────────────────────────────────────────────────┐
 │ TIER 1: ACTIVE PRODUCTION FIRMWARE & HARDWARE SPECIFICATIONS                           │
 │ • Current Firmware Record  • Software Architecture  • Hardware & Wiring  • Schematics  │
 ├────────────────────────────────────────────────────────────────────────────────────────┤
 │ TIER 2: ACTIVE SUBSYSTEM DEEP-DIVE GUIDES                                              │
 │ • USB DAC & WebHID Stack   • Parametric EQ Engine   • UI & OLED Display  • Arbiter     │
 ├────────────────────────────────────────────────────────────────────────────────────────┤
 │ TIER 3: ENGINEERING TOOLS, DIAGNOSTICS & AI MANUALS                                    │
 │ • Master AI Reference      • Troubleshooting & RCA  • WebHID Live Tuner Client App     │
 ├────────────────────────────────────────────────────────────────────────────────────────┤
 │ TIER 4: FUTURE ENHANCEMENTS & ARCHITECTURAL ROADMAP                                    │
 │ • Matrix Mixer & Crossfeed • 24-bit/96kHz Pipeline  • OLED EQ Curves    • PWA Offline  │
 ├────────────────────────────────────────────────────────────────────────────────────────┤
 │ TIER 5: MODULAR PER-FUNCTION SPECIFICATIONS (docs/software_functions/)                 │
 │ • 01: Audio Pipeline / I2S • 03: Parametric EQ DSP  • 04: USB Audio / HID • 06: UI/Ctl │
 ├────────────────────────────────────────────────────────────────────────────────────────┤
 │ TIER 6: AUXILIARY & HISTORICAL ARCHITECTURAL RESEARCH                                  │
 │ • Dual-MCU Distributed Hub • MicroSD Local Player   • Wi-Fi & Networking • System Arch │
 └────────────────────────────────────────────────────────────────────────────────────────┘
```

---

## 2. Master Documentation Directory

### Tier 1: Core System & Active Firmware Implementation
*Definitive technical specifications for the code compiled, flashed, and operating on hardware today.*

| Document | Primary Focus | Key Subsystems & Topics Covered |
| :--- | :--- | :--- |
| **[Current Firmware Implementation](firmware/current_firmware_implementation.md)** | **Definitive Production Record** | Verified operational state: UAC 1.0 + WebHID composite device (`303A:4002`), 1-to-10 band PEQ, SSD1306 OLED, PCNT rotary encoder, I2S0 DMA pipeline, memory sizing, and flash commands. |
| **[Software Architecture](firmware/software.md)** | **Software Architecture & HAL** | Modular C subsystems, ESP-IDF v6.1 `driver/i2s_std.h`, FreeRTOS dual-core task layout (Core 0 control vs Core 1 audio), memory model, and developer invariants. |
| **[Hardware & Wiring Guide](hardware/hardware.md)** | **Electrical & Physical Assembly** | Complete Bill of Materials (BOM), **100% Through-Hole recommendations**, YD-ESP32-S3 physical header pinouts, 5V power routing (`IN-OUT` jumper), DAC solder jumpers, and star-grounding rules. |
| **[Hardware Schematics Guide](hardware/schematics.md)** | **Circuit Schematics & Verification** | Netlists, CLC Pi-filter calculations, pre-assembly sign-off checklists, and verified vector schematics: [`DSP+DAC+SCREEN.svg`](schematics/DSP+DAC+SCREEN.svg) (Rev 1.1, Production), [`DSP+DAC.svg`](schematics/DSP+DAC.svg) (Rev 1.3), and [`DSP+DAC+LOCAL.svg`](schematics/DSP+DAC+LOCAL.svg) (Rev 1.2). |

---

### Tier 2: Subsystem Deep-Dive Guides
*Comprehensive engineering guides detailing the active software and DSP algorithms.*

| Document | Primary Focus | Key Subsystems & Topics Covered |
| :--- | :--- | :--- |
| **[USB Audio Class (UAC) DAC Guide](guides/usb_dac_guide.md)** | **USB Audio & WebHID Stack** | Native USB OTG (GPIO 19/20), TinyUSB UAC 1.0 isochronous streaming (48 kHz / 16-bit stereo), host volume/mute synchronization, and bidirectional 64-byte raw WebHID protocol. |
| **[Parametric Equalizer (PEQ) DSP Guide](guides/parametric_eq_guide.md)** | **DSP Audio Engine** | Robert Bristow-Johnson (RBJ) Audio EQ Cookbook biquad mathematics, 1-to-10 dynamic band scaling, 15 ms click-free crossfade, pre-gain headroom, saturation clamping, and NVS flash persistence. |
| **[User Interface, Display & Controls](guides/ui_and_controls_guide.md)** | **Physical UI & Visualizer** | SSD1306 128x64 OLED graphics driver (I2C 400 kHz @ `0x3C`), 3 operating modes (Playback HUD, Main Menu, and On-Device Digit-by-Digit PEQ Editor), hardware PCNT rotary decoding (GPIO 15/16), WS2812B aRGB RMT loudness/mute LED, and debounced dual navigation buttons. |
| **[Audio Switching & Handover Guide](guides/mode_switching_guide.md)** | **Source Arbiter & Muting** | Central audio source arbiter, 4-phase zero-pop hardware muting protocol (`GPIO 7: XMT`), diagnostic tone arpeggio generator, and continuous frequency sweep. |

---

### Tier 3: Engineering Tools, Diagnostics & AI Reference
*Development manuals, automated diagnostic workflows, and browser tooling.*

| Document | Primary Focus | Key Subsystems & Topics Covered |
| :--- | :--- | :--- |
| **[Master Agentic AI Developer Reference](reference/agentic_ai_reference.md)** | **Autonomous AI Developer Manual** | The 10 Golden Invariants, master pinout definitions, FreeRTOS task affinities, memory caps, automated diagnostic triage tree, and coding rules for autonomous AI agents. |
| **[Troubleshooting & Root Cause Analysis](guides/troubleshooting.md)** | **Post-Mortems & Preventative Eng** | Deep-dive post-mortems: eliminating 1 kHz USB frame buzz, star-grounding architecture, subnormal float DSP slowdowns, Core 1 cache panics during NVS commits, and brownout resets. |
| **[WebHID Live Tuner Application](../tools/webhid_tuner/index.html)** | **Driverless Web Client Tool** | Browser-based interactive client: HTML5 Canvas logarithmic curve visualizer, draggable nodes, mouse-wheel Q factor adjustment, dynamic band scaling, and volatile SRAM auditioning. |

---

### Tier 4: Future Enhancements & Architectural Roadmap
*Implementation-ready specifications for future development cycles.*

| Document | Primary Focus | Key Subsystems & Topics Covered |
| :--- | :--- | :--- |
| **[Future Enhancements Roadmap](firmware/future_enhancements.md)** | **Feature Roadmap & Blueprints** | **Section 1.6 Channel Matrix Mixer & Headphone Crossfeed (Chu Moy/Bauer)**, 24-bit / 96 kHz high-res pipeline, bit-perfect dual-rate (44.1k/48k) dynamic reclocking, independent dual-mono L/R PEQ, OLED live curve rendering, stereo VU meter, rotary PEQ editor, AutoEQ import, and PWA offline deployment. |

---

### Tier 5: Modular Per-Function Specifications (`docs/software_functions/`)
*Exact C99 function signatures, FreeRTOS task models, stack budgets, and production code blocks.*

* **[Software Functions Overview & AI Manual](software_functions/README.md):** Function directory, master task matrix, and AI prompting schema.
* **[01. Audio Pipeline & Master I2S Driver](software_functions/01_audio_pipeline_i2s.md):** Standard Philips I2S0, DMA buffers, hardware soft mute, startup chime.
* **[03. Parametric Equalizer (PEQ) DSP Engine](software_functions/03_parametric_eq_dsp.md):** Biquad kernels, crossfader, saturation clamping, NVS presets.
* **[04. USB Audio Class 1.0 & WebHID Subsystem](software_functions/04_usb_audio_webhid.md):** TinyUSB composite stack, UAC 1.0 callbacks, 64-byte HID packets.
* **[06. UI Display & Hardware Controls](software_functions/06_ui_display_controls.md):** SSD1306 framebuffer renderer, PCNT rotary encoder, debounced buttons.
* **[07. Audio Source Arbiter & Zero-Pop Protocol](software_functions/07_audio_source_arbiter.md):** 4-Phase zero-pop muting, source state machine, tone/sweep generators.

---

### Tier 6: Auxiliary & Historical Architectural Research
*Architectural research, exploratory modules, and historical networking specifications.*

| Document | Primary Focus | Context & Relationship to Production |
| :--- | :--- | :--- |
| **[System Architecture & Overview](reference/architecture.md)** | **High-Level Design & Trade-offs** | System dataflow, DSP boundaries, dual-core workload distribution, and technical comparison (USB UAC vs Wi-Fi vs Bluetooth Classic). |
| **[Dual-MCU Distributed Architecture](research/dual_esp32_architecture.md)** | **Co-Processor Hub Research** | Architectural study on offloading Bluetooth Classic A2DP audio sink, Wi-Fi, and SDMMC storage to an auxiliary ESP32 co-processor while keeping ESP32-S3 dedicated to pure DSP/DAC. |
| **[MicroSD (SDMMC) Local Player Guide](research/sdmmc_player_guide.md)** | **Local Storage Player Design** | Hardware SDMMC Host (1-bit / 4-bit) configuration, FATFS integration, 128 KB PSRAM ringbuffer, and multi-format decoding (`dr_flac`, `dr_wav`, `minimp3`). |
| **[Wi-Fi & Networking Guide](research/wifi_and_networking.md)** | **Network Audio Architecture** | Wi-Fi 4/6 interoperability, disabling modem sleep (`WIFI_PS_NONE`), WPA2/WPA3 PMF auto-negotiation, active AP scanning, and HTTP audio streaming engine. |
| **[02. Wi-Fi & HTTP Streamer Specification](software_functions/02_wifi_and_http_streamer.md)** | **Network Function Reference** | Function-level implementation specifications for Wi-Fi station setup and HTTP stream chunking. |
| **[05. MicroSD Local Player Specification](software_functions/05_sdmmc_local_player.md)** | **Storage Function Reference** | Function-level implementation specifications for SDMMC host and FATFS file streaming. |
| **[08. Dual-MCU Bridge Protocol Specification](software_functions/08_dual_mcu_bridge.md)** | **Inter-Chip Bridge Reference** | High-speed UART and slave I2S bridge specifications for dual-MCU setups. |

---

## 3. Production Hardware Quick Reference (Schematic 2: `DSP+DAC+SCREEN.svg` Rev 1.1)

```
                           YD-ESP32-S3 (N16R8)
             ┌──────────────────────────────────────────────┐
             │ GPIO 4  ────────► PCM5102A LCK (I2S WS)      │
             │ GPIO 5  ────────► PCM5102A DIN (I2S DOUT)    │
             │ GPIO 6  ────────► PCM5102A BCK (I2S Clock)   │
             │ GPIO 7  ────────► PCM5102A XMT (Soft-Mute)   │
             │ GPIO 8  ────────► SSD1306 OLED SDA (I2C)     │
             │ GPIO 9  ────────► SSD1306 OLED SCL (I2C)     │
             │ GPIO 10 ────────► Nav Button 2 (SW8, Right)  │
             │ GPIO 15 ────────► EC11 Encoder Phase A (PCNT)│
             │ GPIO 16 ────────► EC11 Encoder Phase B (PCNT)│
             │ GPIO 17 ────────► EC11 Encoder Push Switch   │
             │ GPIO 18 ────────► Nav Button 1 (SW6, Left)   │
             │ GPIO 19 ────────► Native USB D-              │
             │ GPIO 20 ────────► Native USB D+              │
             │ 5Vin    ────────► PCM5102A & TDA1308 VCC     │
             │                   (Bridge IN-OUT Jumper!)    │
             └──────────────────────────────────────────────┘
```

### 3.1 Mandatory Hardware Solder Jumpers

1. **GY-PCM5102A DAC Module (Underside SMD Pads):**
   * **`SCK` $\rightarrow$ `L` (GND):** **Mandatory.** Enables internal PLL to synthesize MCLK from BCK. (Without this: complete silence).
   * **`FMT` $\rightarrow$ `L` (GND):** **Mandatory.** Selects standard Philips I2S format. (Bridging to H causes loud digital static).
   * **`FLT` $\rightarrow$ `L` (GND):** Normal latency FIR filter rolloff.
   * **`DMP` $\rightarrow$ `L` (GND):** De-emphasis filter off.
   * **`XMT` (Front Pin):** Connect to **GPIO 7**. **Never leave floating** (floating causes random muting).

2. **YD-ESP32-S3 Development Board (Underside Jumper):**
   * **Bridge `IN-OUT` SMD Jumper:** Connects USB-C VBUS to the `5V` header pin to power the DAC and headphone amplifier.

3. **Single-Point Star Grounding (Preventing 1 kHz USB Frame Whine):**
   * Connect digital grounds (ESP32, OLED, encoder) and analog grounds (PCM5102A AGND, TDA1308 GND) to a single common star point at the power entry.
   * **Golden Rule:** Connect the 3.5mm headphone jack sleeve ground **directly to the TDA1308 analog ground pad**, never to the ESP32 digital ground pin.
