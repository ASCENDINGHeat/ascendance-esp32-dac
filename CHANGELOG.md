# Changelog

All notable changes to **Project ASCENDANCE** are documented in this file.

## [v0.4.1] — 2026-11-26

### Fixed & Improved (ESP32-S3 Firmware)
- **24-Bit Stereo PCM Byte-Shift Crackling (`SW_BUF_SZ` Alignment + Resync Guard):** Changed `CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ` from `4096` (`4096 % 6 = 4`) to `4704` (`16 × 294 B` = `784 × 6 B`) in `CMakeLists.txt`, and added a `< 6`-byte remainder discard guard in `tud_audio_rx_done_isr()` (`main/webhid_peq.c`). Prevents partial FIFO overruns during live parameter changes from leaving orphaned bytes that permanently shift 24-bit frame alignment into persistent `> 0 dBFS` crackling.
- **I2S-DMA-Coupled PI + EMA 10.14 Async Feedback:** Registered IRAM-safe I2S TX DMA `on_sent` / `on_send_q_ovf` telemetry callbacks (`audio_pipeline_get_dma_sync()`) in `main/audio_pipeline.c`, and upgraded `webhid_update_async_feedback()` in `main/webhid_peq.c` from P-only to a DMA-gated **Proportional-Integral (PI) + EMA low-pass filtered** 10.14 feedback controller (`alpha = 1/16`, anti-windup $\pm 0.25\text{ sample/ms}$, hard clamp $\pm 1.0\text{ sample/ms}$) to cancel static ESP32-S3 XTAL + I2S divider ppm drift without burst ripple.

## [v0.4.0] — 2026-11-26

### Added & Optimized (ESP32-S3 Firmware)
- **Asynchronous UAC 1.0 Feedback (`EP 0x81 IN`, 10.14 Fixed-Point):** Switched `EP 0x01 OUT` from Adaptive to Asynchronous (`TUSB_ISO_EP_ATT_ASYNCHRONOUS | TUSB_ISO_EP_ATT_DATA`, `bSynchAddress = 0x81`) and added `TUD_AUDIO10_DESC_STD_AS_ISO_SYNC_EP(0x81, 0x03)` (`bRefresh = 3`, 8 ms cadence) with `CFG_TUD_AUDIO_ENABLE_FEEDBACK_EP=1`. Keeps `wMaxPacketSize = 294 B` (`44.1 / 48 kHz` 24-bit) so total DWC2 FIFO stays at `860 B <= 1,024 B` (double-buffered RX + 3 TX FIFOs). Feedback is derived proportionally from SPSC ring buffer occupancy and hard-clamped to nominal $\pm 1.0\text{ sample/ms}$.
- **Lock-Free C11 `_Atomic` SPSC Ring Buffer & Cross-Core Atomics:** Decoupled Core 1 (`audio_dsp_task`) from TinyUSB's `tu_fifo_t` by draining incoming ISO packets on Core 0 inside `tud_audio_rx_done_isr()` into a `DRAM_ATTR` `2,048`-frame (`12,288 B`) power-of-two SPSC ring buffer with `memory_order_acquire`/`memory_order_release` fences and generation-counter flush on stream stop/start/reclock. Converted shared cross-core USB and audio pipeline state flags in `main/webhid_peq.c` and `main/audio_pipeline.c` to C11 `_Atomic`.
- **WebHID Bus Contention Fix:** Raised WebHID interrupt endpoint `bInterval` from `1` ms to `4` ms (`250 Hz`), eliminating unnecessary 1 kHz interrupt polling alongside the 1 ms isochronous audio stream while retaining 4× headroom over the 60 FPS WebHID UI.

### Changed (Android Companion App)
- **4-Tab Navigation (`EQ` | `Tone` | `Presets` | `Device`):** Replaced the `Matrix` tab with a consolidated `Tone` tab (`ToneTab.kt`) grouping **Bass Enhancer & Harmonic Exciter**, **Dynamics & Equal Loudness**, **Channel Matrix & Crossfeed** (with collapsible `Custom 2×2 Matrix`), and a collapsible **FIR Impulse Filter** card.
- **EQ-Only Frequency Response Canvas:** Restricted `PeqCanvasCard` to the `EQ` tab, reclaiming vertical screen height on `Tone`, `Presets`, and `Device`.
- **Top Bar Ergonomics & Haptics:** Added a tappable active-slot pill (`Slot 1–8` dropdown menu) in `TopAppBar`, an amber unsaved-changes badge on `Save` (`DacUiState.hasUnsavedChanges`), and subtle `LocalHapticFeedback` on A/B Bypass, Save, slot switches, and Tone toggles.
- **Persisted Theme Switcher:** Added **Appearance** selector in `DeviceTab` (`Dark Studio`, `AMOLED`, and Android 12+ `Dynamic` Material You) persisted in `SharedPreferences`, with `MaterialTheme.colorScheme` adoption across all tabs. Bumped Android app to `versionCode = 4`, `versionName = "0.4.0"`.

## [v0.3.1] — 2026-11-23

### Fixed
- **USB Audio Playback (`24-bit / 44.1 & 48.0 kHz`):** Lowered UAC 1.0 `CFG_TUD_AUDIO_FUNC_1_SAMPLE_RATE` from `96000` to `48000` and `EP 0x01 OUT` `wMaxPacketSize` from `588` to `294` bytes (`TUD_AUDIO10_DESC_TYPE_I_FORMAT_LEN(2)`). ESP32-S3's DWC2 Full-Speed USB OTG peripheral shares `1,024` bytes of total hardware FIFO RAM across RX + TX endpoints, so `588`-byte packets exceeded the RX FIFO and caused silent USB playback.
- **Boot Stack Overflow:** Moved `out_buf` (`2,048 B`) and `buffer` (`1,024 B`) in `main/audio_pipeline.c` off the `main` task stack into static BSS, raised `CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192` in `sdkconfig.defaults`, and increased `ctrl_disp` stack size from `4096` to `6144` bytes in `main/main.c`.
- **OLED VU Meter Responsiveness:** Switched metering in `main/peq_dsp.c` to pre-volume (post-DSP/mixer, zeroed when muted), accumulated inter-frame max peaks with smoothed RMS ballistics between `100 ms` OLED ticks, widened the OLED VU floor from `-48 dBFS` to `-60 dBFS`, and added `peq_dsp_clear_vu_metrics()` on `100 ms` USB idle.

## [v0.3.0] — 2026-11-19

### Added
- **Psychoacoustic Bass Enhancer & Harmonic Exciter (`WEBHID_CMD_SET_HARMONICS` `0x0E`):**
  - Global DSP stage on Core 1 (post-PEQ/Loudness/FIR, pre-Matrix/Volume/Limiter) with `15 ms` click-free wet/dry crossfade and `A/B Bypass` integration.
  - Precomputed 4096-point `float32` waveshaper LUTs allocated once in `8 MB` Octal PSRAM (`MALLOC_CAP_SPIRAM`) for Bass (`2f + 3f` soft harmonic) and Exciter (`Tube Warmth`, `Tape Saturation`, `Air Sheen`), paired with static internal DIRAM `2×` polyphase half-band oversampling at `<= 48 kHz`.
  - Global NVS persistence under key `"harm_cfg_v1"` (CRC32 + `memcmp` delta verification), `0xFD` `reserved[20..27]` telemetry mirror, OLED Main Menu toggles (`8. Bass Enh`, `9. Exciter`), and full WebHID + Android Companion controls & unit tests.
- **CI Size Profiling:** Added collapsible `idf.py size-components` and `idf.py size-files` summaries plus `build/ASCENDANCE.map` artifact upload in `.github/workflows/build.yml`.
- **Modular React WebHID Tuner & Workspace Rules:** Refactored `tools/webhid_tuner/` into React 18 + Vite + `esbuild` single-file bundle (`dist/index.html`), added accessible audiophile UI terminology across all interfaces, and added `AGENTS.md` + `.agents/skills/`.

## [v0.2.0] — 2026-11-18

### Added
- **8 Custom NVS v3 Preset Slots (`Slot 1`–`Slot 8`):** Replaced hardcoded factory presets with 8 user-editable, custom-named hardware slots (`peq_slot_0..7` + `nvs_meta_v3`), plus legacy `v0.1.0` migration into `Slot 1`.
- **Dynamics, Loudness & FIR Convolution:** Added `-0.5 dBFS` (`1.5 ms` lookahead) True-Peak Limiter, volume-coupled Equal Loudness (ISO 226), and ESP-DSP PIE-accelerated `dsps_fir_f32` FIR Impulse Filter (`0x14` chunked upload, up to `256` taps/ch).
- **OLED Visualizer Views & Power Management:** Added 4 OLED playback views (`Classic`, `EQ Curve`, `VU Meters`, `Split`), `0x3C`/`0x3D` auto-probe, Auto-Mute on Silence, OLED Auto-Dim & Sleep, and USB Suspend low-power mode.

## [v0.1.0] — 2026-11-16

### Added
- Initial composite USB Audio Class 1.0 + raw 64-byte WebHID (`0x4551`) DAC firmware for ESP32-S3 N16R8 + GY-PCM5102A + TDA1308 + SSD1306 OLED.
- Dual-Mono 1–10 band Parametric EQ, `2×2` Channel Matrix + Chu Moy Headphone Crossfeed, A/B Bypass, WebHID Tuner, and native Kotlin/Compose Android Companion app (`tools/android_companion/`).
