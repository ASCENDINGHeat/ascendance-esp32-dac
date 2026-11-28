# ESP32-S3 High-Fidelity USB DAC & DSP: Future Enhancements & Architectural Roadmap

This specification details the technical architectures, mathematical formulations, descriptor configurations, and implementation requirements for planned firmware and tooling enhancements.

> [!NOTE]
> **Implementation Status (`v0.1.0` & `v0.2.0`):**  
> Sections **1.1–1.10**, **2.1–2.5**, **3.1–3.3**, **4.1–4.2**, **5.1** (`v0.2.0` Studio DSP Pipeline, 8-Slot NVS v3 User Profiles, OLED Visualizers & Protection, USB Suspend + Zero-Signal Auto-Mute, WebHID PWA & Android Companion updates) are now **implemented in codebase (`v0.2.0`)**. Hardware flash validation (`v0.3.0+` Wi-Fi / S/PDIF / SDMMC) remains open for future milestones.

---

## Table of Contents
1. [DSP Audio Engine & Storage Enhancements](#1-dsp-audio-engine-enhancements)
   - [1.1 Extended Biquad Filter Topologies (RBJ Audio Cookbook)](#11-extended-biquad-filter-topologies-rbj-audio-cookbook) *(Implemented)*
   - [1.2 High-Resolution 24-Bit / 96 kHz Audio Pipeline](#12-high-resolution-24-bit--96-khz-audio-pipeline) *(Implemented)*
   - [1.3 Dual-Rate Bit-Perfect Streaming (44.1 kHz & 48.0 kHz)](#13-dual-rate-bit-perfect-streaming-441-khz--480-khz) *(Implemented — Tri-Rate w/ 96 kHz)*
   - [1.4 Independent Left / Right Channel PEQ (Dual-Mono Tuning)](#14-independent-left--right-channel-peq-dual-mono-tuning) *(Implemented)*
   - [1.5 Instant A/B Auditioning Toggle](#15-instant-ab-auditioning-toggle) *(Implemented)*
   - [1.6 Audio Channel Matrix Mixer & Headphone Crossfeed](#16-audio-channel-matrix-mixer--headphone-crossfeed) *(Implemented)*
   - [1.7 Lookahead True-Peak Limiter & Dynamic Soft-Clipper (`v0.2.0`)](#17-lookahead-true-peak-limiter--dynamic-soft-clipper-v020)
   - [1.8 Fletcher-Munson Equal-Loudness Contour (`v0.2.0`)](#18-fletcher-munson-equal-loudness-contour-v020)
   - [1.9 ESP-DSP SIMD FIR Convolution Engine (`v0.2.0`)](#19-esp-dsp-simd-fir-convolution-engine-v020)
   - [1.10 8-Slot Editable NVS v3 User Profiles Replacing Factory Presets (`v0.2.0`)](#110-8-slot-editable-nvs-v3-user-profiles-replacing-factory-presets-v020)
2. [On-Device OLED Display & Physical Controls](#2-on-device-oled-display--physical-controls)
   - [2.1 Mini Real-Time Frequency Response Curve on OLED (`v0.2.0`)](#21-mini-real-time-frequency-response-curve-on-oled)
   - [2.2 Live Stereo Peak / RMS VU Meter (`v0.2.0`)](#22-live-stereo-peak--rms-vu-meter)
   - [2.3 Cycleable Playback Views & Main Menu DSP Controls (`v0.2.0`)](#23-cycleable-playback-views--main-menu-dsp-controls-v020)
   - [2.4 OLED I2C Bus Auto-Probe & Hot-Plug Recovery (`v0.2.0`)](#24-oled-i2c-bus-auto-probe--hot-plug-recovery)
   - [2.5 OLED Contrast Dimmer & Burn-In Screensaver (`v0.2.0`)](#25-oled-contrast-dimmer--burn-in-screensaver-v020)
3. [WebHID & Android Companion Client Tooling](#3-webhid-gui-tooling-enhancements)
   - [3.1 AutoEQ & Equalizer APO Profile Import / Export](#31-autoeq--equalizer-apo-profile-import--export) *(Implemented)*
   - [3.2 Progressive Web App (PWA) Offline Deployment](#32-progressive-web-app-pwa-offline-deployment) *(Implemented)*
   - [3.3 Real-Time Phase & Group Delay Visualization](#33-real-time-phase--group-delay-visualization) *(Implemented)*
4. [Power Management & Intelligent Muting (`v0.2.0`)](#4-power-management--intelligent-muting)
   - [4.1 USB Host Suspend / Standby Detection & Low-Power Sleep](#41-usb-host-suspend--standby-detection--low-power-sleep)
   - [4.2 Zero-Signal Automatic Hardware Soft-Mute](#42-zero-signal-automatic-hardware-soft-mute)
5. [v0.2.0 Extended 64-Byte HID & Companion Protocol](#5-v020-extended-64-byte-hid--companion-protocol)
   - [5.1 Extended `webhid_cmd_t` Command Assignments (`0x0A–0x14`)](#51-extended-webhid_cmd_t-command-assignments-0x0a0x14)

---

## 1. DSP Audio Engine Enhancements

### 1.1 Extended Biquad Filter Topologies (RBJ Audio Cookbook)
The current DSP engine implements **Peaking**, **Low-Shelf**, and **High-Shelf** filters. Expanding the engine with additional topologies will provide comprehensive speaker and headphone acoustic correction:

#### 1. High-Pass Filter (HPF / Low-Cut)
Removes sub-audible subsonic rumble (e.g., turntable warp, DC offsets, mechanical stage thump) and protects compact headphone drivers from excessive low-end excursion:
$$\omega_0 = 2\pi \frac{f_0}{f_s}, \quad \alpha = \frac{\sin(\omega_0)}{2Q}$$
$$b_0 = \frac{1 + \cos(\omega_0)}{2}, \quad b_1 = -(1 + \cos(\omega_0)), \quad b_2 = \frac{1 + \cos(\omega_0)}{2}$$
$$a_0 = 1 + \alpha, \quad a_1 = -2\cos(\omega_0), \quad a_2 = 1 - \alpha$$

#### 2. Low-Pass Filter (LPF / High-Cut)
Attenuates ultrasonic hash, DAC delta-sigma out-of-band imaging noise, and harsh treble resonances:
$$b_0 = \frac{1 - \cos(\omega_0)}{2}, \quad b_1 = 1 - \cos(\omega_0), \quad b_2 = \frac{1 - \cos(\omega_0)}{2}$$
$$a_0 = 1 + \alpha, \quad a_1 = -2\cos(\omega_0), \quad a_2 = 1 - \alpha$$

#### 3. Notch (Band-Stop) Filter
Provides ultra-narrow, surgical attenuation ($Q = 5.0\text{ to }10.0$) to eliminate narrow-band mechanical driver resonances, ground loop hum (50 Hz / 60 Hz), or sharp acoustic chamber modes with zero effect on adjacent musical octaves:
$$b_0 = 1, \quad b_1 = -2\cos(\omega_0), \quad b_2 = 1$$
$$a_0 = 1 + \alpha, \quad a_1 = -2\cos(\omega_0), \quad a_2 = 1 - \alpha$$

#### 4. Band-Pass Filter (BPF)
Isolates specific acoustic bands for bench testing, measurement diagnostics, and speech intelligibility enhancement:
$$b_0 = \alpha, \quad b_1 = 0, \quad b_2 = -\alpha$$
$$a_0 = 1 + \alpha, \quad a_1 = -2\cos(\omega_0), \quad a_2 = 1 - \alpha$$

---

### 1.2 High-Resolution 24-Bit / 96 kHz Audio Pipeline
The Texas Instruments **GY-PCM5102A** DAC features 32-bit delta-sigma conversion supporting up to 384 kHz sampling rates over I2S. Upgrading the USB pipeline from 16-bit to **24-bit PCM**:
* **Dynamic Range:** Increases from $96.3\text{ dB}$ (16-bit) to $112\text{ dB}$ (the analog limit of the PCM5102A).
* **Noise Floor:** Drops quantization distortion to below $-140\text{ dBFS}$.
* **USB Full-Speed Isochronous Bandwidth Budget:**
  $$\text{Data Rate (24-bit / 48 kHz Stereo)} = 48{,}000 \times 2 \times 3\text{ bytes} = 288{,}000\text{ bytes/s} \approx 2.304\text{ Mbps}$$
  This fits comfortably within the USB Full-Speed 12.0 Mbps maximum isochronous bandwidth limit (leaving ample headroom for HID control transfers).
* **Buffer Memory Impact:**
  $$\text{Frame Size} = 48\text{ samples} \times 2\text{ channels} \times 3\text{ bytes} = 288\text{ bytes/ms}$$
  A 4096-byte software ring buffer accommodates ~14.2 ms of uncompressed 24-bit studio-grade audio.

---

### 1.3 Dual-Rate Bit-Perfect Streaming (44.1 kHz & 48.0 kHz)
Currently, the firmware runs at a fixed 48,000 Hz. While host operating systems transparently resample 44.1 kHz streams (e.g. CD-rips, standard Spotify streams) to 48.0 kHz, audiophile playback benefits from bit-perfect transmission:
* **Descriptor Specification:** Expose both frequencies within the Type I format descriptor:
  ```c
  TUD_AUDIO10_DESC_TYPE_I_FORMAT(0x02, 2, 16, 44100, 48000)
  ```
* **Dynamic Reclocking Mechanism:**
  When the host issues `AUDIO10_EP_CTRL_SAMPLING_FREQ` (`SET_CUR`):
  1. Capture requested rate ($44{,}100\text{ Hz}$ or $48{,}000\text{ Hz}$).
  2. Execute zero-pop transition: Soft-mute PCM5102A (`GPIO 7 LOW`).
  3. Reconfigure I2S0 clock divider via `i2s_channel_disable()`, update clock configuration, and `i2s_channel_enable()`.
  4. Recalculate active PEQ biquads for the new sample rate ($\omega_0 = 2\pi f_0 / f_s$).
  5. Unmute hardware (`GPIO 7 HIGH`).

---

### 1.4 Independent Left / Right Channel PEQ (Dual-Mono Tuning)
Many real-world listening environments and headphones exhibit asymmetrical response:
* Headphone driver manufacturing variances ($1\text{ to }3\text{ dB}$ channel mismatch).
* Asymmetrical human hearing thresholds.
* Acoustic room reflection imbalances in desktop speaker setups.

**Implementation Architecture:**
* Split the active filter array into two parallel structures:
  ```c
  typedef struct {
      peq_band_param_t left_bands[PEQ_MAX_BANDS];
      peq_band_param_t right_bands[PEQ_MAX_BANDS];
      bool linked_stereo; // True: L/R adjustments track together; False: Independent dual-mono
  } peq_dual_channel_profile_t;
  ```
* In `peq_dsp_process_interleaved_s16()`:
  - De-interleave L/R samples into separate float registers.
  - Process Left channel through `s_biquads_left[]`.
  - Process Right channel through `s_biquads_right[]`.
  - Re-interleave into output buffer.

---

### 1.5 Instant A/B Auditioning Toggle
To immediately evaluate the psychoacoustic impact of equalization curves:
* A dedicated single-click toggle instantly bypasses all biquads with a **15 ms linear crossfade**.
* Eliminates volume jumps by dynamically normalizing the bypass path with the active preamp headroom gain.
* Allows blind ABX testing of headphone tuning profiles directly from the hardware mode button or WebHID GUI.

---

### 1.6 Audio Channel Matrix Mixer & Headphone Crossfeed
A configurable $2 \times 2$ channel matrix mixer positioned in the audio DSP processing pipeline provides spatial soundstage manipulation, mono downmixing, channel balancing, and natural headphone crossfeed:

#### 1. Matrix Formulation
For each stereo sample pair $\begin{bmatrix} x_L[n] & x_R[n] \end{bmatrix}^T$, the output channels are derived via a linear transfer matrix:
$$\begin{bmatrix} y_L[n] \\ y_R[n] \end{bmatrix} = \begin{bmatrix} m_{LL} & m_{RL} \\ m_{LR} & m_{RR} \end{bmatrix} \begin{bmatrix} x_L[n] \\ x_R[n] \end{bmatrix}$$
where:
* $m_{LL}$: Left input into Left output gain.
* $m_{RL}$: Right input into Left output gain (crosstalk / blend).
* $m_{LR}$: Left input into Right output gain (crosstalk / blend).
* $m_{RR}$: Right input into Right output gain.

#### 2. Standard Operational Modes
* **Normal Stereo (Default):**
  $$\begin{bmatrix} m_{LL} & m_{RL} \\ m_{LR} & m_{RR} \end{bmatrix} = \begin{bmatrix} 1.0 & 0.0 \\ 0.0 & 1.0 \end{bmatrix}$$
* **Mono Summed (Downmix):**
  Averages both channels equally to prevent missing instruments/vocals in single-earphone listening or single-speaker installations:
  $$\begin{bmatrix} m_{LL} & m_{RL} \\ m_{LR} & m_{RR} \end{bmatrix} = \begin{bmatrix} 0.5 & 0.5 \\ 0.5 & 0.5 \end{bmatrix} \quad \left(\text{or } -3\text{ dB constant-power: } \begin{bmatrix} 0.707 & 0.707 \\ 0.707 & 0.707 \end{bmatrix}\right)$$
* **Stereo Channel Inversion / Swap:**
  Swaps Left and Right channels for correcting flipped headphone cables or backwards chassis wiring:
  $$\begin{bmatrix} m_{LL} & m_{RL} \\ m_{LR} & m_{RR} \end{bmatrix} = \begin{bmatrix} 0.0 & 1.0 \\ 1.0 & 0.0 \end{bmatrix}$$
* **Phase Polarity Check:**
  Inverting the polarity of one channel ($m_{RR} = -1.0$) allows instant acoustic cancellation testing to verify whether stereo audio tracks or headphone drivers are wired in phase.
* **Mid/Side (M/S) Spatial Width Expansion:**
  Stereo input is converted to Mid ($M$) and Side ($S$) domains, scaled by a stereo width factor $w \in [0.0, 2.0]$, and matrixed back to Left/Right:
  $$M[n] = \frac{x_L[n] + x_R[n]}{2}, \quad S[n] = \frac{x_L[n] - x_R[n]}{2}$$
  $$y_L[n] = M[n] + w \cdot S[n] = \frac{1+w}{2} x_L[n] + \frac{1-w}{2} x_R[n]$$
  $$y_R[n] = M[n] - w \cdot S[n] = \frac{1-w}{2} x_L[n] + \frac{1+w}{2} x_R[n]$$
  - $w = 0.0$: Collapses completely to Mono.
  - $w = 1.0$: Unaltered native stereo soundstage.
  - $w > 1.0$: Widens acoustic stage width for narrow desktop monitor setups or IEMs.

#### 3. Audiophile Headphone Crossfeed (Chu Moy / Bauer Acoustic Simulation)
* **The Psychoacoustic Problem:**
  Loudspeakers radiate sound into a room where sound from the left speaker arrives at the left ear immediately, and reaches the right ear delayed by $\sim 250\text{ to }300\ \mu\text{s}$ (interaural time difference, ITD) with its high-frequency content attenuated by the acoustic shadowing of the human head (interaural level difference, ILD). Headphones completely isolate each ear, causing extreme stereo ping-pong fatigue on early stereo recordings (e.g., 1960s Beatles, jazz, classical) where drums are panned 100% Left and vocals 100% Right.
* **DSP Filter Implementation:**
  $$\begin{aligned}
  y_L[n] &= x_L[n] + H_{\text{shelf}}(z) \cdot x_R[n - D] \\
  y_R[n] &= x_R[n] + H_{\text{shelf}}(z) \cdot x_L[n - D]
  \end{aligned}$$
  - **Low-Pass Crossfeed Filter $H_{\text{shelf}}(z)$:** 1st-order IIR low-pass filter with corner frequency $f_c \approx 700\text{ Hz}$ and attenuation $-6.0\text{ dB}$.
  - **Interaural Delay Line $D$:** A circular delay buffer of $13\text{ samples}$ ($\approx 270\ \mu\text{s}$ at $48\text{ kHz}$) placed in Core 1 internal SRAM (requiring only 52 bytes).
  - Eliminates "in-the-head" listening fatigue and recreates an authentic, open out-of-head speaker presentation.

#### 4. WebHID Control Protocol
A dedicated HID command (`CMD_SET_MIXER = 0x13`) allows dynamic runtime configuration from the web interface:
```c
typedef enum {
    MIXER_MODE_STEREO = 0,
    MIXER_MODE_MONO,
    MIXER_MODE_SWAP,
    MIXER_MODE_WIDTH,
    MIXER_MODE_CROSSFEED,
    MIXER_MODE_CUSTOM_MATRIX
} mixer_mode_t;

typedef struct __attribute__((packed)) {
    uint8_t command_id;    // 0x13
    uint8_t mode;          // mixer_mode_t
    int8_t  balance;       // -100 (Full L) to +100 (Full R), 0 = Center
    uint8_t width_pct;     // 0% (Mono) to 200% (Ultra-Wide), 100% = 1.0
    uint8_t crossfeed_en;  // 0 = Off, 1 = Low (4.5dB), 2 = High (6.0dB)
    float   custom_matrix[2][2]; // [mLL, mRL], [mLR, mRR]
} hid_mixer_packet_t;
```

---

### 1.7 Lookahead True-Peak Limiter & Dynamic Soft-Clipper (`v0.2.0`)
Even with configurable preamp headroom, aggressive sub-bass shelves or transient peaks on hot master recordings can briefly exceed signed 24-bit full-scale (`8388607.0f`). A lightweight lookahead limiter replaces hard digital rail clipping with transparent peak protection:

* **Static Lookahead Buffer (Internal DRAM):**
  - Pre-allocated circular stereo buffer of `144` frames ($1.5\text{ ms}$ at `96 kHz` = `1,152` bytes; scales dynamically to `66` frames at `44.1 kHz` / `72` frames at `48 kHz`).
  - Zero dynamic allocation inside `audio_dsp_task` (respects Golden Rule 3).
* **Envelope & Gain-Reduction Math:**
  - Peak detector tracks instantaneous stereo magnitude $p[n] = \max(|x_L[n]|, |x_R[n]|)$ ahead of the delayed audio tap.
  - Ceiling threshold $T = 8388607.0 \times 10^{-0.5 / 20} \approx -0.5\text{ dBFS}$ (`7918595.0f`).
  - Fast attack ($\tau_{\text{atk}} \approx 0.4\text{ ms}$) smoothly pulls gain reduction $g[n] = \min(g[n-1], T / p[n])$, followed by musical release ($\tau_{\text{rel}} \approx 100\text{ ms}$).
  - Applies soft-knee $\tanh$-style safety saturation above $-0.2\text{ dBFS}$ immediately before `clamp_s24_to_s32()`, plus an atomic clipping/limiting telemetry flag pushed to the OLED VU indicator.

---

### 1.8 Fletcher-Munson Equal-Loudness Contour (`v0.2.0`)
Human ear sensitivity to sub-bass and upper treble rolls off sharply at low listening SPLs (ISO 226:2023 equal-loudness contours). Volume-coupled dynamic compensation preserves body and sparkle during late-night low-volume listening:

* **Volume-Coupled Parametric Offset:**
  - Computes compensation factor $c \in [0.0, 1.0]$ from master volume $V_{\%} \in [0, 100]$ relative to reference level $V_{\text{ref}} = 80\%$:
    $$c = \text{clamp}\left(\frac{V_{\text{ref}} - V_{\%}}{60}, \; 0.0, \; 1.0\right)$$
  - Modulates two dedicated shelf stages (trailing the user PEQ cascade) recalculated only when volume changes:
    * **Low-End Contour:** Low-Shelf @ $80\text{ Hz}$ ($Q = 0.65$), gain $+c \cdot G_{\text{max\_LF}}$ (`0.0` to `+4.0 dB` in *ISO Mild*, `+6.5 dB` in *ISO Full*).
    * **Air Presence Contour:** High-Shelf @ $10\text{ kHz}$ ($Q = 0.71$), gain $+c \cdot G_{\text{max\_HF}}$ (`0.0` to `+1.5 dB` in *ISO Mild*, `+2.5 dB` in *ISO Full*).
  - Automatically flattens to `0.0 dB` bypass whenever `ab_bypass == true` or `loudness_mode == LOUDNESS_OFF`.

---

### 1.9 ESP-DSP SIMD FIR Convolution Engine (`v0.2.0`)
For phase-linear minimum-phase target matching or room acoustic impulse correction beyond 10 IIR biquads:

* **Xtensa LX7 PIE SIMD Execution (`espressif/esp-dsp`):**
  - Registers `espressif/esp-dsp` in `main/idf_component.yml` and executes `dsps_fir_f32_aes3()` across Core 1's DSP loop cleanly after IIR PEQ and before Matrix/Limiter.
* **Sample-Rate Adaptive Tap Ceiling (Core 1 Budget Protection):**
  - **44.1 kHz & 48.0 kHz:** Up to **`256` taps per channel** (`L` & `R`, normalized float32).
  - **96.0 kHz:** Automatically caps/truncates active evaluation to **`128` taps per channel** (keeping FIR MAC load below `24.6M MACs/s` on Xtensa LX7 PIE SIMD so total Core 1 utilization stays well within real-time deadlines).
* **Persistence & Transport:**
  - Taps stored in dedicated NVS keys `fir_taps_l` / `fir_taps_r` with CRC32 and uploaded via chunked `WEBHID_CMD_UPLOAD_FIR_CHUNK (0x14)` from WebHID / Android Companion.

---

### 1.10 8-Slot Editable NVS v3 User Profiles Replacing Factory Presets (`v0.2.0`)
In `v0.1.0`, Slots `0..2` are hardcoded read-only factory presets (`Flat`, `Bass Boost`, `Harman`) and only Slot `3` (`User Custom`) is editable and persisted. In `v0.2.0`, **all hardcoded factory presets are removed** in favor of **8 fully editable, custom-named NVS user slots (`Slot 0..7`)**:

* **Clean Default Initialization:**
  - On first boot (or per-slot / full factory reset), **all 8 slots (`Slot 0..7`) initialize to Reference Flat (`0.0 dB`, 5 ISO bands, default names `"Slot 1"`..`"Slot 8"`)**.
  - Existing `v0.1.0` `user_cfg_v2` data automatically migrates into **`Slot 0`** on first `v0.2.0` boot.
* **24 KB NVS Partition Budget:**
  ```c
  #define PEQ_USER_SLOT_COUNT 8
  #define PEQ_NVS_MAGIC_V3    0x50455133UL // "PEQ3"

  typedef struct __attribute__((packed)) {
      uint32_t magic;
      uint16_t version;           // 3
      uint8_t  linked_stereo;
      uint8_t  loudness_mode;     // 0=Off, 1=Mild, 2=Full
      uint8_t  limiter_enabled;   // 0=Off, 1=Lookahead -0.5dBFS Limiter
      uint8_t  fir_enabled;       // 0=Off, 1=Active
      char     name[16];          // Custom slot label shown on OLED & WebHID/Android
      peq_profile_t left;
      peq_profile_t right;
      peq_mixer_config_t mixer;
      uint32_t crc32;
  } peq_nvs_slot_v3_t;            // ~504 bytes/slot (~4.0 KB total for all 8 slots in 24 KB NVS)
  ```
* **Physical & App Workflow:**
  - Hardware Button 1 / Button 2, OLED Main Menu (`2. Slot: <Custom Name>`), WebHID, and Android Companion all cycle/select any of `Slot 0..7` seamlessly, edit independently, rename via `WEBHID_CMD_RENAME_SLOT (0x0A)`, and commit explicitly to NVS (`slot_cfg_v3_0`..`slot_cfg_v3_7`).

---

## 2. On-Device OLED Display & Physical Controls

### 2.1 Mini Real-Time Frequency Response Curve on OLED (`v0.2.0`)
The **SSD1306 128x64 Monochrome OLED Display** has sufficient spatial resolution to render a compact, responsive frequency response visualizer:

```
  0,0 ┌────────────────────────────────────────────────────────┐ 127,0
      │ [USB DAC] 48.0k 24b                             V: 80% │ Header
   10 ├────────────────────────────────────────────────────────┤
      │  +6dB ┌───┐                                            │
      │   0dB ┤   └───\__________/\____________/───            │ 128x28 Curve
      │  -6dB └────────────────────────────────────            │ Canvas
      │       20Hz        1kHz                     20kHz       │
   48 ├────────────────────────────────────────────────────────┤
      │ EQ: Slot 1 [Flat]                                 PLAY │ Footer
  127 └────────────────────────────────────────────────────────┘ 127,63
```

#### Graph Rendering Algorithm:
1. Divide the 128 horizontal pixels into logarithmic frequency increments:
   $$f(x) = 20 \times \left(\frac{20000}{20}\right)^{\frac{x}{127}} \quad (x \in [0, 127])$$
2. For each horizontal pixel $x$, compute the complex transfer function $H(e^{j\omega})$ across all active biquads:
   $$H(e^{j\omega}) = \prod_{k=1}^{N} \frac{b_{0,k} + b_{1,k}e^{-j\omega} + b_{2,k}e^{-j2\omega}}{1 + a_{1,k}e^{-j\omega} + a_{2,k}e^{-j2\omega}}$$
   $$\text{Gain}_{\text{dB}}(x) = 20 \log_{10} |H(e^{j\omega})| + \text{Preamp}_{\text{dB}}$$
3. Map $\text{Gain}_{\text{dB}}$ onto the 28-pixel vertical canvas ($y \in [14, 42]$ for $\pm 12\text{ dB}$):
   $$y(x) = \text{clamp}\left(28 - \left(\frac{\text{Gain}_{\text{dB}}(x) + 12}{24} \times 28\right) + 14, \; 14, \; 42\right)$$
4. Cache the resulting 128-byte vertical offset array in Core 0 SRAM, recomputing only when an EQ profile update occurs. Rendering executes via fast vertical segment drawing into the display framebuffer.

---

### 2.2 Live Stereo Peak / RMS VU Meter (`v0.2.0`)
* In Core 1's `audio_dsp_task`: Compute post-EQ, pre-clamp peak and exponential-moving-average RMS levels over every 256-sample frame:
  $$\text{Peak}_L = \max_{i=0}^{N-1} |x_L[i]|, \quad \text{RMS}_L = \sqrt{\frac{1}{N}\sum_{i=0}^{N-1} x_L[i]^2}$$
* Pass two 8-bit dBFS-mapped metrics ($\text{Meter}_L$, $\text{Meter}_R$) plus a sticky limiter-active / clip indicator bit (`CLIP`) to Core 0 via atomic/lock-free variables.
* Render dual horizontal bars in the OLED body (`-48 dBFS` to `0 dBFS`) with a $300\text{ ms}$ floating peak hold cap.

---

### 2.3 Cycleable Playback Views & Main Menu DSP Controls (`v0.2.0`)
> *(Note: The original standalone rotary PEQ editor proposal was superseded and implemented in `v0.1.0` as Mode 3 On-Device Digit-by-Digit PEQ Editor in `ui/ui_oled.c` / `main.c`.)*

* **Cycleable Playback Body Views:**
  Users can cycle the `y=14..46` Playback screen body (persisted in RAM/NVS UI preference) across four layouts:
  1. **Classic HUD:** Track/Device status text + volume bar *(current `v0.1.0` layout)*.
  2. **128×28 Live EQ Curve:** Logarithmic frequency response graph with center `0 dB` reference line.
  3. **Dual Stereo VU Meter:** Horizontal Peak + RMS stereo bars with `300 ms` peak hold and `CLIP` / `LIM` flag.
  4. **Split Curve + Compact VU:** Left `96 px` EQ response curve alongside right dual vertical L/R peak bars.
* **Expanded Mode 2 Main Menu:**
  Expose on-device toggles without needing USB host tools:
  - `2. Slot:<Name>` (cycles all 8 editable DAC presets `Slot 1`–`Slot 8`)
  - `3. A/B:<Active/Bypass>` (instant preamp-normalized comparison)
  - `4. Crossfeed:<Off/Low/High>` & `5. Loudness:<Off/Mild/Full>`
  - `6. Limiter:<On/Off>` & `7. View:<Classic/Curve/VU/Split>`

---

### 2.4 OLED I2C Bus Auto-Probe & Hot-Plug Recovery (`v0.2.0`)
* **Dual Address Scanning:** Probe `0x3C` (`I2C_OLED_ADDR_DEFAULT`) and `0x3D` (`I2C_OLED_ADDR_ALT`, already defined in `board_pins.h`) during initialization.
* **Bus Lockup Protection:** If the OLED display is disconnected during operation, I2C transactions time out gracefully (10 ms timeout) without blocking Core 0 or causing audio dropouts on Core 1.
* **Auto-Reinitialization:** When an I2C ACK is redetected, re-send display power-on sequence and resume rendering automatically.

---

### 2.5 OLED Contrast Dimmer & Burn-In Screensaver (`v0.2.0`)
To prevent SSD1306 organic phosphor burn-in during multi-hour desktop DAC sessions:
* **60 s Idle Dimmer:** If no rotary, button, WebHID, or Android UI event occurs for `60 s`, issue SSD1306 contrast command `0x81` dropping contrast from `0xCF` to `0x1F` (`~15%` brightness).
* **Periodic Pixel Shift / 180 s Blank:** Apply a subtle $\pm 1\text{ px}$ alternating frame offset every `30 s` while active, and blank the OLED (`0xAE`) after `180 s` when audio playback is idle (`!webhid_peq_is_streaming()`). Any rotary/button/HID event restores full contrast instantaneously.

---

## 3. WebHID & Android Companion Client Tooling

> **Implemented (`v0.1.0` & `v0.2.0`):** Both [`tools/webhid_tuner/`](../../tools/webhid_tuner/) (Modular React + Vite Desktop Chromium PWA) and [`tools/android_companion/`](../../tools/android_companion/) (Native Kotlin + Jetpack Compose USB control companion app with pinch-to-Q Canvas, **Offline Preview Mode**, and local phone profile library) implement Sections **3.1–3.3** plus full `v0.2.0` controls for **DAC Presets (1–8)**, **True-Peak Limiter**, **Equal Loudness (ISO 226)**, chunked **FIR Impulse Filter** upload, **OLED Screen View**, **Auto-Mute on Silence**, and **OLED Auto-Dim & Sleep** (`0x0A–0x14`).

### 3.1 AutoEQ & Equalizer APO Profile Import / Export
The WebHID Tuner (`tools/webhid_tuner/`) and Android Companion (`tools/android_companion/`) interface directly with standard community parametric equalization formats:

#### Import Support:
* **Equalizer APO / Peace Format:** Parses standard multi-line parametric definitions:
  ```text
  Preamp: -4.5 dB
  Filter 1: ON PK Fc 105 Hz Gain 4.5 dB Q 1.41
  Filter 2: ON LSC Fc 80 Hz Gain 3.0 dB Q 0.71
  Filter 3: ON HSC Fc 10000 Hz Gain -2.0 dB Q 0.71
  ```
* **AutoEQ Database Profiles:** Direct import of Crinacle, Oratory1990, and Rtings headphone parametric profiles (`PK`, `LS/LSC`, `HS/HSC`, `HP/HPQ`, `LP/LPQ`, `NO/NOTCH`, `BP`).
* **Automatic Band Allocation:** Dynamically matches file band counts (`1` to `10` bands) and maps filter types into hardware registers.

#### Export Support:
* Export active profile to `.txt` (Equalizer APO syntax) or `.json` (full Dual-Mono + Matrix snapshot backup).

---

### 3.2 Progressive Web App (PWA) Offline Deployment
* **Service Worker (`sw.js`), Web App Manifest (`manifest.json`), and Icon (`icon.svg`):** Cache all browser assets for offline installable desktop use on Chromium browsers.
* **Zero Dependency Architecture:** Runs completely client-side with `0` external network requests. *(For mobile Android OTG tuning where Android Chrome lacks `navigator.hid`, use the Native Android Companion app in `tools/android_companion/`.)*

---

### 3.3 Real-Time Phase & Group Delay Visualization
* **Phase Response:**
  $$\phi(f) = \arg(H(e^{j\omega})) = \sum_{k=1}^{N} \arctan\left(\frac{\text{Im}(H_k)}{\text{Re}(H_k)}\right)$$
* **Group Delay:**
  $$\tau_g(f) = -\frac{d\phi(\omega)}{d\omega}$$
* **Toggleable Traces:** Secondary canvas traces in both WebHID and Android Companion visualize phase shift ($-180^\circ\text{ to }+180^\circ$) and group delay ($\text{ms}$).

---

## 4. Power Management & Intelligent Muting

### 4.1 USB Host Suspend / Standby Detection & Low-Power Sleep (`v0.2.0`)
* **TinyUSB Suspend Callback (`tud_suspend_cb`):**
  When host PC enters standby or USB port suspends:
  1. Assert PCM5102A hardware soft-mute (`GPIO 7 LOW`).
  2. Power down OLED panel (`0xAE` + charge pump off) and blank WS2812B LED.
  3. Request dynamic CPU frequency drop (`240 MHz -> 80 MHz` via `esp_pm_configure`) dropping quiescent draw from $\sim 85\text{ mA}$ down to $< 15\text{ mA}$.
* **Resume Callback (`tud_resume_cb`):**
  Restores `240 MHz` CPU frequency, wakes the SSD1306 panel, restores WS2812B loudness state, and releases hardware mute cleanly after DMA stabilizes.

---

### 4.2 Zero-Signal Automatic Hardware Soft-Mute (`v0.2.0`)
* **Configurable Digital Silence Detection:**
  If incoming PCM stream stays below $-110\text{ dBFS}$ (`abs(sample) <= 2` on 24-bit) or UAC stops streaming for the configured timeout (**default `10 s`**, configurable `0/5/10/30/60 s` where `0 = disabled`):
  - Ramp output smoothly to zero over `15 ms` and assert PCM5102A hardware soft-mute (`GPIO 7 LOW`), eliminating idle charge-pump/DAC micro-hiss.
* **1 ms Soft-Ramp Wake:**
  The instant non-zero PCM audio frames arrive on Core 1, release `GPIO 7 HIGH` and execute a fast `1 ms` fade-in ramp so playback starts click-free without clipping initial transients.

---

## 5. v0.2.0 Extended 64-Byte HID & Companion Protocol

### 5.1 Extended `webhid_cmd_t` Command Assignments (`0x0A–0x14`)
Building directly on [`usb/usb_hid.h`](../../main/usb/usb_hid.h) commands `0x01..0x09` and `0x13` while preserving full backwards compatibility with `0x4551` ("EQ") 64-byte packets:

| Command ID | Enum Symbol | Payload Layout (`64` Bytes) | Behavior |
| :---: | :--- | :--- | :--- |
| **`0x0A`** | `WEBHID_CMD_RENAME_SLOT` | `band_idx = slot (0..7)`, `reserved[0..15] = char name[16]` (NUL-terminated ASCII) | Updates active slot custom label on OLED/WebHID/Android |
| **`0x0B`** | `WEBHID_CMD_SET_DYNAMICS` | `band_idx = view`, `filter_type = loudness`, `enabled = (limiter | (fir << 1))` | Toggles True-Peak Limiter, Equal Loudness (ISO 226), FIR Impulse Filter, and OLED Screen View live |
| **`0x0C`** | `WEBHID_CMD_SET_POWER_CFG` | `freq_hz = auto_mute_sec`, `q_x100 = oled_dim_sec` | Configures Auto-Mute on Silence and OLED Auto-Dim & Sleep |
| **`0x0D`** | `WEBHID_CMD_FACTORY_RESET_SLOT` | `band_idx = slot (0..7, or 0xFF for all 8 slots)` | Re-seeds target slot(s) to default Reference Flat (`"Slot N"`) |
| **`0x14`** | `WEBHID_CMD_UPLOAD_FIR_CHUNK` | `band_idx = ch (1=L, 2=R)`, `filter_type = count`, `enabled = chunk_idx`, `freq_hz = total_taps`, `reserved[0..47] = 12 x float32 taps` | Streams 256/128-tap minimum-phase FIR impulse into volatile staging buffer |

*(Note: `CMD_GET_ACTIVE_PROFILE (0x01)` with `band_idx = 0xFD` returns the active 16-byte slot name, dynamics flags, FIR tap counts, and power config.)*

---

## Summary Matrix

| Enhancement Area | Component | Primary Benefit | Target Release | Status |
| :--- | :--- | :--- | :---: | :---: |
| **HPF / LPF / Notch / BPF** | `dsp/dsp_core.c` / WebHID / Android | Subsonic protection, noise reduction, and resonance removal | `v0.1.0` | **Implemented** |
| **24-Bit / 96 kHz Audio & 32-bit I2S** | `CMakeLists.txt` / `audio/audio_i2s.c` | 112 dB dynamic range and studio master resolution | `v0.1.0` | **Implemented** |
| **Tri-Rate (44.1k, 48k, 96k)** | TinyUSB / Core 1 Reclock | Bit-perfect CD & Hi-Res playback with zero OS resampling | `v0.1.0` | **Implemented** |
| **Dual-Mono L/R PEQ & NVS v2** | `dsp/dsp_core.c` / WebHID / Android | Independent channel balance and CRC32 persistence | `v0.1.0` | **Implemented** |
| **A/B Instant Toggle** | `dsp/dsp_core.c` / WebHID / Android | Instant preamp-normalized comparison against bypass | `v0.1.0` | **Implemented** |
| **Channel Matrix & Crossfeed** | `dsp/dsp_core.c` / WebHID / Android | Stereo widening, mono downmix, and Headphone Crossfeed (Chu Moy) | `v0.1.0` | **Implemented** |
| **AutoEQ / APO Import, PWA & Phase/GD** | `tools/webhid_tuner/` | One-click community headphone presets & complex curve plots | `v0.1.0` | **Implemented** |
| **Native Android Companion** | `tools/android_companion/` | Control-only mobile tuning alongside OS audio + local library | `v0.1.0` | **Implemented** |
| **True-Peak Limiter & Soft-Clipper** | `dsp/dsp_core.c` (1.5 ms lookahead) | -0.5 dBFS transient clip protection on heavy EQ boosts | `v0.2.0` | **Implemented** |
| **Equal Loudness (ISO 226)** | `dsp/dsp_core.c` / `audio/audio_i2s.c` | Volume-coupled ISO 226 bass/air compensation for low SPLs | `v0.2.0` | **Implemented** |
| **FIR Impulse Filter (`esp-dsp`)** | `dsp/dsp_core.c` (`esp-dsp`) | Adaptive 256-tap (48k) / 128-tap (96k) phase/room FIR engine | `v0.2.0` | **Implemented** |
| **8 Editable DAC Presets (`Slot 1–8`)** | `dsp/dsp_core.c` / `main.c` / Apps | Replaces hardcoded factory presets with 8 flat-seeded named slots | `v0.2.0` | **Implemented** |
| **Cycleable OLED Screen Views** | `ui/ui_oled.c` / `dsp/dsp_core.c` | `Classic`, `EQ Curve`, `VU Meters`, `Split` + expanded menu | `v0.2.0` | **Implemented** |
| **OLED Hot-Plug & Auto-Dim / Sleep** | `ui/ui_oled.c` (`0x3C`/`0x3D`) | Non-blocking I2C recovery, 60 s dim, pixel shift & 180 s sleep | `v0.2.0` | **Implemented** |
| **USB Suspend & Auto-Mute on Silence** | `usb/usb_hid.c` / `GPIO 7` | `<15 mA` USB suspend sleep & zero-signal analog silence mute | `v0.2.0` | **Implemented** |
