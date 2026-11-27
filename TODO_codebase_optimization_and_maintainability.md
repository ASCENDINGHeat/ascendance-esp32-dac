# Project ASCENDANCE — Codebase Optimization & Maintainability Plan

> [!NOTE]
> **Status:** Planning / Audit Only — no source files have been modified.  
> **Codebase Snapshot:** ~18,100 lines total across ESP32-S3 Firmware (`main/`), React WebHID Tuner (`tools/webhid_tuner/`), and Android Companion (`tools/android_companion/`).

---

## 1. Current Hotspot Map

| File | Lines | Current Responsibilities (Why It's Getting Heavy) |
| :--- | ---: | :--- |
| [`main/peq_dsp.c`](main/peq_dsp.c) | **3,090** | Single monolith holding NVS v1/v2/v3 + CRC32, factory presets, RBJ biquads, ISO 226 loudness, Auto-Preamp, Harmonics + LUTs + 2x oversampling, Matrix/Crossfeed, FIR SIMD, Lookahead Limiter, TPDF Dither, VU metering, and duplicated `s24`/`s16` Core 1 audio loops. |
| [`main/minimp3.h`](main/minimp3.h) | **1,865** | Third-party MP3 decoder header sitting directly inside `main/` (unused when USB-only). |
| [`main/webhid_peq.c`](main/webhid_peq.c) | **1,188** | Combines TinyUSB UAC 1.0 descriptors, IRAM USB audio ISR + 10.14 Async feedback + SPSC ring buffer, AND the 64-byte WebHID command dispatcher / `0xFD` report builder. |
| [`main/oled_display.c`](main/oled_display.c) | **1,163** | Combines SSD1306 I2C driver, 5×7 font bitmap, framebuffer primitives, 4 playback views, 14-item menu renderer, and dim/sleep state machine. |
| [`tools/android_companion/.../DacViewModel.kt`](tools/android_companion/app/src/main/java/com/ascendance/dac/ui/DacViewModel.kt) | **946** | God-ViewModel handling USB connection lifecycle, HID packet throttling, incoming report state merging, preset import/export, and all 4 tabs' UI intents. |
| [`main/main.c`](main/main.c) | **891** | Mixes `app_main()`, Core 1 `audio_dsp_task`, Core 0 `ctrl_disp` task, a 14-case OLED menu handler switch, and USB-to-I2S glue. |
| [`tools/webhid_tuner/src/App.jsx`](tools/webhid_tuner/src/App.jsx) | **656** | Root component owning WebHID transport, throttling, state synchronization, file I/O, and layout. |
| [`tools/android_companion/.../ToneTab.kt`](tools/android_companion/app/src/main/java/com/ascendance/dac/ui/screens/ToneTab.kt) | **610** | Single Compose file containing Harmonics, Dynamics/Loudness, Channel Matrix/Crossfeed, and FIR cards. |

---

## 2. Firmware Performance Optimizations (Core 1 Audio & RAM/CPU)

### P1. Deduplicate & Fast-Path the Core 1 DSP Loop (`main/peq_dsp.c`)
- **Current issue:** `peq_dsp_process_interleaved_s24_to_s32()` and `peq_dsp_process_interleaved_s16_to_s32()` duplicate ~80 lines of identical Stage 2 (FIR) + Stage 3 (Harmonics $\rightarrow$ Mixer $\rightarrow$ Volume Smoother $\rightarrow$ Limiter $\rightarrow$ VU $\rightarrow$ TPDF Dither $\rightarrow$ Pack) logic. Every new DSP feature (Auto-Preamp, Subsonic, Dynamic Bass, Post-DSP VU) must be edited in **two** loops.
- **Optimization:** Extract a single `static inline void process_dsp_chunk_f32(const float *ch_l, const float *ch_r, int32_t *dst_s32, size_t chunk, float volume_factor, ...)` shared by both `s24` and `s16` entry points.
- **Extra CPU win:** Add a **flat-bypass fast path** for `s_mixer_rt` (skip `m_ll/m_rl/m_lr/m_rr` multiplies when identity & crossfeed off) and a **limiter-off fast path** (skip lookahead ring-buffer copies when limiter is disabled).

### P2. Block-Oriented Biquad Evaluation + ESP-DSP SIMD (`main/peq_dsp.c`)
- **Current issue:** Biquad PEQ bands run sample-by-sample in scalar C (`for each sample { for each band }`). At 10 bands + 2 loudness shelves (24 biquads/stereo frame), cache locality on coefficients is suboptimal compared to band-major block processing.
- **Optimization:** Process `s_fir_in_l[chunk]` / `s_fir_in_r[chunk]` in **band-major order** (`for each band { dsps_biquad_f32(...) }`) using ESP32-S3 PIE SIMD (already linked via `espressif__esp-dsp`!). Keep `double`-state scalar path only for low-frequency bands (`f0 < 120 Hz`) if needed.
- **Impact:** Frees substantial Core 1 headroom for upcoming `v0.5.0` features (Dynamic Bass, 18 Hz Subsonic HPF, FFT snapshot).

### P3. Eliminate `fabsf()` / Branching Overhead in Inner Loops (`main/peq_dsp.c`)
- **Current issue:** Denormal flush (`if (fabsf(w) < 1e-15f) w = 0.0f;`) runs on every biquad state every sample.
- **Optimization:** Either rely on Xtensa FPU flush-to-zero or inject a tiny alternating Nyquist DC offset (`+1e-18f` / `-1e-18f`) once at the input of the biquad cascade (classic zero-branch denormal prevention).

### P4. Clean Up Lock Granularity Between Core 0 & Core 1
- **Current issue:** `publish_block_metrics()` acquires `taskENTER_CRITICAL(&s_peq_lock)` every ~5.3 ms block on Core 1, contending with Core 0 HID queries (`peq_dsp_get_profile`, `peq_dsp_get_dynamics_cfg`).
- **Optimization:** Publish VU peak/RMS/clip/limiter flags via a small lock-free double-buffered struct or `_Atomic` fields (same pattern already proven in `audio_pipeline.c` DMA telemetry), reducing Core 1 spinlock stalls to zero during steady-state playback.

---

## 3. Firmware Maintainability & Modularization (`main/`)

### M1. Split `main/peq_dsp.c` (3,090 lines) Into Cohesive Modules
Keep [`peq_dsp.h`](main/peq_dsp.h) as the single public API (zero breakage to callers), and split the internals via a private `peq_dsp_internal.h`:

```text
main/
  dsp/
    peq_dsp.c            # Core 1 real-time loop, crossfader, shared chunk processor (~600 lines)
    peq_biquad.c         # RBJ double-precision coeffs, ISO 226 loudness, Auto-Preamp curve (~400 lines)
    peq_harmonics.c      # Bass Enhancer, Harmonic Exciter, LUTs, 2x oversampling (~500 lines)
    peq_mixer_fir.c      # Matrix, M-S Width, BS2B Crossfeed, SIMD FIR convolution (~450 lines)
    peq_storage_nvs.c    # NVS v1/v2/v3 blobs, CRC32, memcmp delta commit, factory slots (~650 lines)
    peq_dsp_internal.h   # Shared internal structs & lock declarations
```
- **Why this matters:** Adding `v0.5.0` features (Subsonic HPF, Dynamic Bass, FFT snapshot, Log Taper) becomes surgical ~40-line edits in the exact module instead of scrolling a 3,100-line file.

### M2. Split `main/webhid_peq.c` (1,188 lines) Into USB Audio vs. HID Protocol
- `main/usb_uac_audio.c`: TinyUSB descriptors, `tud_audio_rx_done_isr`, 10.14 Async feedback PI+EMA, and the lock-free SPSC ring buffer.
- `main/webhid_peq.c`: Pure 64-byte HID command dispatcher (`0x01..0x14`) and `0xFD` / `0xFE` telemetry builders.
- **Why this matters:** Isolates hard real-time `IRAM_ATTR` ISR audio code from protocol parsing so a protocol edit can never accidentally introduce a non-IRAM or FPU call into the USB ISR.

### M3. Split `main/oled_display.c` (1,163 lines) Into Driver vs. UI Views
- `main/ssd1306_drv.c`: I2C transport, 5×7 font table, framebuffer primitives, dim/contrast/sleep.
- `main/oled_ui.c`: Playback views (HUD, EQ Curve, VU, Split, upcoming FFT Spectrum) and table-driven Menu renderer.
- **Table-driven OLED Menu:** Replace the 14-case `switch` statements duplicated across `oled_display.c` and `main.c` with a declarative menu descriptor array (`{ label_fn, on_click_fn, on_rotate_fn }`). Adding a menu item becomes **1 array entry** instead of editing 4 files and manually renumbering items `11..14`.

### M4. Isolate `minimp3.h`
- Move [`main/minimp3.h`](main/minimp3.h) into `main/third_party/minimp3.h` (or remove if SD/MP3 is permanently retired) to keep `main/` clean.

---

## 4. Cross-Layer HID Protocol Maintainability (Biggest Force Multiplier for `v0.5.0`)

### C1. Introduce a Dedicated `v0.5.0` Extension Command (`WEBHID_CMD_SET_DSP_PRO = 0x0F`)
- **Current pressure:** `0xFD` `reserved[27]` (`byte 39`) is now occupied by `auto_att_x10`, leaving bytes `40..63` (`reserved[28..51]`) free in `0xFD`, while `0x0B` (`CMD_SET_DYNAMICS`) is getting crowded with bitflags.
- **Recommendation:** Before adding Log Taper, Subsonic HPF, Dynamic Bass, and 5th OLED Spectrum View, define a clean, versioned `0x0F` (`CMD_SET_DSP_PRO`) packet + `0xFD` bytes `40..47` layout **once**, instead of squeezing bits into `0x0B` one feature at a time.

### C2. Shared Protocol Golden-Vector Spec (`docs/protocol/hid_vectors.json` or Shared Test Vectors)
- Right now, [`webhid_peq.c`](main/webhid_peq.c), [`protocol.js`](tools/webhid_tuner/src/hid/protocol.js), and [`PeqPacketCodec.kt`](tools/android_companion/app/src/main/java/com/ascendance/dac/usb/PeqPacketCodec.kt) are maintained by hand in 3 languages.
- Keeping a compact shared JSON table of golden 64-byte packets (used by both Vitest and JUnit `PeqPacketCodecTest.kt`) guarantees WebHID and Android can never silently drift on byte offsets or endianness.

---

## 5. WebHID & Android Maintainability

### W1. WebHID Tuner (`tools/webhid_tuner/src/`)
- Extract a custom hook `src/hooks/useAscendanceDevice.js` from [`App.jsx`](tools/webhid_tuner/src/App.jsx) (656 lines) to separate WebHID connection/throttling/report-dispatch from React presentation.
- Keep `npm test` + `npm run build` fast and single-file output intact.

### A1. Android Companion (`tools/android_companion/`)
- Split [`DacViewModel.kt`](tools/android_companion/app/src/main/java/com/ascendance/dac/ui/DacViewModel.kt) (946 lines) by extracting:
  - `HidSyncController.kt` (packet throttling, outbound queue, incoming `HidIncomingMessage` state reducer).
  - `PresetImportExportUseCase.kt` (Equalizer APO & FIR file import/export).
- Split [`ToneTab.kt`](tools/android_companion/app/src/main/java/com/ascendance/dac/ui/screens/ToneTab.kt) (610 lines) into focused composables (`HarmonicsCard.kt`, `DynamicsCard.kt`, `MatrixCrossfeedCard.kt`, `FirCard.kt`) matching the WebHID component layout.

---

## 6. Recommended Execution Order (Low Risk $\rightarrow$ High Leverage)

1. **Before remaining `v0.5.0` features (Highest ROI):**
   - **P1** (Deduplicate `s24`/`s16` Core 1 DSP chunk loop) — ~30 min, zero behavior change, makes every remaining DSP feature half the work.
   - **C1** (Lock in `0x0F` / `0xFD[40..47]` layout for all remaining `v0.5.0` flags) — prevents protocol churn across 3 languages.
2. **Mid `v0.5.0` (When touching OLED / DSP heavily):**
   - **M1** (Split `peq_dsp.c` into `main/dsp/*.c`) & **M2** (Split `webhid_peq.c` into UAC audio ISR vs. HID router).
   - **M3** (Table-driven OLED menu + `ssd1306_drv.c` split) — ideal right before adding the 5th FFT Spectrum OLED view.
3. **Post-`v0.5.0` Polish (Optional):**
   - **P2** (Band-major ESP-DSP SIMD biquads), **P4** (Lock-free VU metrics), **W1** (`useAscendanceDevice` hook), **A1** (`DacViewModel` & `ToneTab` decomposition).
