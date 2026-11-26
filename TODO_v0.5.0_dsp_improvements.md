# Project ASCENDANCE — v0.5.0 DSP Pipeline Improvements Draft

Target release: **`v0.5.0`**  
Scope: Core 1 audio precision, headroom protection, headphone spatialization, and Core 0 OLED FFT visualization (strictly preserving all 10 Golden Invariants: `44.1 / 48 kHz` 24-bit UAC 1.0, Core 1 zero-heap real-time isolation, `15 ms` click-free crossfade, and `860 B <= 1,024 B` DWC2 FIFO budget).

---

## Phase 1 — Audible Polish & Headroom Protection *(Low CPU, High Impact)*

- [ ] **1. Per-Sample Volume Smoother & Logarithmic dB Taper (`main/peq_dsp.c`, `main/audio_pipeline.c`)**
  - Replace per-block (`~5.3 ms`) `volume_factor` stepping in `peq_dsp_process_interleaved_s24_to_s32()` with a 1-pole per-sample smoother (`~10 ms` time constant) to eliminate zipper noise during knob turns or OS volume drags.
  - Add optional true logarithmic dB taper (`-60 dB` to `0 dB`) for fine low-volume IEM control.
- [ ] **2. Auto-Preamp Headroom Guard (`main/peq_dsp.c`, `main/webhid_peq.c`, WebHID + Android)**
  - Reuse `peq_dsp_eval_curve_db()` peak positive gain + active Bass Enhancer / Exciter mix headroom to automatically compute negative preamp compensation (`15 ms` crossfade), preventing `0 dBFS` tracks from slamming into the limiter when boosting EQ or harmonics.
- [ ] **3. 24-Bit TPDF Dithering (`main/peq_dsp.c`)**
  - Apply fast `xorshift32` Triangular Probability Density Function (TPDF) dither ($\pm 1\text{ LSB}$ at bit 24) immediately before `clamp_s24_to_s32()` to decorrelate quantization noise at low digital volumes.

---

## Phase 2 — Filter Precision & Tonal Neutrality *(Low–Medium CPU)*

- [ ] **4. Double-Precision Sub-Bass Biquads + ESP-DSP SIMD (`main/peq_dsp.c`)**
  - Use `double` state accumulators for low-frequency biquad bands (`f0 < 120 Hz`) to eliminate single-precision quantization noise on narrow-$Q$ sub-bass filters at `48 kHz`.
  - Evaluate `espressif/esp-dsp` PIE-accelerated `dsps_biquad_f32` for mid/high bands to maintain Core 1 CPU headroom.
- [ ] **5. Tonal-Neutral BS2B Headphone Crossfeed (`main/peq_dsp.c`)**
  - Upgrade Chu Moy crossfeed with complementary high-shelf / low-pass BS2B summing (Chu Moy `700 Hz / 4.5 dB`, Jan Meier `700 Hz / 6.0 dB`, `650 Hz / 9.5 dB`) so toggling Crossfeed keeps overall bass/mid tonal balance flat (`0 dB` net sum).
- [ ] **6. Global Switchable Subsonic / DC Blocker (`15–20 Hz`) (`main/peq_dsp.c`)**
  - Add an optional 1st-order `18 Hz` HPF before the lookahead limiter so infrasonic rumble and residual DC offset never trigger false limiter gain reduction or waste headphone excursion.

---

## Phase 3 — Dynamic Processing & Live OLED Visualizer *(Medium CPU)*

- [ ] **7. Dynamic Bass (Envelope-Controlled Sub-Bass) (`main/peq_dsp.c`)**
  - Envelope-following sub-bass shelf that gently boosts low/moderate passages and pulls back smoothly on heavy sub-bass peaks (`> -3 dBFS`) to preserve punch without mud or limiter pumping.
- [ ] **8. Real-Time 16-Band FFT Spectrum Analyzer (5th OLED View) (`main/peq_dsp.c`, `main/oled_display.c`)**
  - Copy a lock-free 256-sample mono snapshot in internal DRAM every `~30 ms` on Core 1.
  - Run `espressif/esp-dsp` `dsps_fft2r_fc32` strictly on **Core 0 (`oled_ui_task`)** to render a live 16-bar logarithmic RTA spectrum on the 128×64 SSD1306 OLED with zero Core 1 audio overhead.

---

## Cross-Layer Parity Checklist (Before `v0.5.0` Release)
- [ ] Sync any new HID command flags / `0xFD` telemetry bits across `main/webhid_peq.h/.c`, `tools/webhid_tuner/src/`, `tools/android_companion/.../usb/PeqPacketCodec.kt` + `DspModels.kt`, and `docs/`.
- [ ] Keep all 6px-font OLED menu labels $\le 20$ characters at `x = 2`.
- [ ] Verify ESP-IDF v6.1 CI (`build.yml`), WebHID Vitest + `esbuild` single-file bundle, and Android unit tests + lint (`android.yml`).
