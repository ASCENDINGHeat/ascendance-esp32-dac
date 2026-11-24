# Project ASCENDANCE — Repository Agent Rules

Follow these repository-specific rules whenever working inside `ascendance-esp32-dac`. For complete architectural and hardware references, consult [docs/reference/agentic_ai_reference.md](docs/reference/agentic_ai_reference.md) and [docs/software_functions/README.md](docs/software_functions/README.md).

## 1. Repository Subsystems

- **`main/` (`ESP-IDF v6.1` C Firmware):** Targets `esp32s3` (YD-ESP32-S3 N16R8 + GY-PCM5102A DAC + TDA1308 Amp + SSD1306 128×64 OLED). Key modules: [`peq_dsp.c`](main/peq_dsp.c), [`audio_pipeline.c`](main/audio_pipeline.c), [`webhid_peq.c`](main/webhid_peq.c), [`oled_display.c`](main/oled_display.c), [`controls.c`](main/controls.c), [`audio_source_arbiter.c`](main/audio_source_arbiter.c), [`main.c`](main/main.c).
- **`tools/webhid_tuner/` (React 18 + Vite + `esbuild` PWA):** Modular JSX/JS under [`src/`](tools/webhid_tuner/src/) and static PWA files under [`public/`](tools/webhid_tuner/public/). Built into a single offline HTML bundle at `tools/webhid_tuner/dist/index.html` (gitignored — **never commit `dist/` or `node_modules/`**).
- **`tools/android_companion/` (Kotlin + Jetpack Compose App):** Native Android USB HID control app (`com.ascendance.dac`) claiming only control interface `#2` (`EP 0x02 OUT` / `EP 0x82 IN`, VID `0x303A` / PID `0x4002`) so Android UAC audio stays uninterrupted.

## 2. Non-Negotiable Firmware & DSP Invariants

1. **Octal PSRAM Pins Forbidden:** Never configure or touch `GPIO 33, 34, 35, 36, 37`.
2. **Core Affinity Isolation:** Keep hard real-time audio (`audio_dsp_task`, Priority `10`, Core `1`) strictly isolated from USB, I2C OLED (`oled_ui_task`), input polling, and NVS flash commits (all Core `0`).
3. **Zero Heap Allocation in Audio Path:** Never call `malloc`, `calloc`, or `realloc` inside `audio_dsp_task` or `peq_dsp_process_interleaved_s24_to_s32()`.
4. **24-Bit Saturation Clamping:** Flush subnormal biquad states (`< 1e-15f`) and clamp float samples to `[-8388608.0f, 8388607.0f]` (behind `1.5 ms` `-0.5 dBFS` True-Peak Limiter when enabled) before MSB-shifting into 32-bit I2S slots.
5. **Click-Free Crossfading:** Route all live parameter, A/B, Dual-Mono, matrix, loudness, FIR, and harmonics (Bass Enhancer / Exciter) switches through Core 0 `portMUX_TYPE` staging and Core 1's 15 ms double-buffered linear crossfader.
6. **Zero-Pop `GPIO 7`, DWC2 FIFO Limit & Reclocking Order:** Assert hardware mute (`GPIO 7 LOW`) $\ge 15\text{ ms}$ before disabling I2S (`i2s_channel_disable`) for sample-rate changes (`44.1 / 48.0 kHz`) or source handovers; never configure an external MCLK pin (PCM5102A synthesizes MCLK from BCK). Keep UAC 1.0 `EP 0x01 OUT` `wMaxPacketSize <= 294` bytes (`48 kHz / 24-bit`) — ESP32-S3's DWC2 USB OTG peripheral shares `1,024` bytes of total hardware FIFO RAM across RX + TX endpoints (`588` B @ `96 kHz / 24-bit` exceeds the RX FIFO and drops packets silently).
7. **Volatile Live Tuning vs. Explicit Flash Saves:** Live HID tuning (`0x02`, `0x03`, `0x08`, `0x09`, `0x0B`, `0x0C`, `0x0E`, `0x13`) updates RAM only. Write to NVS v3 (`peq_slot_0..7`, `nvs_meta_v3`, `fir_taps_l`/`r`, `harm_cfg_v1`) only on explicit save commands (`0x05`, `0x0A`, `0x0D`, `0x14` with commit flag, or long-press Save) with CRC32 + `memcmp` delta verification.
8. **Task Stack Headroom:** Keep `CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192` in `sdkconfig.defaults`, `ctrl_disp` $\ge 6144$ B, `audio_dsp_task` `8192` B, and avoid kilobyte audio buffers on task stacks (use `static` BSS).

## 3. Cross-Layer Protocol & UI Terminology Parity

- **64-Byte Packet Header:** Every WebHID / Android HID packet uses little-endian magic `0x4551` (`"EQ"`), command byte `0x01..0x0E`, `0x13`, `0x14`, and sub-tags `0xFD` (with harmonics mirrored in `reserved[20..27]`) / `0xFE`.
- **Parity Rule:** Any change to HID packet offsets, DSP enums (`FilterType`, `MixerMode`, `LoudnessMode`, `OledViewMode`, `ExciterMode`), or slot limits (`8` slots, `1..10` bands, `256` FIR taps @ `44.1 / 48 kHz`) must be updated together across `main/` (`webhid_peq.h/.c`, `peq_dsp.h/.c`), `tools/webhid_tuner/src/`, `tools/android_companion/.../usb/PeqPacketCodec.kt` + `DspModels.kt`, and `docs/`.
- **Accessible Audiophile UI Copy:** Keep `ASCENDANCE` / `ASCENDANCE Hi-Fi DAC` branding intact and expose user-friendly terms (`Slot 1`–`Slot 8`, `DAC Presets (1–8)`, `Preview Live`, `Save to DAC`, `Template: Flat/Bass/Harman`, `Bass Enhancer`, `Harmonic Exciter`, `FIR Impulse Filter`, `Equal Loudness (ISO 226)`, `OLED Screen View`, `Auto-Mute on Silence`, `OLED Auto-Dim & Sleep`, `Activity Log`) instead of chip/SDK jargon (`NVS`, `SRAM`, `GPIO`, `SIMD`, `ESP32-S3`, `0x1F`) in UI strings.
- **128×64 OLED Line Width:** Keep 6px-font OLED menu strings $\le 20$ rendered characters at `x = 2`.

## 4. Verification & Documentation Discipline

- **WebHID (`tools/webhid_tuner/`):** Run Vitest (`npm test`), single-file `esbuild` build (`npm run build`), and `node --check` on the `<script>` inside `dist/index.html`.
- **Android (`tools/android_companion/`):** Maintain unit tests in [`PeqPacketCodecTest.kt`](tools/android_companion/app/src/test/java/com/ascendance/dac/PeqPacketCodecTest.kt) (`gradle testDebugUnitTest lintDebug`).
- **Docs First:** Read existing relevant docs in `docs/` before modifying firmware, protocol, or hardware specs, and keep `README.md` concise.
