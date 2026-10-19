# Master Agentic AI Developer Reference & Repository Invariants

This document serves as the **centralized instruction manual, architectural specification, and invariant rulebook** for autonomous AI coding agents (such as Antigravity, Claude Code, GitHub Copilot, Codex) and human developers extending this codebase.

---

## 1. System Identity & Hardware Environment

* **Target MCU:** Espressif **ESP32-S3** (Board: **YD-ESP32-S3 / YD-ESP32-23**, VCC-GND Studio DevKitC-1 clone; Reference: [rtek1000/YD-ESP32-23](https://github.com/rtek1000/YD-ESP32-23), Schematic `YD-ESP32-S3-SCH-V1.4`).
* **Memory Topology:**
  * **16 MB Quad SPI Flash** (`CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y`).
  * **8 MB Octal PSRAM** (`CONFIG_SPIRAM_MODE_OCT=y`).
* **Toolchain:** **ESP-IDF v6.1** using CMake and Ninja.
* **Dual USB Type-C Architecture:**
  * **COM Port (Right Type-C Receptacle):** High-speed USB-to-UART bridge via **WCH CH343P** on `GPIO 43 (TXD0)` and `GPIO 44 (RXD0)` (`/dev/ttyACM0` or `/dev/ttyUSB0`) for monitor logging and flashing.
  * **USB Port (Left Type-C Receptacle):** Direct internal PHY on `GPIO 19 (D-)` and `GPIO 20 (D+)` for **USB Audio Class 1.0 (UAC DAC)**.
* **Onboard Solder Jumpers (PCB Back & Front):**
  * `IN-OUT` Jumper: Bypasses diode D3 to feed USB VBUS power directly out to header pin `5Vin` (Left Header Pin 21). **Must be bridged** to power external audio modules (PCM5102A & TDA1308) via USB.
  * `USB-OTG` Jumper: Parallels VBUS rails of both USB-C ports.
  * `RGB` Jumper: Gates `GPIO 48` signal to onboard WS2812B addressable RGB LED.

---

## 2. The Ten Golden Invariants (DO NOT BREAK)

Any autonomous agent generating or refactoring code in this repository MUST verify compliance with these 10 invariants:

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                       THE TEN GOLDEN INVARIANTS                             │
├─────────────────────────────────────────────────────────────────────────────┤
│ 1. NO BLUETOOTH CLASSIC (A2DP) : ESP32-S3 silicon lacks BR/EDR.             │
│ 2. FORBIDDEN PINS 33–37        : Dedicated to Octal PSRAM. Never touch.     │
│ 3. NO STACK SCRATCHPADS        : mp3dec_scratch_t MUST be static in BSS.    │
│ 4. AUDIO TASK STACK >= 32 KB   : Required for decoder + LwIP + HTTP client. │
│ 5. DISABLE MODEM SLEEP         : esp_wifi_set_ps(WIFI_PS_NONE) is mandatory.│
│ 6. STRICT RECLOCKING PROTOCOL  : Disable channel -> reconfig -> enable.     │
│ 7. ZERO-POP MUTING ORDER       : Fade -> Mute -> Stop -> Reclock -> Play.   │
│ 8. FLOAT SATURATION CLAMPING   : Never raw cast float to int16_t.           │
│ 9. DUAL-CORE AFFINITY          : Core 0 = Network/UI; Core 1 = Realtime DSP.│
│ 10. DUAL-PORT & 5V DISCIPLINE  : COM = UART logs; USB = UAC; Bridge IN-OUT. │
└─────────────────────────────────────────────────────────────────────────────┘
```

### Detailed Explanations:
1. **Never Propose Bluetooth Classic (A2DP) Directly on the ESP32-S3:** The ESP32-S3 silicon lacks BR/EDR baseband hardware. If Bluetooth Classic A2DP is required, implement the **Dual-MCU Architecture** (see `docs/dual_esp32_architecture.md`): offload Bluetooth A2DP, Wi-Fi, and SD storage to a second ESP32 Classic co-processor, routing decoded 16-bit PCM across an inter-chip I2S bus to the ESP32-S3.
2. **Never Touch GPIO 33, 34, 35, 36, 37:** These are routed internally to the Octal PSRAM chip. Calling `gpio_config()` or assigning any peripheral to them immediately crashes the memory cache controller.
3. **Never Allocate Decoder Scratchpads on the Stack:** `mp3dec_scratch_t` is ~16 KB. Local declaration silently overflows FreeRTOS stacks and smashes Wi-Fi heap memory, triggering `LoadProhibited in ieee80211_search_node`.
4. **Maintain 32 KB Minimum Audio Stack:** Any task executing decoding and network fetches requires `32768` bytes of stack.
5. **Always Set `WIFI_PS_NONE`:** Default modem sleep (`WIFI_PS_MIN_MODEM`) causes router beacon timeouts, resulting in disconnect loops.
6. **I2S Reclocking Order:** Never call `i2s_channel_reconfig_std_clock()` while the channel is active. You must disable the channel first, update the clock, and re-enable it.
7. **Zero-Pop Audio Handover:** Never reclock or switch sources while unmuted. Always execute: *Digital Fade-Out $\rightarrow$ Assert `XMT` LOW $\rightarrow$ Stop Source $\rightarrow$ Reclock $\rightarrow$ Start Source $\rightarrow$ Assert `XMT` HIGH $\rightarrow$ Fade-In*.
8. **Hard DSP Saturation Clamping:** Converting float samples directly with `(int16_t)v` causes catastrophic integer sign wrap at $\pm 32768$. Always clamp against `[-32768, 32767]`.
9. **Core Affinity Discipline:** Core 0 runs network, storage reading, and UI. Core 1 is reserved for time-critical DSP, decoding, and I2S DMA.
10. **Dual-Port Topology & Power Rail Discipline:** Maintain UART logging on `GPIO 43/44` (CH343P COM port) while using `GPIO 19/20` for Native USB OTG. When powering via USB-C, bridge the `IN-OUT` solder jumper on the PCB to provide 5V power to the `5Vin` pin for the DAC and amplifier; never feed external 5V to `5Vin` while USB is connected.

---

## 3. Master Hardware Pinout Reference

When generating board header files (`board_pins.h`), use this exact canonical mapping matching the verified YD-ESP32-S3 PCB:

```c
#pragma once
#include "driver/gpio.h"

// =============================================================================
// YD-ESP32-S3 (YD-ESP32-23) Master Pinout Reference
// Ref: https://github.com/rtek1000/YD-ESP32-23 (Schematic V1.4)
// =============================================================================

// --- I2S Audio DAC (Texas Instruments PCM5102A) ---
#define PIN_I2S_WS              GPIO_NUM_4   // Word Select / LCK (Left Header Pin 4)
#define PIN_I2S_DO              GPIO_NUM_5   // Serial Data / DIN (Left Header Pin 5)
#define PIN_I2S_BCK             GPIO_NUM_6   // Continuous Bit Clock / BCK (Left Header Pin 6)
#define PIN_DAC_XMT             GPIO_NUM_7   // Soft Unmute (Left Header Pin 7, HIGH = Play, LOW = Mute)

// --- I2C Display (SSD1306 / SH1106 128x64 OLED) ---
#define PIN_I2C_SDA             GPIO_NUM_8   // I2C Data Line (Left Header Pin 12)
#define PIN_I2C_SCL             GPIO_NUM_9   // I2C Clock Line (Left Header Pin 15)
// NOTE: Standard 4-pin I2C OLED modules (OLED1) ground SA0 internally, setting address to 0x3C.
// If using legacy 7-pin SPI OLED modules wired with DC tied to 3.3V, SA0 is HIGH (0x3D).
#define I2C_OLED_ADDR_DEFAULT   0x3C         // Standard 4-pin I2C OLED (OLED1 in Rev 1.2)
#define I2C_OLED_ADDR_SA0_LOW   0x3C         // SA0 grounded
#define I2C_OLED_ADDR_SA0_HIGH  0x3D         // SA0 tied to 3.3V (7-pin SPI modules)
#define I2C_OLED_ADDR_ALT       0x3D         // Alternative fallback address

// --- Rotary Encoder (EC11) & Tactile Buttons (Schematics Rev 1.3 / Rev 1.1 / Rev 1.2) ---
// Note: Hardware 10 kΩ pull-ups to 3.3V (R9..R11, R8, R12 in Rev 1.1; R5..R7, R4 in Rev 1.2)
#define PIN_ENC_A               GPIO_NUM_15  // Phase A (Left Header Pin 8, PCNT filtered, 10k pull-up)
#define PIN_ENC_B               GPIO_NUM_16  // Phase B (Left Header Pin 9, PCNT filtered, 10k pull-up)
#define PIN_ENC_SW              GPIO_NUM_17  // Momentary Push Switch (Left Header Pin 10, Active-Low, 10k pull-up)
#define PIN_BTN_MODE            GPIO_NUM_18  // Dedicated Mode Button (Left Header Pin 11, Active-Low, 10k pull-up)
#define PIN_BTN_1               GPIO_NUM_18  // Navigation Button 1 / Left (◄ / Menu / Back, 10k pull-up R8)
#define PIN_BTN_2               GPIO_NUM_10  // Navigation Button 2 / Right (► / Next / Preset, 10k pull-up R12)
// Aliases for 3-button preset selection in standalone DSP/DAC builds without encoder:
#define PIN_BTN_PRESET_1        GPIO_NUM_15  // SW1 / SW3: Preset 1 (e.g., Reference Flat / Web Stream 1)
#define PIN_BTN_PRESET_2        GPIO_NUM_16  // SW2 / SW5: Preset 2 (e.g., Bass Boost / Web Stream 2)
#define PIN_BTN_PRESET_3        GPIO_NUM_17  // SW3 / SW4: Preset 3 (e.g., AutoEQ Harman / Web Stream 3)

// --- Native USB OTG (USB DAC Audio Class 1.0) ---
#define PIN_USB_DM              GPIO_NUM_19  // USB D- (Right Header Pin 20 / Left Type-C Receptacle)
#define PIN_USB_DP              GPIO_NUM_20  // USB D+ (Right Header Pin 19 / Left Type-C Receptacle)

// --- MicroSD Storage (SDMMC Host Slot 1 - Omitted in DSP+DAC+SCREEN.svg, freeing GPIO 13, 14, 21) ---
#define PIN_SDMMC_CLK           GPIO_NUM_14  // SD Bus Clock (Left Header Pin 20)
#define PIN_SDMMC_CMD           GPIO_NUM_13  // SD Command / Response (Left Header Pin 19, 10k pull-up)
#define PIN_SDMMC_D0            GPIO_NUM_21  // SD Data 0 (Right Header Pin 18, 10k pull-up)
#define PIN_SDMMC_D1            GPIO_NUM_47  // SD Data 1 (Right Header Pin 17, 4-bit mode only)
#define PIN_SDMMC_D2            GPIO_NUM_38  // SD Data 2 (Right Header Pin 10, 4-bit mode only)
#define PIN_SDMMC_D3            GPIO_NUM_39  // SD Data 3 (Right Header Pin 9, 4-bit mode only)
#define PIN_SDMMC_CD            GPIO_NUM_40  // Card Detect Switch (Right Header Pin 8, Active-Low)

// --- UART Debug & Console (WCH CH343P Bridge) ---
#define PIN_UART0_TX            GPIO_NUM_43  // Console TX (Right Header Pin 2 / Right Type-C Receptacle)
#define PIN_UART0_RX            GPIO_NUM_44  // Console RX (Right Header Pin 3 / Right Type-C Receptacle)

// --- Onboard Peripherals ---
#define PIN_BOOT_BTN            GPIO_NUM_0   // Onboard Boot Button (Right Header Pin 14)
#define PIN_RGB_LED             GPIO_NUM_48  // WS2812B Addressable RGB LED (Right Header Pin 16 / RMT)

// --- Inter-Chip Dual-MCU Bus (Optional Upgrade Link) ---
#define PIN_INTER_I2S_WS        GPIO_NUM_1   // Inter-Chip I2S Word Select RX (Right Header Pin 4)
#define PIN_INTER_I2S_DIN       GPIO_NUM_2   // Inter-Chip I2S Data RX (Right Header Pin 5)
#define PIN_INTER_I2S_BCK       GPIO_NUM_3   // Inter-Chip I2S Bit Clock RX (Left Header Pin 13)
#define PIN_INTER_UART_RX       GPIO_NUM_11  // Inter-Chip UART1 RX from Hub (Left Header Pin 17)
#define PIN_INTER_UART_TX       GPIO_NUM_12  // Inter-Chip UART1 TX to Hub (Left Header Pin 18)
```

---

## 4. Multi-Core Task & Memory Matrix

| Task Name | Core Affinity | Stack Size | Priority | Responsibility |
| :--- | :--- | :--- | :--- | :--- |
| `wifi` / `sys_evt` | **Core 0** | System Default | 23 | 802.11 MAC management |
| `tcpip_thread` | **Core 0** | System Default | 18 | LwIP TCP/IP socket layer |
| `http_server` / `ws_eq` | **Core 0** | 8,192 bytes | 4 | Web UI, REST API & WebSocket PEQ live tuning |
| `sd_reader_task` | **Core 0** | 4,096 bytes | 6 | Reads 16 KB FATFS chunks from MicroSD |
| `mode_manager` | **Core 0** | 4,096 bytes | 5 | State machine & zero-pop audio arbiter |
| `ui_task` | **Core 0** | 4,096 bytes | 3 | LVGL / U8g2 display rendering |
| `rotary_task` | **Core 0** | 3,072 bytes | 4 | PCNT delta polling & button debouncing |
| `audio_stream_task`| **Core 1** | **32,768 bytes**| 10 | Real-time decoding, PEQ DSP, I2S DMA feeding |

---

## 5. Automated Triage & Diagnostic Decision Tree

When analyzing compilation errors or serial monitor panic logs, consult this quick reference:

| Diagnostic Pattern | Root Cause | Automated Resolution |
| :--- | :--- | :--- |
| `wifi:state: init -> auth -> init` (Reason 202) | Mobile hotspot 802.11ax rejection | Advise disabling "Wi-Fi 6 standard" or enabling "Maximize Compatibility"; set `WIFI_PROTOCOL_11B \| 11G \| 11N`. |
| `***ERROR*** A stack overflow in task audio_stream` | Local structures on task stack | Increase task stack to `32768`; convert local structs to `static` in BSS. |
| `LoadProhibited in ieee80211_search_node` | Scratchpad stack smashing | Ensure `mp3dec_scratch_t` is `static` in BSS. |
| `Cache error: Cache access while memory is disabled` | Peripheral assigned to GPIO 33–37 | Reassign peripheral to safe GPIO pins (1–21, 38–48). |
| `Core 1 panic'ed (Cache access while memory is disabled)` | Realtime audio task executing from Flash during `nvs_commit()` | Add `IRAM_ATTR` to Core 1 audio & I2S functions; place biquad state in `DRAM_ATTR`; increase I2S DMA buffer to $\ge 64\text{ ms}$. |
| `Task watchdog got triggered on Core 1` during silence | Subnormal / Denormal floating-point operations in IIR biquad | Add anti-denormal DC bias ($10^{-25}\text{f}$) or flush states $<10^{-15}\text{f}$ to zero in biquad loop. |
| Loud white noise / `NaN` in biquad coefficients | Extreme Q ($>10$) or low frequency ($<20\text{ Hz}$) instability | Clamp $Q \le 10$, clamp $f_0 \ge 20\text{ Hz}$, add `isnan()` sanity check before applying filter coefficients. |
| Periodic click/tick every 30–90s in USB DAC mode | Asynchronous clock drift between USB host SOF and I2S crystal | Implement UAC Isochronous Feedback endpoint (`0x81 IN`) or zero-crossing adaptive ringbuffer watermarking. |
| WebHID `SecurityError: Failed to open the device` on Windows | Windows exclusive lock on Usage Page `0x01` or `0x0C` | Use Vendor-Defined Usage Page (`0xFF60`, Usage `0x61`) in TinyUSB HID report descriptor. |
| `assert failed: xQueueGenericSend` / ISR watchdog panic | Blocking call or non-ISR API executed in TinyUSB callback | Use `xQueueSendFromISR` only; defer biquad recalculation and NVS commits to Core 0 task context. |
| `Brownout detector was triggered` | High current transient during Wi-Fi burst on weak USB power | Add $100\ \mu\text{F}$ low-ESR capacitor across $5\text{V}_{in}$ and GND; shut down Wi-Fi radio when in USB DAC mode. |
| Audio stutter during high-res FLAC playback from MicroSD | SD card internal wear-leveling stall (50–120 ms) | Allocate 128 KB PSRAM ringbuffer for `sd_reader_task`; read in 4 KB multi-block clusters. |
| `i2s_common: i2s_channel_reconfig_std_clock failed` | Reclocking active I2S channel | Call `i2s_channel_disable()` before reconfiguring clock. |
| Audio silence / DAC unpowered / 5V pin reads 3.3V | `IN-OUT` solder jumper is open (Issue #3) | Solder-bridge the `IN-OUT` jumper pads to bypass diode D3; verify 5V with multimeter. |
| Audio silence during playback (5V rail verified) | PCM5102A mode pins floating | Ground `SCK` (enables PLL), pull `XMT` to 3.3V (or `GPIO 7 HIGH`), ground `FMT`. |
| OLED display blank / I2C NACK | I2C address mismatch between `0x3C` (standard 4-pin module `OLED1`) and `0x3D` (legacy 7-pin module with DC to 3V3) | Run I2C bus scanner. Default to `0x3C` for standard 4-pin module `OLED1`. If using legacy 7-pin SPI module with DC tied to 3.3V, configure address to `0x3D`. |
| PEQ slider dragging causes clicks or flash wear | Committing to NVS on every slider move / no crossfade | Ensure live tuning updates volatile SRAM only (auditioning mode); use 15ms double-buffered crossfading and an explicit "Save to Preset" command with Delta check (memcmp) to eliminate unneeded flash wear. |

---

## 6. Subsystem Documentation Navigation

For deep-dive technical implementations, consult the modular subsystem guides:

* **[Architecture & Overview](architecture.md):** System pipeline, FreeRTOS topology, and Wi-Fi vs A2DP design decisions.
* **[Hardware & Wiring Guide](../hardware/hardware.md):** BOM, **100% Through-Hole (No-SMD) parts recommendation**, full pinout table, YD-ESP32-S3 physical header pinout, 5V power routing (`IN-OUT` solder jumper), GY-PCM5102A jumper configuration, and TDA1308 amp specs.
* **[Software Architecture](../firmware/software.md):** Production subsystem architecture, ESP-IDF v6.1 `driver/i2s_std.h`, UAC 1.0 + WebHID composite stack, PEQ biquads, FreeRTOS dual-core task layout, and developer invariants.
* **[Current Firmware Implementation](../firmware/current_firmware_implementation.md):** Definitive technical record of compiled and verified firmware: UAC 1.0 + WebHID composite device, 1-to-10 band PEQ, SSD1306 OLED, PCNT rotary encoder, and I2S pipeline.
* **[Future Enhancements Roadmap](../firmware/future_enhancements.md):** Architectural roadmap: Channel Matrix Mixer & Headphone Crossfeed, 24-bit/96kHz pipeline, dual-rate reclocking, OLED response curve visualizer, and PWA offline tooling.
* **[Wi-Fi & Networking Guide](../research/wifi_and_networking.md):** Wi-Fi 4/6 interoperability, PMF auto-negotiation, and `WIFI_PS_NONE`.
* **[Troubleshooting & RCA](../guides/troubleshooting.md):** Post-mortems for Reason 202, stack overflows, and Core 1 panics.
* **[USB Audio Class (UAC) DAC Guide](../guides/usb_dac_guide.md):** TinyUSB descriptors, ringbuffer sync, dual-port topology, and FiiO JA11 / Moondrop style live PEQ tuning via USB Composite (UAC + WebHID).
* **[Parametric Equalizer (PEQ) DSP Engine Guide](../guides/parametric_eq_guide.md):** RBJ Biquad equations, `esp-dsp` PIE SIMD kernels, AutoEQ JSON schemas, persistent NVS storage for runtime user PEQ (zero reflash needed, independent of factory presets), and real-time live tuning via USB WebHID (`0x4551`), WebSocket (`ws://<ip>/ws/eq`), and HTTP REST (`POST /api/eq/live`).
* **[MicroSD (SDMMC) Local Player Guide](../research/sdmmc_player_guide.md):** 1-bit / 4-bit SDMMC host, FATFS mounting, and 128KB PSRAM buffering.
* **[User Interface, Display & Controls Guide](../guides/ui_and_controls_guide.md):** I2C OLED (SSD1306/SH1106 at standard address `0x3C`), PCNT rotary encoder, and UI state machine.
* **[Multi-Mode Audio Switching & Handover Guide](../guides/mode_switching_guide.md):** `AudioSourceManager` arbiter, 4-phase zero-pop protocol, and RF power gating.
* **[Dual-MCU Distributed Architecture Guide](../research/dual_esp32_architecture.md):** Connectivity hub offloading (Bluetooth Classic A2DP Sink, Wi-Fi, SDMMC, UI) to ESP32 Classic co-processor, inter-chip I2S/UART protocol, and total RF isolation.
* **[Per-Function Software Specifications (Agentic AI Reference)](../software_functions/README.md):** Comprehensive function-by-function implementation reference for autonomous coding agents with exact prototypes, FreeRTOS core affinities, stack requirements, memory boundaries, and production reference implementations.

