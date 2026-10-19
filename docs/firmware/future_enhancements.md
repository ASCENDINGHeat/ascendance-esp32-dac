# ESP32-S3 High-Fidelity USB DAC & DSP: Future Enhancements & Architectural Roadmap

This specification details the technical architectures, mathematical formulations, descriptor configurations, and implementation requirements for planned firmware and tooling enhancements.

> [!NOTE]
> This document serves as the implementation-ready blueprint for future development cycles. The features described herein are documented for design review and are not yet enabled in the active firmware build.

---

## Table of Contents
1. [DSP Audio Engine Enhancements](#1-dsp-audio-engine-enhancements)
   - [1.1 Extended Biquad Filter Topologies (RBJ Audio Cookbook)](#11-extended-biquad-filter-topologies-rbj-audio-cookbook)
   - [1.2 High-Resolution 24-Bit / 96 kHz Audio Pipeline](#12-high-resolution-24-bit--96-khz-audio-pipeline)
   - [1.3 Dual-Rate Bit-Perfect Streaming (44.1 kHz & 48.0 kHz)](#13-dual-rate-bit-perfect-streaming-441-khz--480-khz)
   - [1.4 Independent Left / Right Channel PEQ (Dual-Mono Tuning)](#14-independent-left--right-channel-peq-dual-mono-tuning)
   - [1.5 Instant A/B Auditioning Toggle](#15-instant-ab-auditioning-toggle)
   - [1.6 Audio Channel Matrix Mixer & Headphone Crossfeed](#16-audio-channel-matrix-mixer--headphone-crossfeed)
2. [On-Device OLED Display & Physical Controls](#2-on-device-oled-display--physical-controls)
   - [2.1 Mini Real-Time Frequency Response Curve on OLED](#21-mini-real-time-frequency-response-curve-on-oled)
   - [2.2 Live Stereo Peak / RMS VU Meter](#22-live-stereo-peak--rms-vu-meter)
   - [2.3 Standalone Rotary Encoder PEQ Navigation Menu](#23-standalone-rotary-encoder-peq-navigation-menu)
   - [2.4 OLED I2C Bus Auto-Probe & Hot-Plug Recovery](#24-oled-i2c-bus-auto-probe--hot-plug-recovery)
3. [WebHID GUI Tooling Enhancements](#3-webhid-gui-tooling-enhancements)
   - [3.1 AutoEQ & Equalizer APO Profile Import / Export](#31-autoeq--equalizer-apo-profile-import--export)
   - [3.2 Progressive Web App (PWA) Offline Deployment](#32-progressive-web-app-pwa-offline-deployment)
   - [3.3 Real-Time Phase & Group Delay Visualization](#33-real-time-phase--group-delay-visualization)
4. [Power Management & Intelligent Muting](#4-power-management--intelligent-muting)
   - [4.1 USB Host Suspend / Standby Detection & Low-Power Sleep](#41-usb-host-suspend--standby-detection--low-power-sleep)
   - [4.2 Zero-Signal Automatic Hardware Soft-Mute](#42-zero-signal-automatic-hardware-soft-mute)

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

## 2. On-Device OLED Display & Physical Controls

### 2.1 Mini Real-Time Frequency Response Curve on OLED
The **SSD1306 128x64 Monochrome OLED Display** has sufficient spatial resolution to render a compact, responsive frequency response visualizer:

```
  0,0 ┌────────────────────────────────────────────────────────┐ 127,0
      │ [USB DAC] 48.0k 16b                             V: 80% │ Header
   10 ├────────────────────────────────────────────────────────┤
      │  +6dB ┌───┐                                            │
      │   0dB ┤   └───\__________/\____________/───            │ 128x28 Curve
      │  -6dB └────────────────────────────────────            │ Canvas
      │       20Hz        1kHz                     20kHz       │
   48 ├────────────────────────────────────────────────────────┤
      │ EQ: Harman OE2018                                 PLAY │ Footer
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

### 2.2 Live Stereo Peak / RMS VU Meter
* In Core 1's `audio_dsp_task`: Compute peak and exponential-moving-average RMS levels over every 256-sample frame:
  $$\text{Peak}_L = \max_{i=0}^{N-1} |x_L[i]|, \quad \text{RMS}_L = \sqrt{\frac{1}{N}\sum_{i=0}^{N-1} x_L[i]^2}$$
* Pass two 8-bit metrics ($\text{Meter}_L$, $\text{Meter}_R$) to the UI task via an atomic variable.
* Render a dual horizontal bar meter in the OLED body during music playback with a $300\text{ ms}$ peak decay hold.

---

### 2.3 Standalone Rotary Encoder PEQ Navigation Menu
For operation away from a computer, the EC11 rotary encoder and push switch can manage basic hardware tuning:
* **Normal Mode:** Rotate adjusts Volume ($0\%\text{ to }100\%$); Short Click toggles Mute; Long Press cycles Audio Source.
* **Menu Entry (Triple Click SW):** Enters **Onboard PEQ Editor**:
  * **Level 1 (Band Select):** Rotate selects Band $1 \dots N$. OLED highlights selected band node.
  * **Level 2 (Param Select - Click):** Selects Gain ($\pm 12\text{ dB}$), Frequency ($20\text{ Hz}\dots 20\text{ kHz}$), or Q ($0.1\dots 10.0$).
  * **Level 3 (Adjust - Rotate):** Adjusts parameter in real-time with instant biquad recalculation.
  * **Exit & Save (Double Click):** Commits curve to NVS and returns to default playback HUD.

---

### 2.4 OLED I2C Bus Auto-Probe & Hot-Plug Recovery
* **Dual Address Scanning:** Probe `0x3C` (default) and `0x3D` (alternate solder jumper address) during initialization.
* **Bus Lockup Protection:** If the OLED display is disconnected during operation, I2C transactions time out gracefully (10 ms timeout) without blocking Core 0 or causing audio dropouts on Core 1.
* **Auto-Reinitialization:** When an I2C ACK is redetected, re-send display power-on sequence and resume rendering automatically.

---

## 3. WebHID GUI Tooling Enhancements

### 3.1 AutoEQ & Equalizer APO Profile Import / Export
The WebHID Tuner (`tools/webhid_tuner/index.html`) can interface directly with standard community parametric equalization formats:

#### Import Support:
* **Equalizer APO / Peace Format:** Parses standard multi-line parametric definitions:
  ```text
  Filter 1: ON PK Fc 105 Hz Gain 4.5 dB Q 1.41
  Filter 2: ON LSC Fc 80 Hz Gain 3.0 dB Q 0.71
  Filter 3: ON HSC Fc 10000 Hz Gain -2.0 dB Q 0.71
  ```
* **AutoEQ Database Profiles:** Direct import of Crinacle, Oratory1990, and Rtings headphone compensation targets from standard comma-separated or tab-separated text files.
* **Automatic Band Allocation:** Dynamically matches file band counts (1 to 10 bands) and maps filter types (Peaking, Low-Shelf, High-Shelf, HPF, LPF) directly into the hardware registers.

#### Export Support:
* Export current active profile to `.txt` (Equalizer APO syntax), `.json` (full WebHID snapshot backup), or shareable URL hash.

---

### 3.2 Progressive Web App (PWA) Offline Deployment
To allow instantaneous tuning without requiring internet access or browser file navigation:
* **Service Worker (`sw.js`):** Cache `index.html`, visualizer styles, and JavaScript modules.
* **Web App Manifest (`manifest.json`):** Enable "Install as Desktop / Android App" in Chrome and Edge.
* **Zero Dependency Architecture:** Runs completely client-side with 0 external network requests, functioning in airplane mode, outdoors, or in air-gapped studio environments.

---

### 3.3 Real-Time Phase & Group Delay Visualization
In addition to the magnitude response curve ($|H(f)|$ in dB), audiophiles and acoustic engineers benefit from observing phase shift:
* **Phase Response:**
  $$\phi(f) = \arg(H(e^{j\omega})) = \sum_{k=1}^{N} \arctan\left(\frac{\text{Im}(H_k)}{\text{Re}(H_k)}\right)$$
* **Group Delay:**
  $$\tau_g(f) = -\frac{d\phi(\omega)}{d\omega}$$
* **Toggleable Traces:** Add secondary color traces on the HTML5 Canvas to visualize phase shift and group delay in milliseconds, allowing users to minimize phase smearing in bass transition regions.

---

## 4. Power Management & Intelligent Muting

### 4.1 USB Host Suspend / Standby Detection & Low-Power Sleep
* **TinyUSB Suspend Callback:**
  When host PC enters standby or USB port suspends:
  ```c
  void tud_suspend_cb(bool remote_wakeup_en) {
      audio_pipeline_set_mute(true); // Assert hardware soft-mute (GPIO 7 LOW)
      // Throttle CPU frequency from 240 MHz to 80 MHz to reduce power draw
      set_cpu_freq_mhz(80);
      oled_display_sleep(); // Turn off SSD1306 charge pump and OLED panel
  }
  ```
* **Resume Callback:**
  Restores 240 MHz CPU frequency, wakes display panel, and un-mutes audio when the host PC wakes up.
* **Idle Power Consumption:** Drops quiescent power consumption from $\sim 85\text{ mA}$ down to $<15\text{ mA}$.

---

### 4.2 Zero-Signal Automatic Hardware Soft-Mute
* **Digital Silence Detection:**
  If incoming PCM stream contains 0 amplitude (or host stops queuing audio frames) for $> 10\text{ seconds}$:
  - Assert PCM5102A hardware soft-mute (`GPIO 7 LOW`).
  - This grounds the analog charge pump and DAC output stage, eliminating any micro-hiss or residual line noise when idle.
* **Instant Un-Mute:**
  The instant a non-zero PCM audio packet is received in `tud_audio_rx_done_cb()`, pull `GPIO 7 HIGH` within 1 ms for seamless playback start.

---

## Summary Matrix

| Enhancement Area | Component | Primary Benefit | Implementation Effort |
| :--- | :--- | :--- | :---: |
| **HPF / LPF / Notch / BPF** | `peq_dsp.c` / WebHID | Subsonic protection, noise reduction, and resonance removal | Low |
| **24-Bit / 96 kHz Audio** | `CMakeLists.txt` / I2S | 112 dB dynamic range and studio master resolution | Medium |
| **Dual-Rate (44.1k & 48k)** | TinyUSB / I2S | Bit-perfect CD playback with zero OS resampling | Medium |
| **Dual-Mono L/R PEQ** | `peq_dsp.c` / WebHID | Independent channel balance and asymmetric calibration | Medium |
| **A/B Instant Toggle** | `peq_dsp.c` / WebHID | Instant click-free comparison against flat bypass | Low |
| **Channel Matrix & Crossfeed** | `peq_dsp.c` / WebHID | Stereo widening, mono downmix, and Chu Moy headphone crossfeed | Low |
| **OLED Live EQ Curve** | `oled_display.c` | Real-time response curve rendering on hardware screen | Medium |
| **OLED Stereo VU Meter** | `oled_display.c` / DSP | Live visual feedback of stereo output levels | Low |
| **AutoEQ / APO Import** | `tools/webhid_tuner` | One-click import of Crinacle / Oratory1990 headphone presets | Low |
| **PWA Offline Support** | `tools/webhid_tuner` | Native standalone app installation on desktop and mobile | Low |
| **USB Suspend & Auto-Mute** | `webhid_peq.c` / GPIO | Zero idle hiss and $<15\text{ mA}$ standby power consumption | Low |
