# Changelog

All notable changes to **Project ASCENDANCE** are documented in this file.

## [v0.5.0] — Unreleased

### Added (ESP32-S3 Firmware, OLED, WebHID & Android Parity)
- **Real-Time 32-Band 1024-pt FFT Spectrum Analyzer (`OLED_VIEW_SPECTRUM`, 5th OLED View):** Lock-free `1024`-sample post-DSP mono capture on Core 1 (zero overhead when inactive) feeding `espressif/esp-dsp` `dsps_fft2r_fc32` + Hann window in static BSS on Core 0 (`oled_ui_task`) to render 32 logarithmic bars (`47 Hz..18 kHz`) with `600 ms` peak-hold dots, selectable across OLED, WebHID, and Android Companion.
- **Dynamic Bass (Envelope-Controlled Sub-Bass):** Core 1 `90 Hz` 2nd-order Butterworth sub-bass stage (`5 ms` attack / `120 ms` release, `20 ms` gain smoother, `0–100%` amount) that boosts low/moderate passages and smoothly ducks above `-18 dBFS` toward `-3 dBFS`, with zero-CPU bypass when disabled and full OLED/WebHID/Android controls.
- **4-Level Tonal-Neutral BS2B Headphone Crossfeed:** Upgraded Crossfeed to `Off`, `Chu Moy` (`700 Hz / -4.5 dB`), `Jan Meier` (`700 Hz / -6.0 dB`), and `J. Meier` (`650 Hz / -9.5 dB`) with coherent low-frequency normalization across Firmware, OLED (`Off/CM/JM/JM+`), WebHID, and Android.
- **Auto-Preamp Headroom Guard:** Sweeps active L/R biquad + Equal Loudness cascades across `20 Hz..20 kHz` on Core 0 and applies automatic headroom attenuation (`0x0B` bit `0x04`, `0xFD` bit `0x40` + byte `39` live `auto_att_x10` readout) to prevent digital clipping.
- **Selectable Logarithmic dB Volume Taper (`-60..0 dB`):** Added IEM-friendly logarithmic volume curve alongside the standard quadratic taper (`audio_pipeline_set_log_volume_taper()`), toggleable on OLED (`VolTaper:Std/Log`), WebHID, and Android.
- **Global 18 Hz Subsonic / DC Blocker & 24-Bit TPDF Dither Toggles:** Added a 1st-order `18 Hz` subsonic filter before the limiter plus full WebHID & Android UI controls for both Subsonic and 24-Bit TPDF Dither.
- **Post-DSP Output VU Metering:** Moved Core 1 VU peak/RMS tap & TPDF dither gate to true post-DSP output (post-volume, post-limiter, post-soft-clip, pre-dither) while preserving raw-input silence detection for Auto-Mute.
- **HID `CMD_SET_DSP_PRO` (`0x0F`) & `dsp_pro_v1` NVS Blob:** Versioned 64-byte extension command (`0x0F`), backward-compatible `0xFD` bytes `40..43` (`reserved[28..31]`) telemetry mirror, and CRC32-verified `dsp_pro_v1` flash persistence.

### Refactored & Optimized
- **Subsystem Modularization & Standardized Filenames (M1–M5):** Reorganized `main/` into `main/audio/` (`audio_i2s`, `audio_arbiter`), `main/dsp/` (`dsp_peq.h`, `dsp_internal.h`, `dsp_core`, `dsp_biquad`, `dsp_harmonics`, `dsp_mixer_fir`, `dsp_nvs`), `main/usb/` (`usb_uac`, `usb_hid`), `main/ui/` (`ui_oled`, `ui_ssd1306`), and `main/hal/` (`hal_controls`, `hal_led`); removed unused `minimp3.h` (`-1,865` lines).
- **Core 1 Lock-Free VU & Mixer Fast-Path (P1/P4):** Deduplicated Core 1 `s24`/`s16` DSP loops, made VU telemetry 100% lock-free with C11 `_Atomic`, and added a precomputed `mixer_rt_t.identity` fast-path bypass.
- **Android `ToneTab` Modularization (A1):** Split `ToneTab.kt` into focused card composables under `ui/screens/tone/`.

## [v0.4.2] — 2026-11-26

### Fixed (ESP32-S3 Firmware)
- **Playback-Start Reboot (`CoprocessorException` in USB ISR):** Rewrote `webhid_update_async_feedback()` (`main/webhid_peq.c`) in pure 32-bit/64-bit Q8 fixed-point integer math (zero FPU instructions) and converted `s_audio_sample_rate` to a lock-free `DRAM_ATTR _Atomic uint32_t`. TinyUSB `0.20+` invokes `tud_audio_rx_done_isr()` directly from the hardware USB ISR where Xtensa's FPU is disabled.
- **IRAM Safety for USB Audio ISR:** Marked `tud_audio_rx_done_isr`, `audio_spsc_level_frames`, `audio_spsc_push_frames`, `webhid_update_async_feedback`, and `audio_pipeline_get_dma_sync` with `IRAM_ATTR` (and placed `s_rx_tmp` in `DRAM_ATTR`) so USB ISO RX packets remain safe during SPI flash cache disables (`nvs_commit()`).
- **First Feedback Packet Priming:** Primed nominal 10.14 feedback on `TINYUSB_EVENT_ATTACHED` and on every valid `SET_CUR` sample-rate request (even if unchanged) so `EP 0x81 IN` does not transmit `0x000000` on initial `Alt 1` activation.
- **Cross-Core RMT & I2S Concurrency Fixes:** Added a 10 ms timeout FreeRTOS mutex around WS2812B RMT transmits in `main/led_indicator.c`, converted `s_current_source` in `main/audio_source_arbiter.c` to `_Atomic`, removed Core 0 direct I2S writes and the 1 KB stack `silence[]` buffer so Core 1 exclusively owns `s_tx_chan`, and restored `"ASCENDANCE Hi-Fi"` OLED title branding.

### Changed
- **Debug-Gated Startup Chime:** Added `CONFIG_ASCENDANCE_STARTUP_CHIME` (`default n`) in `main/Kconfig.projbuild` so production builds boot silently (~900 ms faster USB enumeration, safer for sensitive IEMs) while hardware bench/debug builds can still enable the verification chime.

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
