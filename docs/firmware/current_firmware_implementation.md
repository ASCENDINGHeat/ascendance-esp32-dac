# ESP32-S3 High-Fidelity USB DAC & DSP: Current Firmware Implementation

This document provides a comprehensive technical record of the firmware as currently implemented, compiled, and verified on the **YD-ESP32-S3 (N16R8)** hardware platform.

---

## 1. System Overview & Hardware Architecture

The firmware implements **Schematic 2 (`docs/schematics/DSP+DAC+SCREEN.svg`)**, transforming the ESP32-S3 into a driverless, high-resolution **USB Audio Class 1.0 (UAC 1.0) DAC** with a **dynamic 1-to-10 band real-time Parametric Equalizer (PEQ)** and simultaneous **WebHID live tuning** over a single USB-C connection.

```
                           YD-ESP32-S3 (N16R8)
 ┌─────────────────────────────────────────────────────────────────────────────┐
 │ Native USB OTG (GPIO 19: D-, GPIO 20: D+)                                   │
 │   • Interface 0: USB Audio Control (AC) - Mute & Volume Entity              │
 │   • Interface 1: USB Audio Streaming (AS) - 24-bit / 44.1 & 48.0 kHz PCM    │
 │   • Interface 2: Vendor HID (WebHID) - 64-Byte Raw Report Bi-Directional    │
 ├─────────────────────────────────────────────────────────────────────────────┤
 │ Core 0 (System & Control Subsystems)                                        │
 │   • TinyUSB Background Task & Event Dispatcher (16 KB UAC SW FIFO)          │
 │   • Hardware PCNT Rotary Encoder Decoder (GPIO 15/16) + Push Switch (17)    │
 │   • Dual Mode/Nav Pushbuttons (`SW6` GPIO 18, `SW8` GPIO 10)                │
 │   • SSD1306 128x64 OLED Display Driver (I2C Master: SDA=8, SCL=9)          │
 │   • Onboard WS2812B aRGB LED Volume Loudness Indicator (GPIO 48 via RMT)   │
 │   • Audio Source Arbiter & Zero-Pop Handover Protocol                       │
 │   • Versioned NVS v3 Persistence (`peq_slot_0..7`, `nvs_meta_v3`,              │
 │     `fir_taps_l`/`r`, `harm_cfg_v1`, CRC32 + `memcmp` delta verification)   │
 ├─────────────────────────────────────────────────────────────────────────────┤
 │ Core 1 (Real-Time Audio DSP Subsystem)                                      │
 │   • Safe Zero-Pop Sample-Rate Reclocking (44.1 / 48.0 kHz)                  │
 │   • 1-to-10 Band Dual-Mono L/R PEQ (8 RBJ Biquad Topologies) + Loudness     │
 │   • `esp-dsp` FIR Impulse Filter + PSRAM-LUT Bass & Tube/Tape/Air Exciter   │
 │   • 2x2 Channel Matrix & 3-Level Chu Moy Headphone Crossfeed                │
 │   • 15 ms Click-Free Crossfader + Preamp-Normalized Instant A/B Bypass      │
 │   • -0.5 dBFS Lookahead Limiter + 24-bit Clamp -> 32-bit I2S MSB            │
 │   • Master I2S0 DMA Audio Pipeline (32-bit Stereo Slots)                    │
 ├─────────────────────────────────────────────────────────────────────────────┤
 │ External Audio Hardware Interconnects                                       │
 │   • GPIO 4: Master I2S Word Select / LCK (WS)  ────────► GY-PCM5102A DAC   │
 │   • GPIO 5: Master I2S Serial Data (DIN/DOUT) ────────► (32-bit / 384kHz)   │
 │   • GPIO 6: Master I2S Bit Clock (BCK)        ────────►                     │
 │   • GPIO 7: Hardware Soft Mute (Active-LOW)   ────────►                     │
 │                                                              │ Analog Out    │
 │                                                              ▼               │
 │                                                       TDA1308 Stereo Amp     │
 │                                                       (3.5mm Headphone Jack) │
 └─────────────────────────────────────────────────────────────────────────────┘
```

---

## 2. Master Pinout & Hardware Mapping

| GPIO Pin | Function | Peripheral | Electrical Characteristics |
| :---: | :--- | :--- | :--- |
| **GPIO 4** | I2S Word Select (`WS` / `LCK`) | Master I2S0 | CMOS Output (44.1 / 48.0 kHz frame clock) |
| **GPIO 5** | I2S Serial Data Out (`DOUT` / `DIN`) | Master I2S0 | CMOS Output (24-bit audio MSB-aligned in 32-bit stereo slots) |
| **GPIO 6** | I2S Continuous Bit Clock (`BCK`) | Master I2S0 | CMOS Output (`64 * fs`: 2.8224 / 3.072 MHz) |
| **GPIO 7** | DAC Hardware Soft Mute (`XMT` / `MUTE`) | GPIO Output | Active-LOW (0 = Mute, 1 = Unmute); Pulled down on boot |
| **GPIO 8** | I2C Serial Data (`SDA`) | I2C Master 0 | Open-drain, 400 kHz fast mode, external 4.7 kΩ pull-up |
| **GPIO 9** | I2C Serial Clock (`SCL`) | I2C Master 0 | Open-drain, 400 kHz fast mode, external 4.7 kΩ pull-up |
| **GPIO 15** | Rotary Encoder Phase A | PCNT Unit 0 | Input, internal pull-up, 1000 ns hardware glitch filter |
| **GPIO 16** | Rotary Encoder Phase B | PCNT Unit 0 | Input, internal pull-up, 1000 ns hardware glitch filter |
| **GPIO 17** | Rotary Push Switch (`SW`) | GPIO Input | Active-LOW, internal pull-up, debounced |
| **GPIO 18** | Button 1 (`BTN 1` / `SW6`) | GPIO Input | Active-LOW, 10k hardware pull-up (`R8`), debounced; Navigation Left (◄) / Menu / Back |
| **GPIO 10** | Button 2 (`BTN 2` / `SW_NAV2`) | GPIO Input | Active-LOW, internal pull-up enabled, debounced; Navigation Right (►) / Next / Preset Cycle |
| **GPIO 19** | Native USB Data Negative (`D-`) | USB OTG PHY | Dedicated USB Full-Speed transceiver differential pair |
| **GPIO 20** | Native USB Data Positive (`D+`) | USB OTG PHY | Dedicated USB Full-Speed transceiver differential pair |
| **GPIO 43** | UART0 RX (Console Debug) | UART0 | Connected to CH343 USB-to-UART bridge on COM port |
| **GPIO 44** | UART0 TX (Console Debug) | UART0 | Connected to CH343 USB-to-UART bridge on COM port |
| **GPIO 48** | Onboard WS2812B aRGB LED | RMT Peripheral | Single-wire 800 kHz NRZ protocol; dynamic volume loudness brightness & mute indicator |

---

## 3. USB Subsystem: Composite UAC 1.0 + WebHID

Implemented in [`main/usb/usb_hid.c`](../../main/usb/usb_hid.c).

### 3.1 USB Device & Interface Association Descriptor (IAD)
To ensure driverless plug-and-play operation across Windows 10/11, macOS, Linux, and Android, the device advertises the standard USB Interface Association Descriptor (IAD) class:
* **Vendor ID (VID):** `0x303A` (Espressif Systems)
* **Product ID (PID):** `0x4002` (TinyUSB Audio + HID Composite)
* **Device Class:** `0xEF` (`TUSB_CLASS_MISC`)
* **Device SubClass:** `0x02` (`MISC_SUBCLASS_COMMON`)
* **Device Protocol:** `0x01` (`MISC_PROTOCOL_IAD`)
* **Strings:**
  - `0`: Language English (`0x0409`)
  - `1`: `"Ascension Audio"` (Manufacturer)
  - `2`: `"ESP32-S3 Hi-Fi DSP DAC"` (Product)
  - `3`: `"123456"` (Serial Number)
  - `4`: `"Ascension Hi-Fi Audio"` (Audio Interface)
  - `5`: `"WebHID Live PEQ Interface"` (HID Interface)

### 3.2 Configuration Descriptor Structure
1. **Config Descriptor Header:** 9 bytes (`TUD_CONFIG_DESCRIPTOR(1, 3, 0, CONFIG_TOTAL_LEN, 0x00, 100)`).
2. **IAD (Interface Association Descriptor):** 8 bytes grouping Interface 0 (AC) and Interface 1 (AS).
3. **Interface 0: Audio Control (AC):**
   - Standard AC Interface (9 bytes, 0 endpoints).
   - Class-Specific AC Header (9 bytes, $bcdADC = 0x0100$, links Interface 1).
   - Input Terminal (12 bytes, Terminal ID `0x01`, USB Streaming, 2 channels, Stereo L/R).
   - Output Terminal (9 bytes, Terminal ID `0x03`, Generic Speaker, source Feature Unit `0x02`).
   - Feature Unit (13 bytes, Unit ID `0x02`, Mute + Volume controls on Master, Channel 1, and Channel 2).
4. **Interface 1: Audio Streaming (AS):**
   - Alternate Setting 0: Zero-bandwidth idle interface (9 bytes, 0 endpoints).
   - Alternate Setting 1: Active audio streaming interface (9 bytes, 2 endpoints: `EP 0x01 OUT` + `EP 0x81 IN` feedback).
   - Class-Specific AS Header (7 bytes, PCM format, 1 frame delay, links Terminal `0x01`).
   - Type I Format Descriptor (`TUD_AUDIO10_DESC_TYPE_I_FORMAT_LEN(2)` = 14 bytes: 2 channels, 3 bytes/sample, 24-bit resolution, dual-rate `44100, 48000` Hz).
   - Standard Isochronous Audio Data Endpoint (9 bytes, `EP 0x01 OUT`, Asynchronous, `294` max bytes fitting ESP32-S3's `1,024`-byte DWC2 FIFO at `860 B` total, 1 ms polling, `bSynchAddress = 0x81`).
   - Class-Specific Isochronous Endpoint (7 bytes, sampling frequency control).
   - Standard Isochronous Synch/Feedback Endpoint (9 bytes, `EP 0x81 IN`, 3-byte 10.14 fixed-point feedback, `bInterval = 1`, `bRefresh = 3` $\rightarrow$ 8 ms cadence).
5. **Interface 2: Vendor HID (WebHID):**
   - Standard HID Interface Descriptor (32 bytes, `EP 0x02 OUT`, `EP 0x82 IN`, 64-byte packets, `bInterval = 4` ms polling to avoid 1 ms bus contention with ISO audio).
   - 64-byte Generic In/Out Raw Report Descriptor.

### 3.3 UAC 1.0 Callbacks & Host Synchronization
* **`tud_audio_rx_done_isr` / `tud_audio_feedback_params_cb`:** Drains incoming ISO OUT packets strictly on Core 0 into a `DRAM_ATTR` lock-free C11 `_Atomic` SPSC ring buffer (`2,048` stereo 24-bit frames = `12,288 B`, acquire/release memory fences), computes proportional 10.14 feedback clamped to nominal $\pm 1.0\text{ sample/ms}$, and updates `EP 0x81 IN` via `tud_audio_fb_set()` (`Q16.16` $\rightarrow$ `10.14`).
* **`tud_audio_set_itf_cb` / `tud_audio_set_itf_close_ep_cb`:** Tracks when the host activates Alternate Setting 1 (active playback) vs Alternate Setting 0 (idle), flushes the SPSC ring buffer via a generation counter, primes nominal 10.14 feedback, and displays `"USB DAC Active"` overlay on the OLED screen without altering the persistent footer preset label.
* **`tud_audio_get_req_entity_cb`:** Responds to host volume queries:
  - `GET_CUR`: Returns current volume in 1/256 dB units.
  - `GET_MIN`: Returns $-90\text{ dB}$ ($-23040$).
  - `GET_MAX`: Returns $0\text{ dB}$ ($0$).
  - `GET_RES`: Returns $1.0\text{ dB}$ ($256$).
* **`tud_audio_set_req_entity_cb`:** Synchronizes host volume slider and mute status directly into the firmware pipeline:
  - Host mute triggers `audio_pipeline_set_mute()` (hardware pin GPIO 7).
  - Host volume slider converts dB (`<= -80 dB` $\rightarrow$ `0%`, otherwise `round(100 * 10^(dB / 40))`) to align with the firmware's quadratic perceptual volume taper (`(volume / 100)^2`).
* **`tud_audio_get_req_ep_cb` / `tud_audio_set_req_ep_cb`:** Reports active UAC sample rate (`44100` or `48000` Hz), rejects other rates on `SET_CUR`, flushes SPSC + primes nominal feedback, and stages `s_pending_sample_rate` under `s_uac_rate_lock` for safe Core 1 zero-pop reclocking (`audio_pipeline_set_sample_rate()` + `peq_dsp_set_sample_rate()` + `oled_display_set_sample_rate()`).
* **`tud_hid_get_report_cb` / `tud_hid_set_report_cb` / `webhid_peq_send_band_report_ch`:** Supports `0x01..0x0F`, `0x13`, and `0x14` (GetProfile / `0xFE` matrix / `0xFD` extended state, SetBandLive, SetPreampLive, SelectProfile, SaveToPreset, ResetToPreset, SetBandCount, SetAbBypass, SetDualMono, RenameSlot, SetDynamics, SetPowerCfg, FactoryResetSlot, SetHarmonics, SetDspPro, SetMixer, UploadFirChunk).
  - **`WEBHID_CMD_SET_HARMONICS` (`0x0E`):** 64-byte `0x4551` LE report carrying `flags` in `byte 3` (`bit 0 = bass_enabled`, `bit 1 = exciter_enabled`, `bits 2..3 = exciter_mode` where `0=Tube`, `1=Tape`, `2=Air`), `bass_amount_pct` (`0..100`) in `byte 4`, `exciter_amount_pct` (`0..100`) in `byte 5`, `bass_freq_hz` (`uint16` LE, `50..150`) in `bytes 6..7`, and `exciter_freq_hz` (`uint16` LE, `1500..8000`) in `bytes 10..11`. Firmware clamps all values, calls `peq_dsp_set_harmonics_cfg()`, and broadcasts an updated `0xFD` report.
  - **`WEBHID_CMD_SET_DSP_PRO` (`0x0F`):** Configures `pro_flags` in `byte 3` (`0x01=tpdf_dither`, `0x02=subsonic_18hz`, `0x04=vol_taper_log`, `0x08=dynamic_bass`, `0x10=auto_preamp`), `dynamic_bass_amount_pct` (`0..100`) in `byte 4`, and expanded `oled_view` (`0..4`, or `0xFF` unchanged) in `byte 5`.
  - **`0xFD` Extended-State Telemetry Mirror (`reserved[20..31]`):** Preserves `reserved[0..19]` (slot name + FIR tap metadata), mirrors harmonics in `reserved[20..26]`, reports live Auto-Preamp attenuation `auto_att_x10` in `reserved[27]` (byte `39`), and mirrors `v0.5.0` DSP Pro state in `reserved[28..31]` (bytes `40..43`: `version=1`, `pro_flags`, `dynamic_bass_amount_pct`, `oled_view_ext`).

---

## 4. Parametric Equalizer (PEQ) DSP Engine

Implemented across `main/dsp/` ([`dsp_core.c`](../../main/dsp/dsp_core.c), [`dsp_biquad.c`](../../main/dsp/dsp_biquad.c), [`dsp_harmonics.c`](../../main/dsp/dsp_harmonics.c), [`dsp_mixer_fir.c`](../../main/dsp/dsp_mixer_fir.c), [`dsp_nvs.c`](../../main/dsp/dsp_nvs.c)).

### 4.1 Filter Topologies & Biquad Formulations
Filters follow Robert Bristow-Johnson (RBJ) Audio EQ Cookbook Direct Form II Transposed biquads:

$$y[n] = b_0 x[n] + w_1[n-1]$$
$$w_1[n] = b_1 x[n] - a_1 y[n] + w_2[n-1]$$
$$w_2[n] = b_2 x[n] - a_2 y[n]$$

All coefficients are normalized by dividing by $a_0$ (with `isfinite()` and near-zero `a0` passthrough fallbacks, plus subnormal delay-line flushing `< 1e-15f` inside `biquad_filter_sample()`).
* **Peaking Filter (Parametric Bell):**
  $$\omega_0 = 2\pi \frac{f_0}{f_s}, \quad A = 10^{\frac{\text{Gain}_{\text{dB}}}{40}}, \quad \alpha = \frac{\sin(\omega_0)}{2Q}$$
  $$b_0 = 1 + \alpha A, \quad b_1 = -2\cos(\omega_0), \quad b_2 = 1 - \alpha A$$
  $$a_0 = 1 + \frac{\alpha}{A}, \quad a_1 = -2\cos(\omega_0), \quad a_2 = 1 - \frac{\alpha}{A}$$
* **Low-Shelf Filter:**
  $$b_0 = A\left((A+1) - (A-1)\cos(\omega_0) + 2\sqrt{A}\alpha\right)$$
  $$b_1 = 2A\left((A-1) - (A+1)\cos(\omega_0)\right)$$
  $$b_2 = A\left((A+1) - (A-1)\cos(\omega_0) - 2\sqrt{A}\alpha\right)$$
  $$a_0 = (A+1) + (A-1)\cos(\omega_0) + 2\sqrt{A}\alpha$$
  $$a_1 = -2\left((A-1) + (A+1)\cos(\omega_0)\right)$$
  $$a_2 = (A+1) + (A-1)\cos(\omega_0) - 2\sqrt{A}\alpha$$
* **Extended Topologies (Gain-Independent, Fixed $Q$):**
  - **High-Pass (HPF, type `4`)** & **Low-Pass (LPF, type `5`):** Subsonic protection and treble roll-off.
  - **Notch (NTCH, type `6`)** & **Band-Pass (BPF, type `7`):** Narrow resonance removal and acoustic band isolation.

### 4.2 Dynamic Band Count Scaling & Dual-Mono L/R PEQ
* Each channel (`PEQ_CH_LEFT`, `PEQ_CH_RIGHT`) scales independently or linked (`s_linked_stereo`, toggled via `WEBHID_CMD_SET_DUAL_MONO`) from **1 to 10 active bands** without heap re-allocation.
* Post-EQ processing applies a `2×2` channel matrix (`Stereo`, `Mono Mix`, `L-Only`, `R-Only`, `Swap L/R`, `Mid-Side Width 0–200%`, `L/R Balance`, `Custom Matrix`, with a zero-overhead `identity` fast-path bypass) followed by a 4-level tonal-neutral **BS2B Headphone Crossfeed** (`0=Off`, `1=Chu Moy 700 Hz/-4.5 dB`, `2=Jan Meier 700 Hz/-6.0 dB`, `3=J. Meier 650 Hz/-9.5 dB` with coherent low-frequency normalization).

### 4.3 Double-Buffered 15 ms Click-Free Crossfader, A/B Toggle & Cross-Core Staging
When parameters, channel mode, matrix/crossfeed, or A/B bypass state change:
1. Core 0 computes new biquads and mixer coefficients on the stack outside locks, then atomically stages `s_pending_*` under `portMUX_TYPE` spinlock (`s_peq_lock`) with generation validation (`s_state_gen`).
2. Core 1 (`peq_dsp_process_interleaved_s24_to_s32` / `peq_dsp_process_interleaved_s16_to_s32`) consumes staged updates into `s_target_*` and runs an interpolation loop across $N_{\text{fade}} = \text{round}(0.015 \times f_s)$ samples ($662 / 720 / 1440$ samples):
   $$y[n] = (1.0 - \beta) \cdot y_{\text{active}}[n] + \beta \cdot y_{\text{target}}[n], \quad \beta \in [0.0, 1.0]$$
3. **Preamp-Normalized Instant A/B Bypass:** `peq_dsp_set_ab_bypass()` (`0x08`) toggles flat EQ comparison across the 15 ms crossfade while keeping `preamp_gain_db` engaged so A vs. B remains loudness-matched (transient only, never persisted to NVS). When `ab_bypass == true`, Equal Loudness and both the Bass Enhancer and Harmonic Exciter smoothly crossfade to dry bypass over `15 ms`.

### 4.4 Saturation Protection, Auto-Preamp, Subsonic HPF, Limiter, Loudness & FIR
* **Pre-Gain Headroom & Auto-Preamp Guard:** Manual pre-attenuation ($0.0\text{ to }-12.0\text{ dB}$) plus optional **Auto-Preamp Headroom Guard** (`0x0B` bit `0x04` / `0x0F` bit `0x10`) that sweeps active L/R biquad + Equal Loudness cascades across `20 Hz..20 kHz` on Core 0 and applies automatic attenuation to prevent digital clipping.
* **Global 18 Hz Subsonic / DC Blocker & 24-Bit TPDF Dither:** Optional 1st-order `18 Hz` HPF before the limiter (`0x0F` bit `0x02`) and toggleable 24-bit triangular-PDF dither (`0x0F` bit `0x01`).
* **True-Peak Limiter, Soft Clipper & Lock-Free Post-DSP VU:** `1.5 ms` lookahead circular stereo delay buffer (`LA_MAX_FRAMES = 144` frames @ 96 kHz), `-0.5 dBFS` (`7918595.0f`) ceiling with `0.4 ms` attack / `100 ms` release envelope, followed by asymptotic soft-knee saturation above `-0.2 dBFS` (`8196846.0f`) ahead of `clamp_s24_to_s32()`. VU peak/RMS telemetry and the `1024`-sample mono FFT snapshot (`PEQ_FFT_SIZE = 1024`) tap post-DSP output lock-free via C11 `_Atomic`.
* **Equal Loudness (ISO 226):** Modes `0=Off`, `1=ISO Mild` (`+4.0 dB` @ 80 Hz / `+1.5 dB` @ 10 kHz), `2=ISO Full` (`+6.5 dB` / `+2.5 dB`), dynamically scaled when master volume `< 80%` and defeated cleanly during A/B bypass.
* **FIR Impulse Filter (`esp-dsp`):** Hardware PIE-accelerated `dsps_fir_f32` convolution with sample-rate adaptive tap limit (`256` taps/ch @ `44.1 / 48 kHz`, `128` taps/ch @ `96 kHz`) stored in 16-byte aligned internal DRAM and NVS keys `fir_taps_l`/`fir_taps_r`.

### 4.5 Psychoacoustic Bass Enhancer, Dynamic Bass & Harmonic Exciter
* **C API & Clamped Ranges ([`main/dsp/dsp_peq.h`](../../main/dsp/dsp_peq.h)):** Configured via `peq_dsp_set_harmonics_cfg()` / `peq_dsp_get_harmonics_cfg()` (`PEQ_EXCITER_TUBE = 0`, `PEQ_EXCITER_TAPE = 1`, `PEQ_EXCITER_AIR = 2`, amounts `0..100%`, Bass cutoff `50..150 Hz`, Exciter cutoff `1500..8000 Hz`) and `peq_dsp_set_pro_cfg()` (Dynamic Bass enable + `0..100%` amount).
* **Signal Chain Placement:** Executes on Core 1 **after** Stage 1 (PEQ + Equal Loudness) and Stage 2 (`esp-dsp` FIR Impulse Filter), and **before** Stage 3 (`2×2` Channel Matrix & BS2B Crossfeed $\rightarrow$ `18 Hz` Subsonic HPF $\rightarrow$ Master Volume $\rightarrow$ `-0.5 dBFS` Lookahead Limiter $\rightarrow$ 24-bit TPDF dither & clamp).
* **PSRAM Waveshaper LUTs & Static DIRAM State:** Inside `peq_dsp_init()`, `4096`-point `float32` interpolated waveshaper LUTs (`HARM_LUT_SIZE = 4096`) are allocated once in Octal PSRAM (`MALLOC_CAP_SPIRAM`) for Bass (`2f + 3f`) and Exciter (`Tube / Tape / Air`) with static internal DIRAM `2×` polyphase half-band oversampling (`<= 48 kHz`).
* **Dynamic Bass (Envelope-Controlled Sub-Bass):** Runs a `90 Hz` 2nd-order Butterworth LPF with `5 ms` attack / `120 ms` release envelope follower and `20 ms` gain smoother that boosts quiet/moderate sub-bass (up to `+6 dB` at `100%`) and ducks smoothly above `-18 dBFS` toward `-3 dBFS`, with zero-CPU bypass when disabled.
* **Click-Free 15 ms Crossfade & A/B Bypass:** Parameter changes stage from Core 0 under `portMUX_TYPE` (`s_peq_lock`) and crossfade wet/dry smoothly on Core 1 over `15 ms`; `ab_bypass == true` (`0x08`) fades both harmonic stages to dry bypass over `15 ms`.

### 4.6 8 Editable DAC Presets (`NVS v3`), `harm_cfg_v1` & `dsp_pro_v1` Persistence
All hardcoded factory presets (`Flat`, `Bass Boost`, `Harman`) have been removed in `v0.2.0`. The firmware initializes **8 fully editable, custom-named DAC preset slots (`Slot 0..7`)** (`peq_slot_0`..`peq_slot_7` + global `nvs_meta_v3` in namespace `audio_peq`):
* On clean boot or `peq_dsp_reset_slot_to_flat()`, all 8 slots seed to **Reference Flat** (`"Slot 1"`..`"Slot 8"`, 5 ISO bands @ `0.0 dB`).
* Legacy `v0.1.0` NVS v2 (`user_cfg_v2`) or V1 (`user_profile`) blobs automatically migrate into `Slot 0`.
* **Global Harmonics & DSP Pro Keys (`"harm_cfg_v1"`, `"dsp_pro_v1"`):** Global Harmonics (`0x0E`) and DSP Pro (`0x0F`) settings persist in NVS with CRC32 + `memcmp` delta verification. Live HID and OLED menu changes remain volatile in RAM and are written to flash **only** on explicit save (`peq_dsp_save_user_profile_nvs()` / `0x05` / long-press Save).

---

## 5. Audio Pipeline & Master I2S0 Subsystem

Implemented in [`main/audio/audio_i2s.c`](../../main/audio/audio_i2s.c).

* **I2S Configuration:**
  - Standard Philips I2S format (`I2S_COMM_MODE_STAND_I2S`).
  - Bit depth: **32-bit stereo slots** (`I2S_DATA_BIT_WIDTH_32BIT`) carrying 24-bit MSB-aligned PCM (`audio_pipeline_write_s32()`).
  - Clock frequencies: **44,100 Hz**, **48,000 Hz** (default), and **96,000 Hz**, dynamically reclocked on Core 1 via safe zero-pop transition (`mute HIGH -> LOW -> disable -> reconfig_std_clock -> enable -> unmute`).
  - Clock Source: Internal PLL (`I2S_CLK_SRC_DEFAULT`).
  - MCLK: Synthesized internally by PCM5102A's internal PLL from BCK.
* **DMA Buffering:**
  - 8 DMA descriptors $\times$ 256 stereo frames $\times$ 8 bytes = 16,384 bytes DMA FIFO (~42.7 ms at 48 kHz, ~21.3 ms at 96 kHz).
* **Hardware Soft-Mute, USB Suspend & Zero-Signal Auto-Mute (GPIO 7):**
  - Pin pulled LOW on boot, during source handover/reclocking, on USB bus suspend (`tud_suspend_cb` releasing `ESP_PM_CPU_FREQ_MAX` down to `80 MHz`), and after configurable digital silence (`abs <= 2` on 24-bit, default `10 s`), waking smoothly with a `1 ms` linear soft ramp.
* **Master Volume Engine:**
  - Quadratic perceptual volume taper (`norm * norm` where `norm = volume_percent / 100.0f`), coupled to `peq_dsp_notify_volume_pct()` for Fletcher-Munson equal-loudness compensation.
* **Startup Verification Chime:**
  - Synthesizes an ascending four-note chime (C5 $523.25\text{ Hz}$, E5 $659.25\text{ Hz}$, G5 $783.99\text{ Hz}$, C6 $1046.50\text{ Hz}$) on boot to verify end-to-end analog hardware integrity.

---

## 6. Audio Source Arbiter & Zero-Pop Handover

Implemented in [`main/audio/audio_arbiter.c`](../../main/audio/audio_arbiter.c).

The system coordinates three operational audio modes using the **4-Phase Zero-Pop Muting Protocol**:

```
 Phase 1: Hardware Soft-Mute (Pull GPIO 7 LOW for 15 ms)
   ▼
 Phase 2: Pipeline Teardown & Buffer Quiescence
   ▼
 Phase 3: DMA Flush & Clock Re-lock (10 ms)
   ▼
 Phase 4: Spin up New Source & Unmute (Pull GPIO 7 HIGH after 15 ms)
```

| Source Identifier | Name | Description |
| :--- | :--- | :--- |
| `AUDIO_SOURCE_STANDALONE` | **USB DAC / DSP** *(Default Boot)* | Consumes incoming 24-bit UAC 1.0 stream from TinyUSB FIFO (`webhid_peq_audio_read`), handles sample-rate reclocking, runs Core 1 DSP, and writes 32-bit slots via `audio_pipeline_write_s32()`. Sleeps `pdMS_TO_TICKS(1)` when idle (`CONFIG_FREERTOS_HZ=1000`). |
| `AUDIO_SOURCE_TONE_TEST` | **Tone Generator** | Synthesizes an A minor arpeggio (A4, C5, E5, A5 cycling every 250 ms) using the active sample rate (`audio_pipeline_get_sample_rate()`). |
| `AUDIO_SOURCE_SWEEP` | **Frequency Sweep** | Generates a continuous sweep scaled to `audio_pipeline_get_sample_rate()` for acoustic diagnostics. |

---

## 7. User Controls & Hardware UI

Implemented in [`main/hal/hal_hal/hal_controls.c`](../../main/hal/hal_hal/hal_controls.c), [`main/ui/ui_oled.c`](../../main/ui/ui_oled.c), and [`main/main.c`](../../main/main.c).

### 7.1 Physical Controls & SSD1306 OLED Engine (Core 0)
* **I2C Auto-Probe, Hot-Plug & Burn-In Guard:** Auto-probes `0x3C` / `0x3D` (`10 ms` I2C timeout) with non-blocking `2 s` reconnect probe, `60 s` contrast dimmer (`0x81 0x1F`) + `±1 px` periodic shift, and `180 s` idle sleep (`0xAE`).
* **Cycleable OLED Screen Views (`OLED_VIEW_*`):** `0=Classic` (HUD), `1=EQ Curve` (`128×28` response plot), `2=VU Meters` (dual horizontal Peak/RMS + `300 ms` peak hold + `CLIP`/`LIM` flags), `3=Split` (`94 px` curve + `30 px` vertical VU).

#### 1. Default Playback Mode (Normal Operation)
* **Rotary Rotation (GPIO 15/16):** Adjusts master volume in $\pm 2\%$ increments with instant HUD overlay and dynamic WS2812B scaling.
* **Encoder Push Switch (GPIO 17):** Short Click toggles Master Mute; Double-Click commits active slot & dynamics to DAC flash (`Save to DAC`); Long Press (`>800 ms`) cycles active audio input.
* **Button 1 (GPIO 18 / ◄ / SW6):** Short Click opens Main Menu; Long Press cycles DAC preset slots backward (`Slot 8`..`1`).
* **Button 2 (GPIO 10 / ► / SW_NAV2):** Short Click cycles DAC preset slots forward (`Slot 1`..`8`); Double-Click cycles **OLED Screen View** (`Classic -> EQ Curve -> VU Meters -> Split`).

#### 2. Main Menu Navigation Mode (13 Items)
* **Rotary Rotation / Button 2 (►):** Scrolls through all 13 options (every rendered string $\le 20$ chars at `x = 2`): `1. Live EQ Adjust`, `2. Slot:<Name>`, `3. A/B:<Active/Bypass>`, `4. Crossfeed:<Off/Low/High>`, `5. Loudness:<Off/Mild/Full>`, `6. Limiter:<On/Off>`, `7. View:<Classic/Curve/VU/Split>`, `8. Bass Enh:<On/Off>`, `9. Exciter:<Off/Tube/Tape/Air>`, `10. Preamp:<dB>`, `11. Input:<Source>`, `12. Save to DAC`, `13. Exit Menu`.
* **Encoder Switch (GPIO 17):** Confirms/cycles highlighted item (`8. Bass Enh` toggles `On/Off`, `9. Exciter` cycles `Off -> Tube -> Tape -> Air -> Off`, both triggering an immediate `0xFD` WebHID telemetry notification via `webhid_peq_notify_state_changed()`); Long Press saves immediately to DAC (`"Saved to DAC"`).
* **Button 1 (◄):** Exits menu back to Playback Mode.

#### 3. On-Device Live EQ Adjust Mode (Digit-by-Digit PEQ Tuner)
Triggered by selecting **"1. Live EQ Adjust"** in the Main Menu:
* **Layout on SSD1306 OLED (128x64):**
  - **Line 0:** `BND:[3]/5  [PEAK]` (Active band index, total band count, filter type: `[BYP ]`, `[PEAK]`, `[L-SH]`, `[H-SH]`, `[HPF ]`, `[LPF ]`, `[NTCH]`, `[BPF ]`).
  - **Line 1:** `FREQ:  0 1 0 0 0  Hz` (5 individual selectable digits: 10k, 1k, 100, 10, 1).
  - **Line 2:** `GAIN:  + 0 4 . 5  dB` (Selectable Sign `+`/`-`, Tens, Ones, Tenths).
  - **Line 3:** `Q   :  0 1 . 4 1` (Selectable Integer, Tenths, Hundredths).
  - **Line 4:** `< >:Move  Turn:0-9` (Status & navigation hint).
  - **Line 5:** Bottom Action Toolbar: `[+B]  [-B]  [SAVE] [EXIT]`
* **Navigation & Adjustment Matrix:**
  - **Button 1 (GPIO 18 / ◄):** Moves cursor Left across fields, digits, and toolbar items.
  - **Button 2 (GPIO 10 / ►):** Moves cursor Right across fields, digits, and toolbar items.
  - **Rotary Rotation (GPIO 15/16):**
    - When cursor is on a **Digit (`0–9`)**: Dials the selected digit up/down with instantaneous biquad recalculation and 15 ms click-free crossfading on Core 1! Adjusting Frequency, Gain, or Q automatically enables the band (and promotes `PEQ_FILTER_BYPASS` $\rightarrow$ `PEQ_FILTER_PEAKING`).
    - When on **Band**: Switches active band (`1..N`).
    - When on **Type**: Cycles filter type (`PEAK` $\rightarrow$ `L-SH` $\rightarrow$ `H-SH` $\rightarrow$ `HPF` $\rightarrow$ `LPF` $\rightarrow$ `NTCH` $\rightarrow$ `BPF`) and enables the band.
    - When on **Sign**: Toggles `+` and `-` and enables the band.
    - When on **Toolbar**: Rotates highlighted toolbar action (`[+B]`, `[-B]`, `[SAVE]`, `[EXIT]`).
  - **Encoder Switch (GPIO 17):**
    - **Short Click:** If on a toolbar action, triggers it (`+B` adds band, `-B` removes band, `SAVE` commits to DAC, `EXIT` returns to playback). If on a parameter, rapidly jumps to the next section (`Band` $\rightarrow$ `Freq` $\rightarrow$ `Gain` $\rightarrow$ `Q` $\rightarrow$ `Toolbar`).
    - **Long Press ($>800\text{ ms}$):** **Instant Save to DAC** (commits to flash with auto-dismissing `"SAVED TO DAC!"` popup).
  - **Button 1 Long Press ($>800\text{ ms}$):** **Cancel & Revert** (discards all unsaved session changes and restores previous profile).
  - **Button 1 Double Click:** Direct shortcut to **Remove Band** (`[-B]`).
  - **Button 2 Double Click:** Direct shortcut to **Add Band** (`[+B]`).
* **Zero Flash Wear Invariant:** Real-time biquad adjustments remain volatile in SRAM until the user explicitly saves.

### 7.2 SSD1306 128x64 Monochrome OLED Display Modes
* Driven over I2C Master (SDA=GPIO 8, SCL=GPIO 9) at 400 kHz with non-blocking 10 FPS rendering task on Core 0.
* **Supported Screen Modes:**
  1. **Playback Mode:** Dynamic source badge (`USB DAC`, `Synth`, `Sweep`), dynamic bit depth/sample rate (`44.1k 24b`, `48.0k 24b`, `96.0k 24b` updated via `oled_display_set_sample_rate()`), headline `"ASCENDANCE Hi-Fi"`, subtitle, volume progress bar, transient volume/`<< EQ SLOT >>` overlay popups, and persistent footer slot label (`Slot 1`).
  2. **Main Menu Mode:** 4-row scrolling viewport with highlighted selection bar, active preset name, current input, preamp gain readout, and navigation footer (`Turn:Move  Click:OK`).
  3. **Live EQ Adjust Mode:** Full-screen digit-by-digit editor supporting all 8 filter types (`BYP`, `PEAK`, `L-SH`, `H-SH`, `HPF`, `LPF`, `NTCH`, `BPF`), unified integer rounding for Gain and Q (`Q = 10.00`), and bottom action buttons.

### 7.3 Onboard WS2812B aRGB LED Volume Loudness Indicator
Implemented in [`main/hal/hal_led.c`](../../main/hal/hal_led.c) and [`main/hal/hal_led.h`](../../main/hal/hal_led.h).

* **Hardware Mapping:** Onboard `XL-5050RGBC-WS2812B` LED connected to **`GPIO 48`** via factory-bridged `RGB` solder jumper.
* **Driver Architecture:** Uses ESP-IDF v6.1 `esp_driver_rmt` Remote Control peripheral configured at 10 MHz resolution (100 ns tick). A custom bytes encoder drives the 800 kHz NRZ protocol (Bit 0: 300 ns High / 900 ns Low; Bit 1: 900 ns High / 300 ns Low; End-of-transmission Low for latching).
* **Optical Safety Invariant:** WS2812B LEDs at 100% duty cycle produce excessive heat and uncomfortable glare. In accordance with Section 3.5 of `docs/hardware/hardware.md`, maximum brightness is capped in firmware to **~25% duty cycle** (`MAX_BRIGHTNESS = 60` out of 255).
* **Audio-Visual Loudness Mapping:**
  - **Muted State:** Soft Red glow (`R=25, G=0, B=0`), providing immediate visual indication of mute state without checking the OLED screen.
  - **0% Volume:** LED completely OFF (`R=0, G=0, B=0`).
  - **1% – 35% Volume (Subtle Glow):** Deep Blue transitioning into Teal.
  - **36% – 75% Volume (Nominal Listening):** Vibrant Pure Cyan tracking output amplitude.
  - **76% – 100% Volume (Dynamic Peak):** Electric Cyan-Mint with subtle white highlights at maximum output.
* **Global Synchronization:** Updates synchronously upon EC11 rotary knob movement, mute toggles, host OS volume changes via UAC 1.0 Feature Unit, and WebHID tuning events.
* **Boot Animation:** Synchronizes color transitions across the ascending 4-note startup chime (C5 $\rightarrow$ E5 $\rightarrow$ G5 $\rightarrow$ C6) before returning to the active volume level.

---

## 8. Driverless WebHID Live Tuner & Offline PWA

Location: [`tools/webhid_tuner/`](../../tools/webhid_tuner/) (`src/`, `public/manifest.json`, `public/sw.js`, `public/icon.svg`, Vite + React build outputting standalone `dist/index.html`).

* **Zero-Driver / PWA Architecture:** Runs client-side in Google Chrome, Microsoft Edge, Brave, Opera, or Android Chrome via `navigator.hid`, with offline service worker installation (`public/manifest.json`, `public/sw.js`, guarded when `location.protocol !== 'file:'`) and `vite-plugin-singlefile` self-contained builds (`npm run build` -> `dist/index.html`).
* **Interactive HTML5 Canvas Response, Phase & Group Delay Graph:**
  - **Frequency Response Visualizer:** Computes cascaded complex transfer function magnitude (`dB`), Phase (`-180°..+180°`), and Group Delay (`ms`) across all 8 filter topologies (`0..7`).
  - **Draggable Nodes, Wheel Q, Double-Click:** Drag center frequency ($20\text{ Hz}\dots 20\text{ kHz}$) & gain ($-24.0\dots +12.0\text{ dB}$), scroll for `Q` ($0.10\dots 10.00$), double-click to reset or add bands.
* **Dual-Mono L/R PEQ, A/B Toggle, Channel Matrix & Crossfeed:** Switch between Linked Stereo and Left/Right tabs, toggle Preamp-Normalized A/B Bypass (`0x08`), adjust Balance, M/S Stereo Width (`0–200%`), **Custom Channel Matrix**, and **Headphone Crossfeed (Chu Moy)** (`Off/Low/High`).
* **Dynamics, Harmonics, FIR Impulse Filter & Power Settings:** Configure **True-Peak Limiter**, **Equal Loudness (ISO 226)**, **Bass Enhancer** (`0–100%`, `50–150 Hz`) & **Harmonic Exciter** (`Tube Warmth / Tape / Air Sheen`, `0–100%`, `1.5–8.0 kHz` via `0x0E` with `0xFD` `reserved[20..27]` telemetry), **FIR Impulse Filter** (`.txt` / `.csv` / `.wav`), **OLED Screen View**, **Auto-Mute on Silence**, and **OLED Auto-Dim & Sleep**.
* **Equalizer APO / AutoEQ `.txt` & `.json` Import/Export:** Imports/exports standard Equalizer APO filter files (`PK`, `LSC`, `HSC`, `HPQ`, `LPQ`, `NO`, `BP`) and full Dual-Mono/Matrix JSON backups.
* **Live Preview Tuning:** Streams live updates in volatile memory with **0 flash wear**; commits to NVS (`nvs_meta_v3` / `peq_slot_0..7` / `harm_cfg_v1`) only on explicit **Save to DAC**.

---

## 9. Firmware Build & Verification Specifications

* **Build Tool:** ESP-IDF v6.1 (`idf.py build`).
* **Automated CI & Releases:**
  - [`.github/workflows/build.yml`](../../.github/workflows/build.yml): Builds ESP32-S3 firmware inside `espressif/idf:v6.1`, generates `ASCENDANCE_merged.bin`, reports `idf.py size` / `size-components` / `size-files` in the job summary, uploads firmware + `ASCENDANCE.map` artifacts, runs Vitest & Vite single-file build (`dist/index.html`) for `tools/webhid_tuner`, validates the compiled script via `node --check`, and uploads `ascendance-webhid-tuner`.
  - [`.github/workflows/release.yml`](../../.github/workflows/release.yml): Builds tagged firmware images, Android APK, and single-file WebHID tuner HTML (`dist/index.html`) into a GitHub Release on `v*` tags.
* **Compiler Flags (`CMakeLists.txt`) & RTOS Config (`sdkconfig.defaults`):**
  ```cmake
  CFG_TUD_AUDIO=1
  CFG_TUD_AUDIO_FUNC_1_SAMPLE_RATE=48000
  CFG_TUD_AUDIO_ENABLE_EP_OUT=1
  CFG_TUD_AUDIO_ENABLE_EP_IN=0
  CFG_TUD_AUDIO_ENABLE_FEEDBACK_EP=1
  CFG_TUD_AUDIO_FUNC_1_EP_OUT_SZ_MAX=294
  CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ=4704
  CFG_TUD_AUDIO_FUNC_1_N_BYTES_PER_SAMPLE_RX=3
  CFG_TUD_AUDIO_FUNC_1_RESOLUTION_RX=24
  CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX=2
  ```
  - `sdkconfig.defaults`: Enables `CONFIG_FREERTOS_HZ=1000` (1 ms OS ticks) and `CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192` alongside 16 MB Flash and 8 MB Octal PSRAM.
  - `audio_dsp_task` (Core 1, Priority 10): 8,192 B stack with static BSS PCM buffers (`s_usb_raw_buf`, `s_synth_s16_buf`, `s_dsp_s32_buf`) reading from the `12,288 B` `DRAM_ATTR` C11 `_Atomic` SPSC ring buffer; `ctrl_disp` (Core 0, Priority 4): 6,144 B stack; `audio/audio_i2s.c` startup chime/write buffers reside in static BSS.
* **Flash Command (over COM UART port `/dev/ttyACM0`):**
  ```bash
  idf.py -p /dev/ttyACM0 flash monitor
  ```
* **Verification Status:** Verified on `v0.4.1` across ESP-IDF v6.1 CI (`build.yml`), Android Companion CI (`android.yml`), local WebHID Vitest + `esbuild` single-file bundle, and physical ESP32-S3 + PCM5102A + SSD1306 hardware.
