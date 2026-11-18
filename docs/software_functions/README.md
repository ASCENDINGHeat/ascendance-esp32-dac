# Software Function Specifications & Agentic AI Orchestration Manual

> **Definitive Implementation Reference for Autonomous AI Coding Agents & Embedded Engineers**  
> **Target Platform:** ESP32-S3 (Xtensa Dual-Core LX7 @ 240 MHz, 16 MB Flash, 8 MB Octal PSRAM)  
> **Target Framework:** ESP-IDF v6.1 (`driver/i2s_std.h`, FreeRTOS SMP, TinyUSB, esp-dsp)  
> **Production Baseline:** Schematic 2 (`docs/schematics/DSP+DAC+SCREEN.svg`)  
> **Primary Technical Record:** [`docs/current_firmware_implementation.md`](../firmware/current_firmware_implementation.md)

---

## 1. Executive Summary & Orchestration Purpose

This directory (`docs/software_functions/`) provides the **comprehensive, function-by-function technical specification** for the ESP32-S3 High-Fidelity USB DAC & Real-Time DSP Audio System. It documents every currently compiled C function in [`main/`](../../main) alongside architectural specifications for exploratory and future subsystems.

### Why This Directory Exists for Agentic AI
When an autonomous AI coding agent (or human systems engineer) implements new features, refactors pipelines, or diagnoses anomalies, high-level summaries often omit critical micro-architectural constraints. This reference enforces:
1. **Exact C99 Function Signatures:** No guessing parameter types, const qualifiers, or pointer directions.
2. **Explicit Concurrency & Task Affinity:** FreeRTOS core assignment (Core 0 vs Core 1), priority levels, and stack budgets.
3. **Memory Ownership & Heap Caps:** Strict boundaries between internal DRAM (`MALLOC_CAP_INTERNAL`) and 8MB Octal PSRAM (`MALLOC_CAP_SPIRAM`).
4. **Error Handling & Invariants:** Specific `esp_err_t` return codes, recovery trees, and pre/post-conditions.
5. **Drop-In Production Code:** Complete, tested C reference implementations ready for modularization into `.c` and `.h` compilation units.

---

## 2. Master System Task & Core Affinity Matrix

To guarantee glitch-free audio playback at low latency while serving USB endpoints, hardware controls, and display refresh, all FreeRTOS tasks adhere to the following execution topology:

```
                  ESP32-S3 DUAL-CORE FREERTOS ARCHITECTURE
       Core 0: System, Control & Visual Subsystems   Core 1: Hard Real-Time Audio DSP & I2S
     ┌───────────────────────────────────────────┐  ┌──────────────────────────────────────┐
     │ • TinyUSB Device Stack Task (Prio 5)      │  │ • High-Priority Audio DSP (Prio 10)  │
     │ • Control Event Dispatcher (Prio 4)       │  │ • Safe Zero-Pop Sample Rate Reclock  │
     │ • Hardware PCNT Poller & Debounce (Prio 3)│  │ • Dual-Mono PEQ + FIR + Loudness     │
     │ • SSD1306 OLED Render (0x3C/0x3D, Prio 2) │  │ • Matrix/Crossfeed + True-Peak Lim.  │
     │ • NVS v3 Persistence (8 Slots + FIR)      │  │ • 24-bit Clamping -> 32-bit I2S MSB  │
     │ • Audio Source Arbiter State Machine      │  │ • Master I2S0 DMA (44.1/48/96 kHz)   │
     └───────────────────────────────────────────┘  └──────────────────────────────────────┘
```

| Task Identifier | Core Affinity | Priority | Stack Budget | Heap Allocation | Input Source | Output Target | Lifecycle Status |
| :--- | :---: | :---: | :---: | :--- | :--- | :--- | :---: |
| **`audio_dsp_task`** | **Core 1** | **10** | **8,192 B** | Internal DRAM / Static | UAC 1.0 FIFO / Synth | I2S0 DMA Channel | **Active Production** |
| **`tinyusb_task`** | **Core 0** | **5** | **4,096 B** | Internal DRAM | USB OTG PHY (GPIO 19/20) | FreeRTOS Audio Buffer | **Active Production** |
| **`ctrl_disp`** | **Core 0** | **4** | **4,096 B** | Internal DRAM | Control Event Queue | Arbiter / PEQ / OLED | **Active Production** |
| **`controls_task`** | **Core 0** | **3** | **3,072 B** | Internal DRAM | PCNT Unit 0 / GPIO Poll (50 Hz) | Control Event Queue | **Active Production** |
| **`oled_ui_task`** | **Core 0** | **2** | **4,096 B** | Internal DRAM | Display State Struct | I2C Bus (`0x3C`/`0x3D`, SDA=8, SCL=9) | **Active Production** |
| `wifi_task` *(Aux)* | Core 0 | 23 | System | Internal DRAM | 2.4 GHz RF PHY | IP Stack (`esp_netif`) | Auxiliary / Future |
| `audio_stream_task` *(Aux)*| Core 1 | 5 | 32 KB | DRAM / PSRAM | HTTP Socket / Wi-Fi | I2S Output Buffer | Auxiliary / Future |
| `sd_reader_task` *(Aux)* | Core 0 | 8 | 8 KB | 128 KB Octal PSRAM | MicroSD Card Slot 1 | Decoder Queue | Auxiliary / Future |

---

## 3. The 10 Golden Rules for Agentic AI Code Generation

Autonomous agents modifying or generating code in this repository MUST comply with [`AGENTS.md`](../../AGENTS.md) and these 10 invariants:

1. **Octal PSRAM Pin Protection (FATAL):** Never allocate, toggle, or mux `GPIO 33, 34, 35, 36, 37`. These are hardwired to the high-speed Octal PSRAM (`APS6404L`). Any access causes an immediate catastrophic kernel panic (`Cache disabled but cached memory region accessed`).
2. **Core 1 Isolation for Audio DSP:** All time-critical audio processing, biquad/FIR DSP calculations, and I2S DMA writes must be pinned to **Core 1** at high priority (Priority 10). Core 0 handles USB event loops, NVS flash commits, and display I/O.
3. **No Dynamic Allocations in Audio Loops:** Never call `malloc()`, `calloc()`, or `realloc()` inside real-time audio processing loops (`audio_dsp_task`, `peq_dsp_process_interleaved_s24_to_s32`). Pre-allocate all buffers in static BSS or initialization paths.
4. **Saturation Clamping:** All floating-point DSP output samples must pass through optional `-0.5 dBFS` True-Peak Limiter safety and be clamped to signed 24-bit full-scale bounds (`[-8388608.0f, 8388607.0f]`) before shifting MSB-aligned into 32-bit I2S slots (`int32_t`).
5. **Driver Gating Before Reclocking:** Before changing I2S sample rates (`i2s_channel_reconfig_std_clock`), the channel MUST be stopped with `i2s_channel_disable()`. Reclocking an active channel triggers an `ESP_ERR_INVALID_STATE` assertion.
6. **Hardware Soft Mute (`GPIO 7: XMT`) Sequencing:** When switching audio sources, changing sample rates, suspending USB, or triggering Auto-Mute on Silence, assert hardware mute (`GPIO 7 LOW`) $\ge 15\text{ ms}$, reconfigure clocks/streams, flush DMA, and release mute (`GPIO 7 HIGH`) with soft ramp.
7. **Volatile Live Tuning vs Explicit Flash Commits:** Real-time PEQ, Matrix, Loudness, Limiter, and A/B adjustments via WebHID or Android Companion must update active state in volatile RAM with double-buffered 15 ms crossfading. NVS v3 (`peq_slot_0..7`, `nvs_meta_v3`, `fir_taps_l`/`r`) flash writes are ONLY permitted on explicit user save commands with `memcmp()` delta checks and CRC32 verification.
8. **I2C Display Auto-Probe (`0x3C` / `0x3D`):** SSD1306 monochrome I2C OLED modules auto-probe both `0x3C` and `0x3D` on GPIO 8 (SDA) and GPIO 9 (SCL) with non-blocking `2 s` hot-plug recovery.
9. **Zero External MCLK Configuration:** The Texas Instruments GY-PCM5102A DAC has its `SCK` jumper tied to `L` (GND) to utilize its internal low-jitter PLL derived directly from BCK (GPIO 6). Never route an external MCLK line.
10. **Cross-Layer Client & UI Parity:** Keep 64-byte `0x4551` HID packets, accessible audiophile UI labels, and tests synchronized across `main/`, `tools/webhid_tuner/src/` (React + `esbuild` single-file bundle, never commit `dist/`), and `tools/android_companion/`.

---

## 4. Subsystem Function Directory

Detailed function specifications are organized into the following modular reference guides:

### Active Production Subsystems & Agent Customizations
| Module Guide | Primary Source File(s) | Functional Scope | Key Functions / Artifacts Documented |
| :--- | :--- | :--- | :--- |
| **[Workspace Rules & Skills](../../AGENTS.md)** | `AGENTS.md`, `.agents/skills/*` | Always-on invariants + `ascendance-hid-parity` & `ascendance-verify-build` runbooks. | `AGENTS.md`, `ascendance-hid-parity`, `ascendance-verify-build` |
| **[01. Audio Pipeline & I2S Driver](01_audio_pipeline_i2s.md)** | `main/audio_pipeline.c`, `driver/i2s_std.h` | I2S master 32-bit slot mode, DMA configuration, perceptual volume taper, startup chime, soft mute. | `audio_pipeline_init`, `audio_pipeline_write_s32`, `audio_pipeline_set_mute`, `audio_pipeline_set_sample_rate`, `audio_pipeline_play_startup_chime` |
| **[03. Parametric Equalizer (PEQ) DSP](03_parametric_eq_dsp.md)** | `main/peq_dsp.c`, `main/peq_dsp.h` | 8 RBJ topologies, Dual-Mono L/R PEQ, True-Peak Limiter, Equal Loudness, `esp-dsp` FIR, 8 NVS v3 slots. | `peq_dsp_init`, `peq_dsp_process_interleaved_s24_to_s32`, `peq_dsp_set_ab_bypass`, `peq_dsp_set_linked_stereo`, `peq_dsp_set_mixer`, `peq_dsp_save_user_profile_nvs` |
| **[04. USB Audio Class & WebHID](04_usb_audio_webhid.md)** | `main/webhid_peq.c`, `main/webhid_peq.h`, `tools/webhid_tuner/src/`, `tools/android_companion/` | UAC 1.0 tri-rate isochronous endpoint (24-bit 44.1/48/96 kHz), USB suspend PM, 64-byte HID (`0x01..0x14`), React & Android clients. | `webhid_peq_init`, `webhid_peq_audio_read`, `webhid_peq_consume_pending_sample_rate`, `tud_audio_set_req_ep_cb`, `tud_hid_set_report_cb`, `webhid_peq_send_band_report_ch` |
| **[06. UI Display & Hardware Controls](06_ui_display_controls.md)** | `main/controls.c`, `main/oled_display.c` | SSD1306 128x64 OLED (`0x3C`/`0x3D`, 4 views, burn-in guard), PCNT rotary decoder, debounced buttons. | `controls_init`, `oled_display_init`, `oled_display_set_state`, `oled_display_set_sample_rate`, `oled_display_show_eq_overlay`, `oled_display_set_eq_preset` |
| **[07. Audio Source Arbiter](07_audio_source_arbiter.md)** | `main/audio_source_arbiter.c` | Multi-mode audio router (USB DAC ↔ Tone Arpeggio ↔ Sweep), zero-pop 4-phase muting sequence. | `audio_source_arbiter_init`, `audio_source_arbiter_cycle`, `audio_source_arbiter_get_current` |

### Auxiliary & Future Subsystems
| Module Guide | Primary Source File(s) | Functional Scope | Key Functions Documented |
| :--- | :--- | :--- | :--- |
| **[02. Wi-Fi & HTTP Streamer](02_wifi_and_http_streamer.md)** | `minimp3.h` (Historical / Future) | Station initialization, WPA2/WPA3 auto-negotiation, active AP scanning, HTTP chunk reading, MP3 frame decoding. | `wifi_init_sta`, `wifi_event_handler`, `audio_stream_task`, `mp3_decode_render_frame` |
| **[05. MicroSD (SDMMC) Local Player](05_sdmmc_local_player.md)** | `driver/sdmmc_host.h`, `fatfs` | 1-bit SDMMC host setup, FATFS filesystem mount, 128 KB PSRAM ringbuffer feeder, FLAC/WAV decoders. | `sdmmc_player_init`, `sd_reader_task`, `sd_player_play_file`, `flac_decode_stream` |
| **[08. Dual-MCU Bridge Protocol](08_dual_mcu_bridge.md)** | Inter-chip I2S/UART | Slave I2S audio sink, high-speed packetized UART bridge (921,600 baud), command serialization, CRC16 verification. | `dual_mcu_i2s_slave_init`, `dual_mcu_uart_init`, `dual_mcu_send_command` |

---

## 5. Agentic AI Prompting & Code Generation Guidelines

When instructing an AI subagent or performing automated refactors, reference [`AGENTS.md`](../../AGENTS.md), activate `.agents/skills/` (`ascendance-hid-parity` / `ascendance-verify-build`), and format constraints explicitly:

```markdown
Role: ESP32-S3 Audio & Cross-Layer DSP Engineer
Target Function: peq_dsp_process_interleaved_s24_to_s32 (plus WebHID/Android parity if protocol changes)
Constraints:
- Comply with AGENTS.md and read relevant docs/software_functions/* specs first.
- Core Affinity: Must run on Core 1 inside audio_dsp_task (zero malloc/flash writes).
- Forbidden GPIOs: Ensure no registers touch GPIOs 33-37.
- Verification: Firmware compiles via `idf.py build` / `.github/workflows/build.yml`; WebHID passes `npm test && npm run build`; Android passes `testDebugUnitTest`.
```
