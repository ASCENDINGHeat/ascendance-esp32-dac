# Project ASCENDANCE — v0.5.0 DSP Pipeline & Cross-Platform UI Draft

Target release: **`v0.5.0`**  
Scope: Core 1 audio precision, headroom protection, headphone spatialization, live FFT spectrum visualization, and **full UI + HID protocol parity** across **On-Device OLED**, **React WebHID Tuner**, and **Android Companion (`EQ` | `Tone` | `Presets` | `Device`)**.  
Invariants preserved: `44.1 / 48 kHz` 24-bit Async UAC 1.0 (`860 B <= 1,024 B` DWC2 FIFO), Core 1 zero-heap real-time isolation, `15 ms` click-free crossfade, volatile live tuning vs. explicit NVS commits, and accessible audiophile UI copy (no chip/SDK jargon).

---

## Phase 1 — Audible Polish & Headroom Protection *(Low CPU, High Impact)*

- [x] **1. Per-Sample Volume Smoother & Logarithmic dB Taper (`main/dsp/peq_dsp.c`, `main/audio_pipeline.c`)**
  - [x] Replace per-block (`~5.3 ms`) `volume_factor` stepping in `peq_dsp_process_interleaved_s24_to_s32()` & `s16_to_s32()` with a 1-pole per-sample smoother (`10 ms` time constant) to eliminate zipper noise during knob turns or OS volume drags.
  - [x] Add selectable volume taper: **Quadratic (Default)** vs. **Logarithmic dB (`-60..0 dB`)** for fine low-volume IEM control. (Full parity: firmware + OLED menu + WebHID + Android Companion + `dsp_pro_v1` NVS.)
- [x] **2. Auto-Preamp Headroom Guard (`main/peq_dsp.c`)**
  - Reuse `peq_dsp_eval_curve_db()` peak positive gain + active Bass Enhancer / Exciter mix headroom to automatically compute negative preamp compensation (`15 ms` crossfade), preventing `0 dBFS` tracks from slamming into the limiter when boosting EQ or harmonics. (Full parity: firmware + OLED menu + WebHID + Android Companion + NVS v3.)
- [x] **3. 24-Bit TPDF Dithering (`main/peq_dsp.c`)**
  - Apply fast `xorshift32` Triangular Probability Density Function (TPDF) dither ($\pm 1\text{ LSB}$ at bit 24) immediately before `clamp_s24_to_s32()` when signal is non-silent (preserving bit-exact digital zero for auto-mute) to decorrelate quantization noise at low digital volumes.
- [x] **3b. True Post-DSP Output VU Metering (`main/dsp/peq_dsp.c`)**
  - Moved the VU peak/RMS tap in `process_dsp_chunk_f32_to_s32()` (shared by `s24_to_s32` and `s16_to_s32`) from `mx_l / mx_r` to `fin_l / fin_r` (post-volume, post-limiter, post-wake-ramp, post-soft-clipper, pre-TPDF-dither) with dither gated on `fin_l / fin_r` magnitude and raw-input `s_silence_samples` preserved for Auto-Mute.

---

## Phase 2 — Filter Precision & Tonal Neutrality *(Low–Medium CPU)*

- [ ] **4. Double-Precision Sub-Bass Biquads + ESP-DSP SIMD (`main/peq_dsp.c`)**
  - [x] Upgraded RBJ `calculate_biquad()` coefficient synthesis to `double` precision (zero Core 1 CPU overhead, eliminates low-frequency coefficient quantization).
  - [ ] Optionally use `double` state accumulators for `f0 < 120 Hz` bands and evaluate `espressif/esp-dsp` PIE-accelerated `dsps_biquad_f32` for mid/high bands.
- [ ] **5. Tonal-Neutral BS2B Headphone Crossfeed (`main/peq_dsp.c`)**
  - [x] Added coherent low-frequency crossfeed makeup headroom normalization (`1 / (1 + 0.5 * cf_gain)`) in `compute_mixer_rt()` so toggling Crossfeed levels 1 & 2 stays loudness-neutral without boosting bass into the limiter.
  - [ ] Expand to 4-level BS2B presets (`Off`, `Chu Moy 700 Hz / 4.5 dB`, `Jan Meier 700 Hz / 6.0 dB`, `J. Meier 650 Hz / 9.5 dB`) with full UI parity.
- [x] **6. Global Switchable Subsonic / DC Blocker (`18 Hz`) (`main/dsp/peq_dsp.c`)**
  - Added optional 1st-order `18 Hz` HPF before the lookahead limiter (`r = exp(-2*pi*18/fs)`) so infrasonic rumble and residual DC offset never trigger false limiter gain reduction, with full OLED + WebHID + Android + `dsp_pro_v1` NVS parity (plus 24-Bit TPDF Dither UI toggles).

---

## Phase 3 — Dynamic Processing & Live OLED Visualizer *(Medium CPU)*

- [ ] **7. Dynamic Bass (Envelope-Controlled Sub-Bass) (`main/peq_dsp.c`)**
  - Envelope-following sub-bass shelf that gently boosts low/moderate passages and pulls back smoothly on heavy sub-bass peaks (`> -3 dBFS`) to preserve punch without mud or limiter pumping.
- [ ] **8. Real-Time 16-Band FFT Spectrum Analyzer (5th OLED View) (`main/peq_dsp.c`, `main/oled_display.c`)**
  - Copy a lock-free 256-sample mono snapshot in internal DRAM every `~30 ms` on Core 1.
  - Run `espressif/esp-dsp` `dsps_fft2r_fc32` strictly on **Core 0 (`oled_ui_task`)** to render a live 16-bar logarithmic RTA spectrum on the 128×64 SSD1306 OLED with zero Core 1 audio overhead.

---

## Phase 4 — HID Protocol & Cross-Platform UI Parity *(New Features $\rightarrow$ New UI)*

### A. 64-Byte HID Protocol (`0x4551`) & NVS Persistence
- [x] **Add `WEBHID_CMD_SET_DSP_PRO` (`0x0F`):**
  - Bitflags for: `tpdf_dither_en` (`0x01`), `subsonic_filter_en` (`0x02`), `vol_taper_log_en` (`0x04`), `dynamic_bass_en` (`0x08`), `auto_preamp_en` (`0x10`), plus `dynamic_bass_amount_pct` (`0..100`) and expanded `oled_view` (`0..4`: Classic, EQ Curve, VU Meter, Split, **Spectrum**, or `0xFF` unchanged).
  - [ ] Expand `crossfeed_level` (`0..3`: Off, Chu Moy, Jan Meier, J. Meier) in `WEBHID_CMD_SET_MIXER` (`0x13`).
- [x] **Mirror New State in `0xFD` Telemetry Report (`reserved[28..31]` = `bytes 40..43`):**
  - Keeps `reserved[0..27]` 100% backward-compatible with `v0.4.x` clients; packs `dsp_pro_version=1`, `pro_flags`, `dynamic_bass_amount_pct`, and `oled_view_ext` into `bytes 40..43`.
- [x] **NVS Persistence:** Added `dsp_pro_v1` blob (`peq_storage_nvs.c`) with CRC32 + `memcmp` delta verification, committed only on explicit Save.

### B. React WebHID Tuner (`tools/webhid_tuner/src/`)
- [x] **Dynamics / Preamp Card:** Added **Auto-Preamp Headroom Guard** toggle + live `(-X.X dB)` attenuation badge and **Volume Taper** (`Standard (Quadratic)` / `Logarithmic dB (IEM)`).
- [ ] **Tone / Dynamics Card:**
  - Add **Dynamic Bass** toggle + Amount slider (`0–100%`) inside the Bass Enhancer / Tone section.
  - Add **Subsonic Filter (18 Hz)** and **24-Bit TPDF Dither** toggles under **Dynamics & Output Purity** (`0x0F` bits 0 & 1 already wired in firmware/NVS).
  - Update **Headphone Crossfeed** selector to 4 presets (`Off`, `Chu Moy`, `Jan Meier`, `J. Meier`).
- [ ] **Device Settings Card:**
  - Add **Spectrum (FFT)** as the 5th option in **OLED Screen View**.
- [ ] **AutoEQ Quick-Import:** Paste/drop AutoEQ `ParametricEQ.txt` (or raw URL) with 1-click Auto-Preamp headroom calculation + automatic DAC Slot Name suggestion from filename/model.
- [x] **Protocol & Tests:** Updated `src/hid/protocol.js` (`0x0F`, `0xFD[40..43]`) with Vitest golden-vector tests (`10/10` passing) and verified `npm run build`.

### C. Android Companion App (`tools/android_companion/`)
- [x] **Models & Codec (`DspModels.kt`, `PeqPacketCodec.kt`, `PeqPacketCodecTest.kt`):**
  - Added `DspProConfig`, `CMD_SET_DSP_PRO` (`0x0F`), `0xFD[40..43]` decoding, and unit tests.
  - [ ] Add `OledViewMode.SPECTRUM` and 4-level BS2B `CrossfeedLevel`.
- [ ] **`Tone` Tab (`ToneDynamicsCard.kt`, `ToneHarmonicsCard.kt`, `ToneMatrixCrossfeedCard.kt`):**
  - [x] Added **Auto-Preamp Headroom Guard** switch + live attenuation readout in `ToneDynamicsCard.kt`.
  - [ ] Add **Dynamic Bass** toggle + slider inside `ToneHarmonicsCard.kt`.
  - [ ] Add **Subsonic Filter (18 Hz)** and **24-Bit TPDF Dither** switches inside `ToneDynamicsCard.kt`.
  - [ ] Expand **Crossfeed** segmented control to `Off` | `Chu Moy` | `Meier` | `J. Meier` in `ToneMatrixCrossfeedCard.kt`.
- [ ] **`Presets` Tab (`PresetsTab.kt`):**
  - [ ] Add **AutoEQ Quick-Import** (paste/import AutoEQ `ParametricEQ.txt` with 1-click Auto-Preamp + Slot Name auto-fill from filename).
- [ ] **`Device` Tab (`DeviceTab.kt`):**
  - [x] Added **Volume Taper** segmented toggle (`Standard` | `Log dB (IEM)`).
  - [ ] Add **Spectrum** chip to **OLED Screen View** (`Classic` | `Curve` | `VU` | `Split` | `Spectrum`).

### D. On-Device SSD1306 128×64 OLED UI (`main/oled_display.c`, `main/main.c`)
- [ ] **5th Playback View (`OLED_VIEW_SPECTRUM`):** 16-bar logarithmic FFT visualizer with peak-hold dots, selectable via `BTN2` double-click, Menu, WebHID, or Android Companion.
- [ ] **OLED Main Menu Additions (strictly $\le 20$ chars at `x = 2`):**
  - [x] `AutoPreamp:On/Off`
  - [x] `VolTaper:Std/Log`
  - [ ] `Subsonic:On/Off`
  - [ ] `Dyn Bass:On/Off`
  - [ ] `View: Spectrum` (in existing `OLED View` cycler)
  - [ ] `Crossfeed: Off/CM/JM/JM+`

---

## Verification Checklist (Before `v0.5.0` Release)
- [ ] Firmware CI (`build.yml` under `espressif/idf:v6.1`) builds cleanly with zero `-Werror=address-of-packed-member` or stack/RAM regressions.
- [ ] WebHID Tuner passes `npm test`, `npm run build`, and `node --check` on `dist/index.html` (`dist/` and `node_modules/` remain untracked).
- [ ] Android Companion passes `gradle testDebugUnitTest lintDebug assembleDebug` (`android.yml`).
- [ ] Documentation updated (`docs/firmware/current_firmware_implementation.md`, `docs/guides/usb_dac_guide.md`, `CHANGELOG.md`).
