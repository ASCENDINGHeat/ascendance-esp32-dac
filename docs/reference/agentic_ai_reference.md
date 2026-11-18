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

```text
┌─────────────────────────────────────────────────────────────────────────────┐
│                       THE TEN GOLDEN INVARIANTS                             │
├─────────────────────────────────────────────────────────────────────────────┤
│ 1. NO BLUETOOTH CLASSIC (A2DP) : ESP32-S3 silicon lacks BR/EDR.             │
│ 2. FORBIDDEN PINS 33–37        : Dedicated to Octal PSRAM. Never touch.     │
│ 3. ZERO HEAP ALLOC IN DSP LOOP : Use static/pre-allocated BSS or init DRAM. │
│ 4. CORE 1 AUDIO AFFINITY       : Core 0 = USB/UI/NVS; Core 1 = Real-Time DSP│
│ 5. 15 MS CLICK-FREE CROSSFADE  : Stage updates on Core 0, fade on Core 1.   │
│ 6. STRICT RECLOCKING PROTOCOL  : Mute LOW -> Disable -> Reconfig -> Enable. │
│ 7. ZERO-POP MUTING ORDER       : Fade -> Mute -> Stop -> Reclock -> Play.   │
│ 8. 24-BIT SATURATION CLAMPING  : Clamp [-8388608, 8388607] -> 32-bit MSB.   │
│ 9. VOLATILE LIVE VS NVS V3 SAVE: Tune RAM live; write NVS v3 only on save.  │
│ 10. DUAL-PORT & 5V DISCIPLINE  : COM = UART logs; USB = UAC; Bridge IN-OUT. │
└─────────────────────────────────────────────────────────────────────────────┘
```

### Detailed Explanations:
1. **Never Propose Bluetooth Classic (A2DP) Directly on the ESP32-S3:** The ESP32-S3 silicon lacks BR/EDR baseband hardware. If Bluetooth Classic A2DP is required, implement the **Dual-MCU Architecture** (see [`../research/dual_esp32_architecture.md`](../research/dual_esp32_architecture.md)). *(Auxiliary research modules like Wi-Fi/MP3 additionally require `32 KB` decoder stacks, `WIFI_PS_NONE`, and static `mp3dec_scratch_t` in BSS.)*
2. **Never Touch GPIO 33, 34, 35, 36, 37:** These are routed internally to the Octal PSRAM chip. Calling `gpio_config()` or assigning any peripheral to them immediately crashes the memory cache controller.
3. **Zero Dynamic Allocation in Real-Time Audio Loops:** Never call `malloc()`, `calloc()`, or `realloc()` inside `audio_dsp_task` or `peq_dsp_process_interleaved_s24_to_s32()`. Pre-allocate biquad, FIR (`16`-byte aligned DRAM), and lookahead limiter buffers in static BSS or initialization paths.
4. **Core Affinity Discipline:** Core 0 handles TinyUSB (`tinyusb_task`), input controls (`controls_task`, `ctrl_disp`), I2C OLED (`oled_ui_task`), WS2812B LED, and NVS flash commits. Core 1 (`audio_dsp_task`, Priority `10`, `8,192 B` stack) is reserved strictly for audio DSP and I2S DMA.
5. **Click-Free Crossfading & Subnormal Flushing:** Never swap biquad, A/B, Dual-Mono, loudness, FIR, or matrix states instantaneously; stage under `portMUX_TYPE` on Core 0 and interpolate over `15 ms` on Core 1, flushing delay states `< 1e-15f` to zero.
6. **I2S Reclocking Order:** Never call `i2s_channel_reconfig_std_clock()` while the channel is active. Mute `GPIO 7` LOW, disable the channel (`i2s_channel_disable()`), update the clock (`44.1 / 48.0 / 96.0 kHz`), and re-enable.
7. **Zero-Pop Audio Handover:** Never reclock or switch sources while unmuted. Always execute: *Digital Fade-Out $\rightarrow$ Assert `XMT` (`GPIO 7`) LOW $\rightarrow$ Stop Source $\rightarrow$ Reclock $\rightarrow$ Start Source $\rightarrow$ Assert `XMT` HIGH $\rightarrow$ Fade-In*.
8. **Hard 24-Bit Saturation Clamping & Limiter Safety:** Bound float samples behind the optional `-0.5 dBFS` (`1.5 ms` lookahead) True-Peak Limiter and clamp strictly to signed 24-bit full-scale `[-8388608.0f, 8388607.0f]` before MSB-aligning into 32-bit I2S slots (`int32_t`).
9. **Volatile Live Tuning vs. Explicit NVS v3 Flash Commits:** Real-time WebHID / Android Companion tuning modifies RAM only. Persistent commits to `nvs_meta_v3`, `peq_slot_0..7` (`Slot 1`–`Slot 8`), and `fir_taps_l`/`r` occur only on explicit save commands (`0x05`, `0x0A`, `0x0D`, `0x14` with commit flag, or long-press Save) with CRC32 + `memcmp` delta verification.
10. **Dual-Port Topology & Power Rail Discipline:** Maintain UART logging on `GPIO 43/44` (CH343P COM port) while using `GPIO 19/20` for Native USB OTG. Bridge the `IN-OUT` solder jumper on the PCB to supply 5V to `5Vin` for the DAC and amplifier; never feed external 5V to `5Vin` while USB is connected.

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
#define PIN_BTN_PRESET_1        GPIO_NUM_15  // SW1 / SW3: Preset 1
#define PIN_BTN_PRESET_2        GPIO_NUM_16  // SW2 / SW5: Preset 2
#define PIN_BTN_PRESET_3        GPIO_NUM_17  // SW3 / SW4: Preset 3

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
| :--- | :---: | :---: | :---: | :--- |
| **`audio_dsp_task`** | **Core 1** | **8,192 B** | **10** | Real-time 24-bit UAC DSP (10-band EQ, FIR, Loudness, Matrix, Limiter) & I2S0 DMA |
| **`tinyusb_task`** | **Core 0** | **4,096 B** | **5** | TinyUSB UAC 1.0 tri-rate audio + 64-byte raw HID event dispatcher |
| **`ctrl_disp`** | **Core 0** | **4,096 B** | **4** | Button/encoder event handler, NVS v3 save engine, and OLED menu state machine |
| **`controls_task`** | **Core 0** | **3,072 B** | **3** | 50 Hz hardware PCNT rotary encoder delta poll & button click/double/long debounce |
| **`oled_ui_task`** | **Core 0** | **4,096 B** | **2** | 10 FPS SSD1306 I2C framebuffer (`Classic`/`Curve`/`VU`/`Split`), hot-plug probe, dimmer |
| *`audio_stream_task` (Aux)* | Core 1 | 32,768 B | 5 | Research Wi-Fi/MP3 decoder pipeline (not compiled in production USB DAC `main/`) |
| *`sd_reader_task` (Aux)* | Core 0 | 8,192 B | 8 | Research SDMMC FATFS reader (`128 KB` PSRAM ringbuffer) |

---

## 5. Automated Triage & Diagnostic Decision Tree

When analyzing compilation errors, CI failures, or serial monitor panic logs, consult this quick reference:

| Diagnostic Pattern | Root Cause | Automated Resolution |
| :--- | :--- | :--- |
| `-Werror=address-of-packed-member` | Taking pointer to field inside `__attribute__((packed))` HID struct | Copy scalar/array to aligned stack variable before passing pointer (`memcpy`). |
| `Cache error: Cache access while memory is disabled` | Peripheral assigned to GPIO 33–37 | Reassign peripheral to safe GPIO pins (1–21, 38–48). |
| `Core 1 panic'ed (Cache access while memory is disabled)` | Real-time audio task executing from Flash during `nvs_commit()` | Keep Core 1 audio & I2S functions IRAM-safe, state in DRAM, and DMA buffer $\ge 42\text{ ms}$. |
| `Task watchdog got triggered on Core 1` during silence | Subnormal / Denormal floating-point operations in IIR biquad | Flush delay states $< 10^{-15}\text{f}$ to zero in biquad loop. |
| Loud white noise / `NaN` in biquad coefficients | Extreme $Q$ ($>10$) or low frequency ($<20\text{ Hz}$) instability | Clamp $Q \le 10.0$, $f_0 \ge 20\text{ Hz}$, check `isfinite()` / non-zero `a0` before applying. |
| WebHID `SecurityError: Failed to open the device` on Windows | Windows exclusive lock on Usage Page `0x01` or `0x0C` | Use Vendor-Defined Usage Page (`0xFF60`, Usage `0x61`) in TinyUSB HID report descriptor. |
| `assert failed: xQueueGenericSend` / ISR watchdog panic | Blocking call or non-ISR API executed in TinyUSB callback | Use lock-free ringbuffers / spinlock flags; defer DSP recalculation and NVS commits to task context. |
| Android app interrupts Spotify/Tidal audio when connecting | Claiming UAC Audio Interfaces `#0`/`#1` | Claim **only USB Control/HID Interface `#2`** via `UsbManager.claimInterface()`, leaving audio `#0`/`#1` with Android OS. |
| `i2s_common: i2s_channel_reconfig_std_clock failed` | Reclocking active I2S channel | Call `i2s_channel_disable()` before reconfiguring clock (after `GPIO 7 LOW` mute). |
| Audio silence / DAC unpowered / 5V pin reads 3.3V | `IN-OUT` solder jumper is open (Issue #3) | Solder-bridge the `IN-OUT` jumper pads to bypass diode D3; verify 5V with multimeter. |
| Audio silence during playback (5V rail verified) | PCM5102A mode pins floating | Ground `SCK` (enables PLL), pull `XMT` to `GPIO 7` (HIGH to play), ground `FMT`. |
| OLED display blank / I2C NACK | I2C address mismatch (`0x3C` vs `0x3D`) or hot-plug disconnect | Firmware auto-probes `0x3C` / `0x3D` at boot and runs a non-blocking `2 s` reconnect probe. |
| Rollup `Code is nested too deeply` or `EACCES` on Android | `/storage/emulated/0` mounted `noexec` or ARM64 Rollup AST stack check | Run WebHID Vitest & `esbuild` single-file build (`scripts/build-singlefile.mjs`) in executable `$HOME/.cache` if on Android; on Linux/CI run `npm ci && npm test && npm run build`. |
| PEQ slider dragging causes clicks or flash wear | Committing to NVS on every slider move / no crossfade | Live commands (`0x02`/`0x03`) update volatile RAM with `15 ms` crossfade; flash writes use explicit save (`0x05`) + CRC32 + `memcmp` delta check. |

---

## 6. Workspace Agent Customizations & Documentation Navigation

* **Always-On Repository Rule:** [`AGENTS.md`](../../AGENTS.md) (root repository rule automatically discovered by Antigravity and compatible coding agents).
* **On-Demand Workspace Skills:**
  * [`ascendance-hid-parity`](../../.agents/skills/ascendance-hid-parity/SKILL.md): Cross-layer synchronization across firmware C, React WebHID (`tools/webhid_tuner/src/`), Android Kotlin (`tools/android_companion/`), and docs.
  * [`ascendance-verify-build`](../../.agents/skills/ascendance-verify-build/SKILL.md): Verification and CI workflows for ESP-IDF v6.1, Vitest + `esbuild` single-file bundle, and Android Gradle builds.
* **Core Guides:**
  * **[Architecture & Overview](architecture.md)** · **[Hardware & Wiring](../hardware/hardware.md)** · **[Software Architecture](../firmware/software.md)** · **[Current Firmware Record](../firmware/current_firmware_implementation.md)** · **[Future Enhancements](../firmware/future_enhancements.md)**
  * **[USB DAC Guide](../guides/usb_dac_guide.md)** · **[Parametric EQ Guide](../guides/parametric_eq_guide.md)** · **[UI & Controls Guide](../guides/ui_and_controls_guide.md)** · **[Mode Switching Guide](../guides/mode_switching_guide.md)** · **[Troubleshooting & RCA](../guides/troubleshooting.md)**
  * **[Per-Function Software Specifications](../software_functions/README.md)** · **[Wi-Fi Research](../research/wifi_and_networking.md)** · **[SDMMC Player Research](../research/sdmmc_player_guide.md)** · **[Dual-MCU Research](../research/dual_esp32_architecture.md)**
