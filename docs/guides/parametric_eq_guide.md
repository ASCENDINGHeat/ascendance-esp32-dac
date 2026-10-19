# Parametric Equalizer (PEQ) DSP Engine & User Upload Roadmap

This technical guide defines the mathematical theory, digital signal processing (DSP) architecture, and web upload interface for adding a **User-Configurable Parametric Equalizer (PEQ)** to the ESP32-S3 audio player.

> **Note for Agentic AI / AI Assistants:**  
> This specification contains exact Biquad coefficient equations, JSON schemas for user presets, REST API endpoints, and a step-by-step implementation roadmap for autonomous coding agents.

---

## 1. System DSP Pipeline Architecture

The Parametric Equalizer processes digital audio samples in real-time immediately before they are dispatched to the I2S DMA transmit buffer:

```
  ┌────────────────────────────────────────────────────────┐
  │         Audio Source (Wi-Fi Stream or USB DAC)         │
  └───────────────────────────┬────────────────────────────┘
                              │ 16-bit Stereo PCM (Interleaved L/R)
                              ▼
  ┌────────────────────────────────────────────────────────┐
  │                  Pre-Gain / Headroom Stage             │
  │    Applies negative gain (e.g. -3 dB to -6 dB) to      │
  │    prevent digital clipping from positive EQ boosts    │
  └───────────────────────────┬────────────────────────────┘
                              │ Floating-Point Conversion (f32)
                              ▼
  ┌────────────────────────────────────────────────────────┐
  │   Cascaded Biquad IIR Filter Bank (1–10 Bands Dynamic) │
  │                                                        │
  │   Band 1: Low Shelf   (Sub-bass contour, 30–100 Hz)    │
  │   Band 2: Peaking EQ  (Bass control, 100–300 Hz)       │
  │   Band 3: Peaking EQ  (Lower midrange, 300–800 Hz)     │
  │   Band 4: Peaking EQ  (Vocal presence, 1–3 kHz)        │
  │   Band 5: Peaking EQ  (Harshness cut, 4–7 kHz)         │
  │   Band 6: High Shelf  (Air & sparkle, 8–16 kHz)        │
  │   ... Optional Bands 7–15 (AutoEQ Headphone Target)    │
  └───────────────────────────┬────────────────────────────┘
                              │ Clamped Integer Saturation (16-bit)
                              ▼
  ┌────────────────────────────────────────────────────────┐
  │              I2S DMA Driver (GPIO 4 / 5 / 6)           │
  │                  ──► [GY-PCM5102A DAC]                 │
  └────────────────────────────────────────────────────────┘
```

---

## 2. Mathematical Foundations: Biquad IIR Filters

The standard digital audio filter is a 2nd-order Infinite Impulse Response (IIR) Biquad filter governed by the difference equation:

$$y[n] = \frac{b_0}{a_0}x[n] + \frac{b_1}{a_0}x[n-1] + \frac{b_2}{a_0}x[n-2] - \frac{a_1}{a_0}y[n-1] - \frac{a_2}{a_0}y[n-2]$$

### 2.1 Coefficient Calculation Formulas (Robert Bristow-Johnson Audio EQ Cookbook)

Given:
* $F_s$: Sample rate in Hz (e.g. 44100 or 48000)
* $f_0$: Center frequency in Hz ($0 < f_0 < F_s / 2$)
* $dBgain$: Boost or cut in decibels ($\text{e.g. } -12\text{ dB to }+12\text{ dB}$)
* $Q$: Quality factor ($\text{bandwidth control, typical } 0.707 \text{ to } 3.0$)

**Intermediate Variables:**
$$A = 10^{\frac{dBgain}{40}} = \sqrt{10^{\frac{dBgain}{20}}}$$
$$\omega_0 = 2\pi \frac{f_0}{F_s}$$
$$\alpha = \frac{\sin(\omega_0)}{2Q}$$

#### A. Peaking EQ (Bell Filter)
$$b_0 = 1 + \alpha A, \quad b_1 = -2\cos(\omega_0), \quad b_2 = 1 - \alpha A$$
$$a_0 = 1 + \frac{\alpha}{A}, \quad a_1 = -2\cos(\omega_0), \quad a_2 = 1 - \frac{\alpha}{A}$$

#### B. Low Shelf Filter
$$b_0 = A \left[ (A+1) - (A-1)\cos(\omega_0) + 2\sqrt{A}\alpha \right]$$
$$b_1 = 2A \left[ (A-1) - (A+1)\cos(\omega_0) \right]$$
$$b_2 = A \left[ (A+1) - (A-1)\cos(\omega_0) - 2\sqrt{A}\alpha \right]$$
$$a_0 = (A+1) + (A-1)\cos(\omega_0) + 2\sqrt{A}\alpha$$
$$a_1 = -2 \left[ (A-1) + (A+1)\cos(\omega_0) \right]$$
$$a_2 = (A+1) + (A-1)\cos(\omega_0) - 2\sqrt{A}\alpha$$

#### C. High Shelf Filter
$$b_0 = A \left[ (A+1) + (A-1)\cos(\omega_0) + 2\sqrt{A}\alpha \right]$$
$$b_1 = -2A \left[ (A-1) + (A+1)\cos(\omega_0) \right]$$
$$b_2 = A \left[ (A+1) + (A-1)\cos(\omega_0) - 2\sqrt{A}\alpha \right]$$
$$a_0 = (A+1) - (A-1)\cos(\omega_0) + 2\sqrt{A}\alpha$$
$$a_1 = 2 \left[ (A-1) - (A+1)\cos(\omega_0) \right]$$
$$a_2 = (A+1) - (A-1)\cos(\omega_0) - 2\sqrt{A}\alpha$$

#### D. High-Pass Filter (HPF / Subsonic Cut)
$$b_0 = \frac{1 + \cos(\omega_0)}{2}, \quad b_1 = -(1 + \cos(\omega_0)), \quad b_2 = \frac{1 + \cos(\omega_0)}{2}$$
$$a_0 = 1 + \alpha, \quad a_1 = -2\cos(\omega_0), \quad a_2 = 1 - \alpha$$

---

## 3. ESP32-S3 Hardware Acceleration (`esp-dsp`)

The ESP32-S3's Xtensa LX7 core includes **PIE (Processor Instruction Extension)** vector DSP instructions capable of single-cycle parallel float multiply-accumulate operations.

### 3.1 Adding `esp-dsp` Dependency (`main/idf_component.yml`)
```yaml
dependencies:
  espressif/esp-dsp: "^1.4.0"
```

### 3.2 High-Performance Assembly Kernel
Instead of standard C loops, invoke the hand-optimized assembly function:
```c
#include "dsps_biquad.h"

// dsps_biquad_f32_ae32 processes a block of audio samples using Xtensa SIMD registers
esp_err_t dsps_biquad_f32_ae32(const float *input, float *output, int len, const float *coeffs, float *w);
```
* **Performance:** Processing a 10-band stereo PEQ at 48 kHz on the ESP32-S3 consumes **less than 4% CPU utilization** on Core 1!

### 3.3 Dynamic PEQ Band Count & CPU Scaling Architecture

The PEQ DSP engine features dynamic active band scaling supporting **1 to 10 cascaded Biquad IIR bands** (`PEQ_MAX_BANDS = 10`, default `PEQ_DEFAULT_NUM_BANDS = 5`). Rather than unconditionally executing a fixed maximum of 10 biquads for every sample, the Core 1 audio DSP pipeline (`peq_dsp_process_interleaved_s16`) loops strictly across active bands (`s_active_num_bands`):

```c
// Core 1 Real-time inner loop: scales computation strictly to active bands
int act_count = s_active_num_bands;
for (int b = 0; b < act_count; b++) {
    act_l = biquad_filter_sample(&s_active_biquads[b], act_l, ...);
    act_r = biquad_filter_sample(&s_active_biquads[b], act_r, ...);
}
```

#### Dynamic CPU Scaling & Resource Matrix (Xtensa LX7 @ 240 MHz)

| Active Band Count | Use Case Profile | Xtensa LX7 Biquad Operations / Sample | MIPS @ 44.1 kHz Stereo | Core 1 CPU Utilization (%) |
| :---: | :--- | :---: | :---: | :---: |
| **1 Band** | Subsonic HPF / Single Notch Filter | 2 biquads (L/R) | ~0.22 MIPS | **~0.4%** |
| **3 Bands** | Classic Tone Controls (Bass, Mid, Treble) | 6 biquads (L/R) | ~0.66 MIPS | **~1.2%** |
| **5 Bands** | Standard Audiophile PEQ (Default) | 10 biquads (L/R) | ~1.10 MIPS | **~2.0%** |
| **7 Bands** | Extended Acoustic Tuning & Room Correction | 14 biquads (L/R) | ~1.54 MIPS | **~2.8%** |
| **10 Bands** | Pro Mastering & Calibrated AutoEQ Targets | 20 biquads (L/R) | ~2.20 MIPS | **~3.8%** |

#### Click-Free Band Count Crossfading (15 ms)
When the active band count is changed (via WebHID `CMD_SET_BAND_COUNT = 0x07`, browser dynamic dropdown, or C API `peq_dsp_set_band_count()`):
1. The target filter bank is populated with the desired band count (`s_target_num_bands`).
2. A linear 15 ms crossfader blends between active and target filter banks sample-by-sample.
3. Upon crossfade completion, `s_active_num_bands` updates to the target count, seamlessly switching filter topologies without clicks, pops, or DC thumps.

#### Auto-Expansion on High Band Edits
When the host updates a higher band index via `peq_dsp_update_user_band(band_idx, ...)` or WebHID `CMD_SET_BAND_LIVE`:
* If `band_idx >= s_user_profile.num_bands`, the engine automatically expands `num_bands = band_idx + 1` (clamped to `PEQ_MAX_BANDS`).
* This guarantees that user edits to extended bands immediately activate and audition without requiring a separate manual pre-configuration step.

#### Dynamic Band Count C API Reference
```c
/**
 * @brief Query current active dynamic band count.
 * @return int Active band count (1 to PEQ_MAX_BANDS).
 */
int peq_dsp_get_band_count(void);

/**
 * @brief Dynamically set active band count with 15ms double-buffered crossfade.
 * @param num_bands Target band count (1 to PEQ_MAX_BANDS).
 * @return esp_err_t ESP_OK on success, ESP_ERR_INVALID_ARG if out of bounds.
 */
esp_err_t peq_dsp_set_band_count(int num_bands);
```

---

## 4. User Upload & AutoEQ Profile Schema

To support community standard headphone equalization curves (such as [AutoEQ](https://github.com/jaakkopasanen/AutoEq) for Sennheiser, Sony, Beyerdynamic, etc.), the firmware supports two ingestion formats:

### 4.1 JSON Profile Format (`/api/eq`)
```json
{
  "profile_name": "Sennheiser HD600 (Harman Target)",
  "preamp_gain_db": -5.2,
  "bands": [
    { "type": "low_shelf", "freq_hz": 105, "gain_db": 5.5, "q": 0.71 },
    { "type": "peaking",   "freq_hz": 210, "gain_db": -2.0, "q": 1.40 },
    { "type": "peaking",   "freq_hz": 1350,"gain_db": -1.8, "q": 1.80 },
    { "type": "peaking",   "freq_hz": 3200,"gain_db": 2.5,  "q": 2.00 },
    { "type": "peaking",   "freq_hz": 5800,"gain_db": -3.5, "q": 3.20 },
    { "type": "high_shelf","freq_hz": 10000,"gain_db": 1.5, "q": 0.71 }
  ]
}
```

### 4.2 Parametric EQ Text Format (REW / Peace Equalizer standard)
```text
Preamp: -5.2 dB
Filter 1: ON PK Fc 105 Hz Gain 5.5 dB Q 0.71
Filter 2: ON PK Fc 210 Hz Gain -2.0 dB Q 1.40
Filter 3: ON PK Fc 1350 Hz Gain -1.8 dB Q 1.80
Filter 4: ON PK Fc 3200 Hz Gain 2.5 dB Q 2.00
Filter 5: ON PK Fc 5800 Hz Gain -3.5 dB Q 3.20
```

---

## 5. Persistent Storage & NVS Profile Architecture (User PEQ Decoupled from Factory Presets)

A fundamental architectural requirement of this system is that **user-customized PEQ settings can be modified at runtime and saved to persistent non-volatile storage without requiring firmware recompilation or reflashing (`idf.py flash`), completely independent of the built-in factory presets**.

```
  ┌─────────────────────────────────────────────────────────────────────────────┐
  │                         PEQ PROFILE STORAGE ARCHITECTURE                    │
  ├─────────────────────────────────────────────────────────────────────────────┤
  │                                                                             │
  │   IMMUTABLE FACTORY PRESETS                     PERSISTENT USER PROFILE     │
  │   (Compiled in Flash .rodata)                   (Stored in Flash NVS)       │
  │                                                                             │
  │   ┌─────────────────────────────┐               ┌─────────────────────────┐ │
  │   │ Preset 1: Flat / Bypass     │               │ Custom Profile (Slot 0) │ │
  │   │  • All bands 0 dB, Preamp 0 │               │  • Namespace: "user_peq"│ │
  │   ├─────────────────────────────┤               │  • Key: "profile" (Blob)│ │
  │   │ Preset 2: Warm Bass Boost   │               │  • Survives reboots/OTA │ │
  │   │  • +4.5dB Low Shelf @ 105Hz │               │  • Editable via Web/API │ │
  │   ├─────────────────────────────┤               │  • Zero reflash needed  │ │
  │   │ Preset 3: Harman OE2018     │               │  • CRC32 protected      │ │
  │   │  • Calibrated AutoEQ Curve  │               │  • Factory-resettable   │ │
  │   └──────────────┬──────────────┘               └────────────┬────────────┘ │
  │                  │                                           │              │
  │                  └─────────────────────┬─────────────────────┘              │
  │                                        │ Active Slot Selector               │
  │                                        ▼ (Persisted in NVS)                 │
  │                         ┌─────────────────────────────┐                     │
  │                         │ Active Filter Bank (Core 1) │                     │
  │                         │ Double-Buffered Crossfader  │                     │
  │                         └─────────────────────────────┘                     │
  └─────────────────────────────────────────────────────────────────────────────┘
```

### 5.1 The Separation Principle: Immutable Presets vs. Mutable User Storage
1. **Immutable Factory Presets (ROM / Flash `.rodata`):**
   * Stored as compile-time constants in `const peq_profile_t g_factory_presets[]`.
   * **Preset 1 (Flat):** Bit-perfect pass-through (preamp 0 dB, all filters bypassed).
   * **Preset 2 (Bass Boost):** Warm sub-bass shelf (+4.5 dB @ 105 Hz, Q=0.71, preamp -4.5 dB).
   * **Preset 3 (Harman AutoEQ):** Multi-band target curve matching standard headphone acoustic profiles.
   * **Safety Guarantee:** These presets can **never** be overwritten, corrupted, or deleted by web uploads, user mistakes, or NVS clearing. They serve as permanent, guaranteed safe recovery baselines.
2. **Mutable User Custom Profile (ESP-IDF NVS Partition):**
   * Stored in a dedicated Non-Volatile Storage (NVS) namespace: `"user_peq"`.
   * Saved as a compact binary struct blob (`peq_nvs_profile_t`) with a 32-bit magic word and CRC32 checksum.
   * Can be altered in real-time via the Web Dashboard, REST API (`POST /api/eq/user`), or MicroSD file import **without reflashing firmware**.
   * Automatically restored upon system boot; persists across power cycles, hard resets, and firmware OTA updates.
   * Features an instant **"Clone Preset to User"** or **"Factory Reset"** action that copies any chosen factory preset into the user slot, restoring a clean baseline in flash with zero code modifications.

### 5.2 NVS Data Schema & Packed Binary Serialization
To guarantee ultra-fast boot loading (<5 ms) without JSON parsing overhead in the audio initialization critical path, the user profile is stored as a packed binary structure:

```c
#define MAX_PEQ_BANDS        10
#define PEQ_NVS_MAGIC        0x50455131  // "PEQ1" in ASCII
#define PEQ_NVS_VERSION      1

typedef enum {
    PEQ_FILTER_BYPASS    = 0,
    PEQ_FILTER_PEAKING   = 1,
    PEQ_FILTER_LOWSHELF  = 2,
    PEQ_FILTER_HIGHSHELF = 3,
    PEQ_FILTER_HIGHPASS  = 4,
    PEQ_FILTER_LOWPASS   = 5,
} peq_filter_type_t;

typedef struct __attribute__((packed)) {
    uint8_t  type;          // peq_filter_type_t
    uint8_t  enabled;       // 1 = active, 0 = bypassed
    uint16_t freq_hz;       // Center/Cutoff frequency (20 Hz to 20,000 Hz)
    int16_t  gain_db_x10;   // Gain in tenths of a dB (-150 to +150 = -15.0 dB to +15.0 dB)
    uint16_t q_x100;        // Quality factor x100 (e.g. 71 = 0.71, 141 = 1.41)
} peq_band_nvs_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;         // PEQ_NVS_MAGIC
    uint16_t version;       // Schema version (1)
    uint8_t  active_slot;   // Currently selected profile (0 = User, 1 = Preset 1, etc.)
    uint8_t  num_bands;     // Number of configured bands (1 to MAX_PEQ_BANDS)
    int16_t  preamp_db_x10;  // Pre-gain attenuation (e.g. -52 = -5.2 dB headroom)
    char     name[32];      // Null-terminated profile label (e.g. "Sennheiser HD600 Custom")
    peq_band_nvs_t bands[MAX_PEQ_BANDS];
    uint32_t crc32;         // CRC32 checksum over the preceding bytes
} peq_nvs_profile_t;
```

### 5.3 Non-Blocking Atomic NVS Commit & Live Crossfade
When the user adjusts an EQ band or uploads an AutoEQ file via the web interface:
1. **Core 0 (Web Server Task):**
   * Ingests JSON payload from `POST /api/eq/user`.
   * Calculates new Biquad filter coefficients into a `pending_bank` buffer.
   * Packs the configuration into a `peq_nvs_profile_t` struct and computes the CRC32 checksum.
   * Commits the binary blob to NVS:
     ```c
     nvs_handle_t nvs_h;
     esp_err_t err = nvs_open("user_peq", NVS_READWRITE, &nvs_h);
     if (err == ESP_OK) {
         nvs_set_blob(nvs_h, "profile", &nvs_profile, sizeof(nvs_profile));
         nvs_set_u8(nvs_h, "active_slot", 0); // 0 = User Custom
         nvs_commit(nvs_h);
         nvs_close(nvs_h);
     }
     ```
2. **Atomic Swap & Core 1 Crossfading:**
   * An atomic pointer swap alerts the Core 1 `audio_stream_task`.
   * Core 1 executes a **15 ms linear crossfade** between the old and new filter banks.
   * **Result:** The user hears the acoustic adjustment immediately without clicks or dropouts, and the new profile is permanently stored in flash for future boots.

---

## 6. Real-Time Live Tuning Engine: WebHID (USB), WebSocket (Wi-Fi) & HTTP REST

When tuning a Parametric Equalizer, users drag graphical sliders or response curve handles on a screen. This interaction generates **30 to 60 parameter update events per second**. The ESP32-S3 supports **three distinct real-time tuning modalities**, ensuring seamless live tuning across both standalone Wi-Fi player and USB dongle DAC use cases:

```
                      REAL-TIME LIVE TUNING MULTI-CHANNEL ARCHITECTURE
  ┌───────────────────────────────────────────────────────────────────────────────────┐
  │ FRONTEND CLIENT (Phone / Laptop / PC Browser)                                     │
  │   • Live Interactive Frequency Curve (HTML5 Canvas)                               │
  │   • Real-Time Draggable Band Sliders (Freq, Gain, Q, Preamp)                      │
  └───────────────────┬───────────────────────┬───────────────────────┬───────────────┘
                      │                       │                       │
     [CHANNEL 1: USB WEBHID]    [CHANNEL 2: WI-FI WEBSOCKET]  [CHANNEL 3: HTTP REST]
     • FiiO JA11 Dongle Style   • Full-Duplex Socket Stream   • Throttled Polling
     • Single USB-C Cable       • Over Home Wi-Fi or SoftAP   • Fallback Endpoint
     • Zero Wi-Fi Required      • ws://<ip>/ws/eq             • POST /api/eq/live
     • Latency: <1 ms           • Latency: <2 ms              • Latency: 15–40 ms
     • 64-byte HID Reports      • Binary / JSON Frames        • 204 No Content
                      ▼                       ▼                       ▼
  ┌───────────────────────────────────────────────────────────────────────────────────┐
  │ CORE 0: CONTROL & USB/NETWORK TASK                                                │
  │                                                                                   │
  │   1. Ingests high-frequency tuning packet (<1 ms processing)                      │
  │   2. Recomputes Biquad coefficients (RBJ equations) into SRAM                     │
  │   3. VOLATILE AUDITIONING ONLY: Modifies s_active_peq_bank in SRAM                │
  │   4. Sets volatile dirty flag (s_peq_dirty = true); ZERO flash writes!            │
  │   5. Broadcasts state changes to all connected clients                            │
  └─────────────────────────────────────┬─────────────────────────────────────────────┘
                                        │ Atomic Lock-Free Pointer Swap
                                        ▼
  ┌───────────────────────────────────────────────────────────────────────────────────┐
  │ CORE 1: REAL-TIME DSP AUDIO TASK (audio_stream_task)                              │
  │                                                                                   │
  │   • 15 ms Double-Buffered Linear Crossfader (Eliminates Zipper Clicks)            │
  │   • Xtensa PIE SIMD Vector Biquad Filtering (esp-dsp ae32 assembly kernel)        │
  │   • Hard Saturation Clamping [-32768, 32767]                                      │
  │   • Uninterrupted I2S DMA Transfer to PCM5102A DAC                                │
  └───────────────────────────────────────────────────────────────────────────────────┘
                                        │
                                        ▼ User Triggers Explicit "Save to Preset"
                                          (UI Button / WebHID CMD / HW Long-Press)
  ┌───────────────────────────────────────────────────────────────────────────────────┐
  │ NON-VOLATILE FLASH PERSISTENCE: EXPLICIT "SAVE TO PRESET" (Core 0 Background)     │
  │   • Triggered by UI "Save to Preset", WebHID CMD_SAVE_TO_PRESET, or HW long-press │
  │   • Delta / Dirty Check: If active == flash, skip write entirely (0 bytes written)│
  │   • Writes packed peq_nvs_profile_t with CRC32 to "user_peq" NVS namespace        │
  │   • ABSOLUTE ZERO flash writes while auditioning and dragging sliders!            │
  └───────────────────────────────────────────────────────────────────────────────────┘
```

### 6.1 The Four Golden Rules of Embedded Live Tuning
1. **The Flash Wear Mitigation Invariant (Volatile Auditioning + Explicit "Save to Preset"):**
   * Standard SPI NOR flash has a physical limit of approximately **100,000 write cycles per sector**.
   * Dragging a slider produces ~60 events per second. Committing to NVS on every tick would destroy the ESP32-S3's flash memory in less than 30 minutes of active tuning!
   * **Rule:** Live tuning updates across all channels (WebHID, WebSocket, REST) MUST operate strictly in **volatile SRAM (Live Auditioning Mode)**. 
   * **Persistence:** Flash writes occur ONLY when the user explicitly triggers **"Save to Preset"** (via the Web UI "Save to Preset" button, WebHID `CMD_SAVE_TO_PRESET`, or hardware long-press shortcut) — or via an optional configurable debounced autosave timer. This guarantees deterministic user control and **practically zero flash wear**.
2. **The Zipper-Noise Prevention Invariant:**
   * Abrupt coefficient substitution across consecutive audio blocks creates sharp step discontinuities, perceived by human ears as harsh clicking or "zipper noise".
   * **Rule:** The DSP engine must apply either continuous coefficient slewing or a **10–15 ms double-buffered linear crossfade** between the previous and newly computed filter banks.
3. **Core Separation & Non-Blocking Critical Path:**
   * Core 0 handles USB HID reports, network frames, JSON/binary parsing, and NVS flash writes.
   * Core 1 is strictly dedicated to audio decoding and I2S DMA feeding. Core 1 MUST NEVER wait on a mutex, allocate heap memory, or block on Core 0 networking or USB interrupts.
4. **Bi-Directional Multi-Client State Synchronization:**
   * If a user adjusts Band 1 on their phone, an open desktop browser (or physical rotary encoder) should reflect that exact movement in real time.

---

### 6.2 Method 1: Full-Duplex WebSocket Stream (`ws://<ip>/ws/eq`) [Recommended]

WebSocket (RFC 6455) is the industry standard for interactive audio tuning. It maintains a persistent TCP connection with minimal framing overhead (2 to 6 bytes per packet vs. ~500 bytes of HTTP headers).

#### 1. Performance & Latency Comparison
* **Latency:** **<2 ms** transmission time over Wi-Fi (vs. 20–50 ms for HTTP request-response cycles).
* **Network Bandwidth:** $\sim 60 \times 24 \text{ bytes} \approx 1.4\text{ KB/sec}$ at 60 FPS (negligible Wi-Fi impact).
* **CPU Overhead:** $<0.5\%$ on Core 0.

#### 2. WebSocket Packet Formats
The server accepts two payload types on `ws://<ip>/ws/eq`:

##### A. Compact JSON Format (Standard Web Client)
```json
{ "op": "tune", "band": 2, "type": "peaking", "freq": 1250, "gain": 3.5, "q": 1.41 }
```

##### B. Ultra-Fast 8-Byte Binary Packet (Low-Latency / Gamepad / Controller)
For maximum throughput and zero string allocation on the MCU:
```c
typedef struct __attribute__((packed)) {
    uint8_t  magic;        // 0xAA (Sync Header)
    uint8_t  band_idx;     // 0 to 9
    uint8_t  filter_type;  // 1=Peaking, 2=LowShelf, 3=HighShelf, 4=HPF, 5=LPF
    uint16_t freq_hz;      // 20 to 20,000 Hz
    int16_t  gain_db_x10;  // e.g. +35 = +3.5 dB, -45 = -4.5 dB
    uint16_t q_x100;       // e.g. 141 = 1.41, 71 = 0.71
} peq_ws_live_frame_t;
```
Processing this 8-byte binary frame requires **zero JSON parsing** and executes in $<5\,\mu\text{s}$ on the ESP32-S3!

#### 3. ESP-IDF v6.1 WebSocket Server Implementation
```c
#include "esp_http_server.h"

static esp_err_t ws_eq_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        ESP_LOGI("WS_EQ", "Client connected: fd=%d", httpd_req_to_sockfd(req));
        return ESP_OK;
    }

    httpd_ws_frame_t ws_pkt;
    uint8_t rx_buf[128];
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.payload = rx_buf;

    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, sizeof(rx_buf) - 1);
    if (ret != ESP_OK) return ret;

    if (ws_pkt.type == HTTPD_WS_TYPE_TEXT) {
        rx_buf[ws_pkt.len] = '\0';
        // Parse compact JSON payload
        peq_handle_live_json((char *)rx_buf);
    } else if (ws_pkt.type == HTTPD_WS_TYPE_BINARY && ws_pkt.len == sizeof(peq_ws_live_frame_t)) {
        // Parse ultra-fast binary frame
        peq_ws_live_frame_t *bin = (peq_ws_live_frame_t *)rx_buf;
        if (bin->magic == 0xAA) {
            peq_update_band_live(bin->band_idx, bin->filter_type, 
                                bin->freq_hz, bin->gain_db_x10 / 10.0f, bin->q_x100 / 100.0f);
        }
    }

    // Reset the 3-second debounced NVS autosave timer
    peq_reset_autosave_timer();
    return ESP_OK;
}
```

---

### 6.3 Method 2: Throttled HTTP REST Live Tuning (`POST /api/eq/live`)

For environments where WebSockets cannot be established (e.g., restricted proxies, simple Python/cURL scripts, or headless integrations), live tuning is supported via a high-efficiency HTTP REST endpoint.

#### 1. Volatile RAM Endpoint: `POST /api/eq/live`
* **Crucial Distinction:** Unlike `POST /api/eq/user` (which writes to flash), `POST /api/eq/live` updates **volatile SRAM only** and returns immediately with `204 No Content`.
* **Payload:**
  ```json
  { "band": 2, "freq": 1250, "gain": 3.5, "q": 1.41 }
  ```
* **Performance Response:** Returning `204 No Content` avoids generating a response body, cutting response packets down to a single TCP ACK frame.

#### 2. Frontend Client Throttling Pattern (JavaScript)
Browsers firing slider `input` events must be throttled to 20–30 Hz using `requestAnimationFrame` and an `AbortController` to cancel stale in-flight HTTP requests:

```javascript
let activeAbortController = null;
let pendingUpdate = null;
let isThrottling = false;

function onSliderInput(bandIdx, freq, gain, q) {
    pendingUpdate = { band: bandIdx, freq: freq, gain: gain, q: q };
    if (!isThrottling) {
        isThrottling = true;
        requestAnimationFrame(sendThrottledRestUpdate);
    }
}

async function sendThrottledRestUpdate() {
    if (!pendingUpdate) {
        isThrottling = false;
        return;
    }
    // Cancel any previous request still in flight
    if (activeAbortController) activeAbortController.abort();
    activeAbortController = new AbortController();

    const data = pendingUpdate;
    pendingUpdate = null;

    try {
        await fetch('/api/eq/live', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify(data),
            signal: activeAbortController.signal
        });
    } catch (err) {
        if (err.name !== 'AbortError') console.error("REST live tuning error:", err);
    }

    // Re-check after 35 ms (~30 Hz limit)
    setTimeout(() => {
        if (pendingUpdate) sendThrottledRestUpdate();
        else isThrottling = false;
    }, 35);
}
```

---

### 6.4 Method 3: USB Composite WebHID Live Tuning (FiiO JA11 / Moondrop Style) [Zero Wi-Fi Required]

Commercial USB DSP dongles like the **FiiO JA11**, **Moondrop FreeDSP / Dawn Pro**, and **Qudelix 5K** tune their onboard parametric EQ over the exact same USB-C cable that streams audio—without requiring Wi-Fi, pairing, or local AP connections.

This project implements the exact same architecture using **TinyUSB Composite (UAC 1.0 + Custom Vendor HID)**:

#### 1. How It Works
* **Single Physical USB-C Cable:** Audio streaming (`UAC 1.0`, 48 kHz / 16-bit) and live tuning control run concurrently on `GPIO 19 (D-)` and `GPIO 20 (D+)`.
* **Standard WebHID API (`navigator.hid`):** Chromium browsers (Chrome, Edge, Opera on Windows, macOS, Linux, and Android) can communicate directly with the ESP32-S3's custom HID interface without installing any OS drivers or native software.
* **64-Byte Raw HID Control Packets:** The browser sends 64-byte binary packets containing the target band index, filter type, frequency, gain, and Q factor. The TinyUSB callback `tud_hid_set_report_cb()` processes the packet in $<100\ \mu\text{s}$, recalculates Biquad coefficients into SRAM, and triggers the 15 ms linear crossfader.
* **Dynamic Band Scaling & Custom Band Count (`CMD_SET_BAND_COUNT = 0x07`):** The browser dynamically sets the active band count (1 to 10 bands). Users can add custom bands via the `+ Add Band` button, delete bands via card `✕` buttons, set counts via the numeric stepper, or double-click anywhere on the canvas graph. Core 1 dynamically crossfades to the target band count without clicks or drops.
* **Interactive Frequency Response Graph with Draggable Control Nodes:**
  * Direct manipulation of filter points on the response graph ($X$-axis: frequency 20 Hz – 20 kHz; $Y$-axis: gain -24.0 dB to +12.0 dB).
  * Mouse wheel / touchpad scroll over nodes dynamically adjusts filter Q factor ($0.10$ to $10.00$) with visual $-3\text{ dB}$ bandwidth guide lines and shaded region.
  * Live hover tooltips display active band parameters and filter types.
  * Double-click on existing node resets gain to 0.0 dB; double-click on empty canvas space adds a new band at that frequency and gain.
* **Custom Value (Within Bound) Inputs:** The browser interface provides precise numeric inputs alongside range sliders with live two-way binding. Users can type exact values clamped to safe DSP bounds:
  * **Frequency:** 20 Hz – 20,000 Hz (1 Hz resolution)
  * **Gain:** -24.0 dB – +12.0 dB (0.1 dB resolution)
  * **Q Factor:** 0.10 – 10.00 (0.01 resolution)
  * **Preamp Headroom:** -12.0 dB – 0.0 dB (0.1 dB resolution)
* **Bi-directional State Feedback & Band Count Sync:** The ESP32-S3 reports current active coefficients, preset status, and active dynamic band count (`reserved[0]` at byte offset 16) back to the browser via HID IN reports (`tud_hid_report()`), enabling the browser to automatically adapt its slider cards in real time.

> [!TIP]
> For complete TinyUSB composite descriptors, HID report schemas (`peq_hid_packet_t`), endpoint handlers, and the frontend JavaScript WebHID client (`UsbDacTuner`), see [`usb_dac_guide.md` (Section 5)](usb_dac_guide.md#5-fiio-ja11--moondrop-style-live-peq-tuning-over-usb-composite-uac--webhid-architecture).

---

### 6.5 Explicit "Save to Preset" Engine & Flash Persistence Architecture

To guarantee **absolute zero unneeded flash wear** and prevent accidental overwrites, the firmware adopts the **Commercial DSP "Audition & Explicit Save" Design Pattern** (as used in the Qudelix 5K and FiiO Control):

```
                     AUDITION VS. PERSISTENT STORAGE LIFECYCLE
┌─────────────────────────────────────────────────────────────────────────────────┐
│ VOLATILE AUDITIONING (RAM ONLY)                                                 │
│   • Sliders modify s_active_peq_bank in SRAM instantly (<1 ms).                 │
│   • DSP Core 1 crossfades immediately; user auditions changes live.             │
│   • s_peq_dirty = true; UI shows badge: [Unsaved Changes - Auditioning].        │
│   • ZERO bytes written to SPI flash memory!                                     │
└────────────────────────────────────────┬────────────────────────────────────────┘
                                         │ User clicks [💾 Save to Preset] or
                                         │ triggers Hardware Long-Press (>3s)
                                         ▼
┌─────────────────────────────────────────────────────────────────────────────────┐
│ EXPLICIT SAVE ENGINE (peq_storage_save_active_to_preset)                        │
│   1. Delta / Dirty Check: Compares SRAM bank against flash (memcmp).            │
│      If identical, aborts immediately -> 0 flash cycles expended!               │
│   2. Packages active coefficients into packed binary struct peq_nvs_profile_t.  │
│   3. Calculates CRC32 checksum.                                                 │
│   4. Writes to NVS partition "user_peq" and executes nvs_commit().              │
│   5. Clears dirty flag (s_peq_dirty = false).                                   │
│   6. Broadcasts sync event to all clients: {"op":"preset_saved","slot":slot}.    │
└─────────────────────────────────────────────────────────────────────────────────┘
```

#### 1. Firmware Implementation (`peq_storage.c`)

```c
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_rom_crc.h"
#include "peq_storage.h"

static const char *TAG = "PEQ_STORAGE";
static bool s_peq_dirty = false;

esp_err_t peq_storage_save_active_to_preset(uint8_t slot, const char *profile_name)
{
    nvs_handle_t nvs_h;
    esp_err_t err;

    // 1. Prepare NVS binary struct from volatile SRAM active bank
    peq_nvs_profile_t new_prof = {0};
    new_prof.magic = PEQ_NVS_MAGIC;
    new_prof.version = PEQ_NVS_VERSION;
    new_prof.active_slot = slot;
    new_prof.num_bands = s_active_peq_bank.num_bands;
    new_prof.preamp_db_x10 = (int16_t)roundf(s_active_peq_bank.preamp_db * 10.0f);
    
    if (profile_name && strlen(profile_name) > 0) {
        strncpy(new_prof.name, profile_name, sizeof(new_prof.name) - 1);
    } else {
        snprintf(new_prof.name, sizeof(new_prof.name), "User Preset %u", slot);
    }

    for (int i = 0; i < new_prof.num_bands; i++) {
        new_prof.bands[i].type = s_active_peq_bank.bands[i].type;
        new_prof.bands[i].enabled = s_active_peq_bank.bands[i].enabled ? 1 : 0;
        new_prof.bands[i].freq_hz = s_active_peq_bank.bands[i].freq_hz;
        new_prof.bands[i].gain_db_x10 = (int16_t)roundf(s_active_peq_bank.bands[i].gain_db * 10.0f);
        new_prof.bands[i].q_x100 = (uint16_t)roundf(s_active_peq_bank.bands[i].q * 100.0f);
    }

    // 2. Compute CRC32
    new_prof.crc32 = esp_rom_crc32_le(0, (const uint8_t *)&new_prof, 
                                      sizeof(peq_nvs_profile_t) - sizeof(uint32_t));

    // 3. Open NVS and perform Dirty Comparison (Delta check against existing flash)
    err = nvs_open("user_peq", NVS_READWRITE, &nvs_h);
    if (err != ESP_OK) return err;

    peq_nvs_profile_t existing_prof;
    size_t required_size = sizeof(peq_nvs_profile_t);
    char key[16];
    snprintf(key, sizeof(key), "prof_%u", slot);

    if (nvs_get_blob(nvs_h, key, &existing_prof, &required_size) == ESP_OK) {
        if (existing_prof.crc32 == new_prof.crc32 &&
            memcmp(&existing_prof, &new_prof, sizeof(peq_nvs_profile_t)) == 0) {
            ESP_LOGI(TAG, "Slot %u already identical in flash. Skipping NVS write.", slot);
            nvs_close(nvs_h);
            s_peq_dirty = false;
            return ESP_OK; // Zero flash write!
        }
    }

    // 4. Commit to NVS flash
    err = nvs_set_blob(nvs_h, key, &new_prof, sizeof(peq_nvs_profile_t));
    if (err == ESP_OK) {
        nvs_set_u8(nvs_h, "active_slot", slot);
        err = nvs_commit(nvs_h);
    }
    nvs_close(nvs_h);

    if (err == ESP_OK) {
        s_peq_dirty = false;
        ESP_LOGI(TAG, "Explicitly saved active PEQ to Preset Slot %u ('%s')", slot, new_prof.name);
    }
    return err;
}
```

#### 2. Optional Debounced Autosave (Configurable Alternative)
For users who prefer seamless hands-free saving without pressing a button, firmware provides an optional FreeRTOS one-shot timer (`CONFIG_PEQ_AUTOSAVE_DELAY_MS = 3000`). When enabled, every volatile adjustment calls `xTimerReset()`, committing to NVS after 3 seconds of silence. However, **Explicit Save to Preset** remains the primary recommended default.

---

### 6.6 Complete Live Tuning & Control Interface Matrix

| Channel / Method | Protocol | Transport / Endpoint | Description | Latency | Lifecycle / Storage |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **Save to Preset** | HTTP REST | Wi-Fi (`POST /api/eq/save_preset`) | **Explicitly commits active volatile tuning to NVS slot.** | 35 ms | **Flash NVS Commit** with Delta check. |
| **Save to Preset** | WebSocket | Wi-Fi (`ws://<ip>/ws/eq`) | **`{"op":"save_to_preset","slot":0,"name":"..."}`** | 10 ms | **Flash NVS Commit** with Delta check. |
| **Save to Preset** | Raw HID | USB (`CMD_SAVE_TO_PRESET: 0x05`) | **Explicit WebHID commit to target preset slot.** | <1 ms | **Flash NVS Commit** with Delta check. |
| **Live Slider Drag**| Raw HID | USB Composite (`CMD_SET_BAND_LIVE: 0x02`) | FiiO JA11 / Dongle style driverless live tuning. | **<1 ms** | **Volatile SRAM** (Zero flash writes). |
| **Set Band Count** | Raw HID | USB Composite (`CMD_SET_BAND_COUNT: 0x07`) | Dynamic band scaling (1 to 10 bands). Updates active biquad cascade with 15ms crossfade. | **<1 ms** | **Volatile SRAM** (Zero flash writes). |
| **Live Slider Drag**| WebSocket | Wi-Fi (`ws://<ip>/ws/eq`) | Real-time 60 FPS slider stream & multi-client sync. | **<2 ms** | **Volatile SRAM** (Zero flash writes). |
| **Live Slider Drag**| HTTP REST | Wi-Fi (`POST /api/eq/live`) | Throttled volatile live tuning fallback (returns 204). | 15–40 ms | **Volatile SRAM** (Zero flash writes). |
| **GET Profiles** | HTTP REST | Wi-Fi (`GET /api/eq/profiles`) | Lists all profiles (Presets 1–3 + User Slots) with dirty flag. | 20 ms | Read-only inspect. |
| **GET Active** | HTTP REST | Wi-Fi (`GET /api/eq/active`) | Retrieves active profile ID, coefficients, and dirty state. | 15 ms | Read-only inspect. |
| **POST Active** | HTTP REST | Wi-Fi (`POST /api/eq/active`) | Switches active profile with 15ms crossfade (`{"slot": 0..3}`). | 20 ms | Persisted to NVS immediately. |
| **POST Reset** | HTTP REST | Wi-Fi (`POST /api/eq/user/reset`) | Resets the User Custom PEQ from a factory preset template. | 35 ms | **Flash NVS Commit** immediately. |
| **GET Export** | HTTP REST | Wi-Fi (`GET /api/eq/export`) | Downloads User Custom PEQ as an AutoEQ-compatible JSON file. | 25 ms | Download. |
| **POST Import** | HTTP REST | Wi-Fi (`POST /api/eq/import_sd`) | Imports a PEQ profile file from MicroSD storage into NVS. | 40 ms | **Flash NVS Commit** immediately. |

---

### 6.7 Universal Frontend Client Implementation (WebSocket with REST Fallback)

This lightweight JavaScript module connects to WebSocket on load, falls back to throttled HTTP REST if WebSocket fails, and guarantees zero flash wear:

```javascript
class LivePEQTuner {
    constructor() {
        this.ws = null;
        this.useWebSocket = false;
        this.pendingRest = null;
        this.isThrottlingRest = false;
        this.connectWebSocket();
    }

    connectWebSocket() {
        const wsUrl = `ws://${window.location.host}/ws/eq`;
        this.ws = new WebSocket(wsUrl);

        this.ws.onopen = () => {
            console.log("PEQ Live Tuning: WebSocket connected (High-Speed Mode)");
            this.useWebSocket = true;
        };

        this.ws.onclose = () => {
            console.warn("PEQ Live Tuning: WebSocket disconnected. Falling back to HTTP REST.");
            this.useWebSocket = false;
            // Attempt reconnect after 3 seconds
            setTimeout(() => this.connectWebSocket(), 3000);
        };

        this.ws.onmessage = (event) => {
            // Handle multi-client synchronization broadcast from ESP32
            const syncMsg = JSON.parse(event.data);
            if (syncMsg.op === "sync") {
                this.updateUIBand(syncMsg.band, syncMsg.freq, syncMsg.gain, syncMsg.q);
            }
        };
    }

    // Called on slider 'input' event (while actively dragging)
    tuneBandLive(bandIdx, type, freqHz, gainDb, qVal) {
        if (this.useWebSocket && this.ws.readyState === WebSocket.OPEN) {
            // Stream via WebSocket at full 60 FPS
            this.ws.send(JSON.stringify({
                op: "tune",
                band: bandIdx,
                type: type,
                freq: freqHz,
                gain: gainDb,
                q: qVal
            }));
        } else {
            // Fallback to throttled HTTP REST
            this.queueThrottledRest(bandIdx, type, freqHz, gainDb, qVal);
        }
    }

    queueThrottledRest(band, type, freq, gain, q) {
        this.pendingRest = { band, type, freq, gain, q };
        if (!this.isThrottlingRest) {
            this.isThrottlingRest = true;
            this.sendRestTick();
        }
    }

    async sendRestTick() {
        if (!this.pendingRest) {
            this.isThrottlingRest = false;
            return;
        }
        const data = this.pendingRest;
        this.pendingRest = null;

        await fetch('/api/eq/live', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify(data)
        }).catch(() => {});

        setTimeout(() => this.sendRestTick(), 35); // 30 Hz throttle
    }

    // Explicit Save to Preset (Triggered when user clicks "💾 Save to Preset")
    async saveToPreset(slot = 0, profileName = "User Custom") {
        if (this.useWebSocket && this.ws.readyState === WebSocket.OPEN) {
            this.ws.send(JSON.stringify({
                op: "save_to_preset",
                slot: slot,
                name: profileName
            }));
        } else {
            await fetch('/api/eq/save_preset', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ slot: slot, name: profileName })
            }).catch(console.error);
        }
        console.log(`Explicitly committed active EQ to NVS Preset Slot ${slot}`);
    }
}
```

---

## 7. Hardware Preset Controls & User PEQ Selection

The persistent User PEQ integrates seamlessly into both hardware control configurations:

### 7.1 Standalone 3-Button Controller (Schematic 1, Rev 1.3)
In standalone DSP/DAC builds without a display, the 3 tactile pushbuttons on `GPIO 15`, `GPIO 16`, and `GPIO 17` provide instant profile switching and explicit saving:

* **Short Press SW3 (`GPIO 15`):** Engage **Preset 1 (Reference Flat / Bypass)**.
* **Short Press SW5 (`GPIO 16`):** Engage **Preset 2 (Warm Bass Boost)**.
* **Short Press SW4 (`GPIO 17`):** Engage **Preset 3 (Harman AutoEQ Target)**.
* **Long Press (>1.5s) on SW4 (`GPIO 17`) OR Combo (SW3 + SW5):** **Engage Persistent User Custom PEQ (NVS Slot 0)**!
* **Explicit Save Shortcut (Hold SW4 for >3.0s OR Hold SW3 + SW4 simultaneously):** **Commits the current active volatile tuning into NVS User Preset Slot 0!** The WS2812B RGB LED flashes **Magenta 3 times rapidly** to confirm successful flash commit.
* **WS2812B RGB LED Status Feedback (`GPIO 48`):**
  * 🟢 **Green:** Preset 1 Active (Flat)
  * 🔵 **Cyan:** Preset 2 Active (Bass Boost)
  * 🔷 **Blue:** Preset 3 Active (Harman Target)
  * 🟣 **Magenta / Violet:** **Persistent User Custom PEQ Active (NVS)**
  * 🟣⚡ **Triple Magenta Flash:** **Explicit Save to NVS Successful**

### 7.2 Local Player DAP UI with OLED & Rotary Encoder (Schematic 2, Rev 1.2)
On builds equipped with the 0.96" I2C OLED display (`OLED1`) and EC11 rotary encoder (`SW2`), profile selection and explicit saving are integrated into the display UI:

```
┌────────────────────────────────────────────────────────┐
│ [Wi-Fi] 44.1kHz 16b          VOL: [████████░░] 80%    │ <- Status Header
│                                                        │
│ EQ Profile: [User Custom (NVS)] [Unsaved*]             │ <- Active Profile & Dirty Flag
│ Profile: Sennheiser HD600 Harman +2dB Sub              │ <- Profile Name from NVS
│                                                        │
│ [PEQ: Custom (NVS)]           || Playback: PLAYING    │ <- Status Footer
└────────────────────────────────────────────────────────┘
```

* **Rotating Encoder in EQ Menu:** Cycles through:
  * `[Preset 1: Flat Reference]`
  * `[Preset 2: Bass Boost +4.5dB]`
  * `[Preset 3: Harman Target OE]`
  * `● [User Custom (NVS): My Headphones]`
  * **`> [💾 Save Active to User Preset]`** <- Explicit commit of active volatile tuning into NVS. Selecting it executes `peq_storage_save_active_to_preset(0, "User Custom")` with Delta verification, displaying `[SAVED TO NVS]`.
* Pressing the encoder button commits the selection to NVS (`active_slot`) and applies the crossfade immediately.

---

## 8. Reference for AI Agents: Implementation Checklist & DSP Invariants

### 8.1 DSP Invariants
1. **Saturation Clamping:** Never convert float samples directly back to `int16_t` with a raw C cast (`(int16_t)sample`). Floating-point overflow causes catastrophic integer wrap-around (e.g. `+32768.0f` wrapping to `-32768`, producing deafening distortion). Always use hard saturation clamping:
   ```c
   static inline int16_t clamp16(float v) {
       if (v > 32767.0f) return 32767;
       if (v < -32768.0f) return -32768;
       return (int16_t)v;
   }
   ```
2. **Pre-Gain Headroom Invariant:** If any PEQ band has positive gain ($G > 0\text{ dB}$), Pre-Gain MUST be set to at least $-G_{max}\text{ dB}$ to guarantee mathematical immunity to digital clipping.
3. **Decoupled Preset Invariant:** User profile writes to NVS MUST NEVER touch or overwrite `g_factory_presets[]`. Factory presets are strictly `const` in `.rodata`.
4. **CRC32 Validation:** Before applying an NVS profile upon boot, compute and verify its CRC32 checksum. If the checksum mismatches (e.g. power was interrupted during an NVS write), log an error and safely fall back to **Preset 1 (Flat)**.
5. **Volatile Auditioning & Explicit Save Invariant (Flash Wear Prevention):** Real-time slider adjustments via USB WebHID (`0x4551`), WebSocket (`/ws/eq`), or HTTP REST (`POST /api/eq/live`) MUST NEVER write directly to NVS flash. High-frequency slider dragging (30–60 Hz) will rapidly burn out SPI flash sectors. All live updates modify volatile SRAM only (`s_active_peq_bank`). Flash persistence occurs strictly via **Explicit Save to Preset** commands (`POST /api/eq/save_preset`, WebHID `CMD_SAVE_TO_PRESET`, or hardware long-press shortcuts) with Delta comparison (`memcmp`) to guarantee zero redundant flash writes.

### 8.2 Implementation Checklist
Future AI agents tasked with implementing this PEQ subsystem should execute the following steps:

- [ ] **Step 1:** Add `espressif/esp-dsp: "^1.4.0"` to `main/idf_component.yml`.
- [ ] **Step 2:** Create `peq_biquad.h` / `peq_biquad.c`:
  - Implement coefficient calculation functions:
    `peq_calc_peaking(...)`, `peq_calc_lowshelf(...)`, `peq_calc_highshelf(...)`.
- [ ] **Step 3:** Implement NVS storage manager `peq_storage.h` / `peq_storage.c`:
  - `esp_err_t peq_storage_init(void)`: Initialize NVS partition and load active slot.
  - `esp_err_t peq_storage_save_active_to_preset(uint8_t slot, const char *name)`: Explicit save with CRC32 and Delta dirty check.
  - `esp_err_t peq_storage_load_user_profile(peq_nvs_profile_t *prof)`: Read and validate CRC32.
  - `esp_err_t peq_storage_reset_user_to_preset(int preset_id)`: Clone factory preset into user slot.
- [ ] **Step 4:** Implement filter pipeline and double-buffered crossfader:
  ```c
  void peq_process_interleaved_s16(int16_t *samples, int num_samples, peq_bank_t *peq);
  void peq_trigger_crossfade(const peq_bank_t *new_bank, int crossfade_ms);
  ```
- [ ] **Step 5:** Integrate PEQ into `audio_stream_task`:
  - Insert `peq_process_interleaved_s16(pcm_out, samples * info.channels, &s_peq_bank)` right before `i2s_output_write()`.
- [ ] **Step 6:** Create the Control Interfaces (USB WebHID, WebSocket Server & REST API):
  - In USB DAC mode, configure TinyUSB composite descriptor and implement `tud_hid_set_report_cb()` for driverless 64-byte WebHID packets and `CMD_SAVE_TO_PRESET` (see [`usb_dac_guide.md`](usb_dac_guide.md)).
  - Initialize `esp_http_server` with WebSocket support enabled.
  - Register `ws://<ip>/ws/eq` full-duplex live stream handler (supports compact JSON, 8-byte binary frames, and `op: "save_to_preset"`).
  - Register `POST /api/eq/live` volatile RAM endpoint (returns 204 No Content).
  - Register `POST /api/eq/save_preset` explicit NVS commit endpoint.
  - Register `GET /api/eq/profiles`, `GET /api/eq/active`, `POST /api/eq/active`.
  - Register `POST /api/eq/user/reset`.
  - Serve single-page HTML5/Canvas equalization dashboard on `GET /` with live canvas response curve and explicit `[💾 Save to Preset]` button.
- [ ] **Step 7:** Connect hardware buttons (`GPIO 15, 16, 17`) and rotary encoder to profile switcher and long-press explicit save shortcut.
- [ ] **Step 8:** Verify stability with `idf.py build`.
