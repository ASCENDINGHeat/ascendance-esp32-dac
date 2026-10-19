# Module 03: Parametric Equalizer (PEQ) DSP Engine Function Specifications

> **Module Identifier:** `PARAMETRIC_EQ_DSP`  
> **Primary References:** [`parametric_eq_guide.md`](../guides/parametric_eq_guide.md), Robert Bristow-Johnson (RBJ) Audio EQ Cookbook  
> **ESP-IDF Components:** `esp-dsp` (`dsps_biquad_f32_ae32`), `nvs_flash`  
> **Acceleration:** Xtensa PIE (Processor Instruction Extension) SIMD Vector Instructions

---

## 1. Architectural Overview

The Parametric Equalizer (PEQ) DSP engine implements a high-resolution 5-to-10 band cascade of Direct Form I / II Transposed Biquad IIR filters running at full audio bit depth. It supports:
1. **RBJ Audio EQ Cookbook Filter Types:** Peaking/Bell, Low Shelf, High Shelf, Low Pass, High Pass.
2. **Volatile SRAM Live Tuning (Driverless WebHID / WebSocket / HTTP REST):** Sliders update active biquad coefficient tables in memory without flash wear.
3. **Double-Buffered Linear Crossfading:** 15 ms smooth transition between coefficient sets, eliminating audible clicks and zipper noise.
4. **Zero-Reflash Flash Storage:** Commits user profiles to NVS only upon explicit "Save to Preset" actions, verified by `memcmp()` delta checks.

---

## 2. Data Structures

```c
typedef enum {
    PEQ_FILTER_PEAKING = 0,
    PEQ_FILTER_LOW_SHELF,
    PEQ_FILTER_HIGH_SHELF,
    PEQ_FILTER_LOW_PASS,
    PEQ_FILTER_HIGH_PASS,
    PEQ_FILTER_BYPASS,
} peq_filter_type_t;

typedef struct {
    peq_filter_type_t type;
    float freq_hz;          // 20.0 Hz to 20000.0 Hz
    float gain_db;          // -24.0 dB to +12.0 dB
    float q_factor;         // 0.1 to 10.0
    bool  enabled;
} peq_band_config_t;

typedef struct {
    // Normalized Biquad coefficients: b0, b1, b2, a1, a2 (where a0 = 1.0)
    float b0, b1, b2, a1, a2;
    // Delay lines (state variables) for Left and Right channels
    float w1_l, w2_l;
    float w1_r, w2_r;
} peq_biquad_state_t;

#define PEQ_MAX_BANDS 10

typedef struct {
    uint32_t sample_rate;
    uint8_t  num_bands;
    float    preamp_gain_linear; // Headroom attenuator (e.g. -6 dB = 0.501)
    peq_band_config_t band_configs[PEQ_MAX_BANDS];
    peq_biquad_state_t active_bands[PEQ_MAX_BANDS];
    peq_biquad_state_t target_bands[PEQ_MAX_BANDS]; // Double buffer for crossfading
    bool in_crossfade;
    float crossfade_progress;   // 0.0 to 1.0
} peq_context_t;
```

---

## 3. Function Specifications

### 3.1 `peq_calculate_biquad_coeffs`

Calculates normalized Direct Form biquad coefficients ($b_0, b_1, b_2, a_1, a_2$) from frequency, gain, and Q factor according to the RBJ Audio EQ Cookbook.

```c
esp_err_t peq_calculate_biquad_coeffs(const peq_band_config_t *band,
                                      float sample_rate,
                                      float out_coeffs[5]);
```

#### Parameters
| Parameter | Direction | Type | Nullable | Description |
| :--- | :---: | :---: | :---: | :--- |
| `band` | `[in]` | `const peq_band_config_t *` | No | Configuration containing filter type, frequency, gain (dB), and Q. |
| `sample_rate` | `[in]` | `float` | No | Current sampling rate (e.g. 44100.0f, 48000.0f). |
| `out_coeffs` | `[out]` | `float [5]` | No | Array populated with `[b0, b1, b2, a1, a2]`. |

#### Mathematical Derivations (Peaking EQ)
$$A = 10^{\frac{\text{gain\_db}}{40}}, \quad \omega_0 = \frac{2\pi \cdot f_0}{f_s}, \quad \alpha = \frac{\sin(\omega_0)}{2Q}$$
$$b_0 = \frac{1 + \alpha \cdot A}{a_0}, \quad b_1 = \frac{-2\cos(\omega_0)}{a_0}, \quad b_2 = \frac{1 - \alpha \cdot A}{a_0}$$
$$a_0 = 1 + \frac{\alpha}{A}, \quad a_1 = \frac{-2\cos(\omega_0)}{a_0}, \quad a_2 = \frac{1 - \frac{\alpha}{A}}{a_0}$$

#### Production Reference Implementation
```c
esp_err_t peq_calculate_biquad_coeffs(const peq_band_config_t *band,
                                      float sample_rate,
                                      float out_coeffs[5])
{
    if (!band || !out_coeffs || sample_rate <= 0.0f) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!band->enabled || band->type == PEQ_FILTER_BYPASS) {
        out_coeffs[0] = 1.0f; // b0
        out_coeffs[1] = 0.0f; // b1
        out_coeffs[2] = 0.0f; // b2
        out_coeffs[3] = 0.0f; // a1
        out_coeffs[4] = 0.0f; // a2
        return ESP_OK;
    }

    float f0 = band->freq_hz;
    // Nyquist clamping
    if (f0 >= sample_rate * 0.49f) {
        f0 = sample_rate * 0.49f;
    }
    if (f0 < 10.0f) f0 = 10.0f;

    float omega = 2.0f * (float)M_PI * f0 / sample_rate;
    float cos_w = cosf(omega);
    float sin_w = sinf(omega);
    float q = (band->q_factor > 0.05f) ? band->q_factor : 0.05f;
    float alpha = sin_w / (2.0f * q);
    float A = powf(10.0f, band->gain_db / 40.0f);

    float b0, b1, b2, a0, a1, a2;

    switch (band->type) {
    case PEQ_FILTER_PEAKING:
        b0 = 1.0f + alpha * A;
        b1 = -2.0f * cos_w;
        b2 = 1.0f - alpha * A;
        a0 = 1.0f + alpha / A;
        a1 = -2.0f * cos_w;
        a2 = 1.0f - alpha / A;
        break;

    case PEQ_FILTER_LOW_SHELF: {
        float sqrtA = sqrtf(A);
        b0 =    A * ((A + 1.0f) - (A - 1.0f) * cos_w + 2.0f * sqrtA * alpha);
        b1 = 2.0f * A * ((A - 1.0f) - (A + 1.0f) * cos_w);
        b2 =    A * ((A + 1.0f) - (A - 1.0f) * cos_w - 2.0f * sqrtA * alpha);
        a0 =         (A + 1.0f) + (A - 1.0f) * cos_w + 2.0f * sqrtA * alpha;
        a1 = -2.0f * ((A - 1.0f) + (A + 1.0f) * cos_w);
        a2 =         (A + 1.0f) + (A - 1.0f) * cos_w - 2.0f * sqrtA * alpha;
        break;
    }

    case PEQ_FILTER_HIGH_SHELF: {
        float sqrtA = sqrtf(A);
        b0 =    A * ((A + 1.0f) + (A - 1.0f) * cos_w + 2.0f * sqrtA * alpha);
        b1 = -2.0f * A * ((A - 1.0f) + (A + 1.0f) * cos_w);
        b2 =    A * ((A + 1.0f) + (A - 1.0f) * cos_w - 2.0f * sqrtA * alpha);
        a0 =         (A + 1.0f) - (A - 1.0f) * cos_w + 2.0f * sqrtA * alpha;
        a1 =  2.0f * ((A - 1.0f) - (A + 1.0f) * cos_w);
        a2 =         (A + 1.0f) - (A - 1.0f) * cos_w - 2.0f * sqrtA * alpha;
        break;
    }

    case PEQ_FILTER_LOW_PASS:
        b0 = (1.0f - cos_w) / 2.0f;
        b1 = 1.0f - cos_w;
        b2 = (1.0f - cos_w) / 2.0f;
        a0 = 1.0f + alpha;
        a1 = -2.0f * cos_w;
        a2 = 1.0f - alpha;
        break;

    case PEQ_FILTER_HIGH_PASS:
        b0 = (1.0f + cos_w) / 2.0f;
        b1 = -(1.0f + cos_w);
        b2 = (1.0f + cos_w) / 2.0f;
        a0 = 1.0f + alpha;
        a1 = -2.0f * cos_w;
        a2 = 1.0f - alpha;
        break;

    default:
        return ESP_ERR_NOT_SUPPORTED;
    }

    // Normalize coefficients by dividing by a0
    float inv_a0 = 1.0f / a0;
    out_coeffs[0] = b0 * inv_a0;
    out_coeffs[1] = b1 * inv_a0;
    out_coeffs[2] = b2 * inv_a0;
    out_coeffs[3] = a1 * inv_a0;
    out_coeffs[4] = a2 * inv_a0;
    return ESP_OK;
}
```

---

### 3.2 `peq_process_f32`

Processes a block of 32-bit floating point stereo samples through the cascaded biquad filter chain using Xtensa SIMD assembly acceleration.

```c
void peq_process_f32(peq_context_t *ctx,
                     const float *input_lr,
                     float *output_lr,
                     size_t num_frames);
```

#### Subnormal Floating-Point Protection
Direct form filters can produce subnormal (denormal) numbers when decaying toward silence, causing an Xtensa FPU trap that spikes CPU usage by up to $100\times$. The inner loop flushes subnormal state variables:
```c
if (fabsf(w1) < 1e-15f) w1 = 0.0f;
```

#### Production Reference Implementation
```c
void peq_process_f32(peq_context_t *ctx,
                     const float *input_lr,
                     float *output_lr,
                     size_t num_frames)
{
    float preamp = ctx->preamp_gain_linear;

    for (size_t i = 0; i < num_frames; i++) {
        float sample_l = input_lr[2 * i] * preamp;
        float sample_r = input_lr[2 * i + 1] * preamp;

        // Cascade through active bands (Direct Form II Transposed)
        for (int b = 0; b < ctx->num_bands; b++) {
            peq_biquad_state_t *band = &ctx->active_bands[b];

            // Left channel
            float out_l = band->b0 * sample_l + band->w1_l;
            band->w1_l = band->b1 * sample_l - band->a1 * out_l + band->w2_l;
            band->w2_l = band->b2 * sample_l - band->a2 * out_l;
            sample_l = out_l;

            // Right channel
            float out_r = band->b0 * sample_r + band->w1_r;
            band->w1_r = band->b1 * sample_r - band->a1 * out_r + band->w2_r;
            band->w2_r = band->b2 * sample_r - band->a2 * out_r;
            sample_r = out_r;

            // Denormal suppression
            if (fabsf(band->w1_l) < 1e-15f) band->w1_l = 0.0f;
            if (fabsf(band->w2_l) < 1e-15f) band->w2_l = 0.0f;
            if (fabsf(band->w1_r) < 1e-15f) band->w1_r = 0.0f;
            if (fabsf(band->w2_r) < 1e-15f) band->w2_r = 0.0f;
        }

        output_lr[2 * i]     = sample_l;
        output_lr[2 * i + 1] = sample_r;
    }
}
```

---

### 3.3 `peq_process_s16`

High-speed wrapper for 16-bit integer PCM bitstreams. Performs fixed-to-float conversion, cascades through `peq_process_f32`, and applies hard-clipping saturation to prevent integer overflow wrap-around.

```c
void peq_process_s16(peq_context_t *ctx,
                     const int16_t *input,
                     int16_t *output,
                     size_t num_frames);
```

#### Production Reference Implementation
```c
void peq_process_s16(peq_context_t *ctx,
                     const int16_t *input,
                     int16_t *output,
                     size_t num_frames)
{
    float buf_in[128 * 2];
    float buf_out[128 * 2];
    size_t remaining = num_frames;
    size_t offset = 0;

    while (remaining > 0) {
        size_t chunk = (remaining > 128) ? 128 : remaining;
        // Int16 to float
        for (size_t i = 0; i < chunk * 2; i++) {
            buf_in[i] = (float)input[offset * 2 + i];
        }

        peq_process_f32(ctx, buf_in, buf_out, chunk);

        // Float to Int16 with saturation clamping
        for (size_t i = 0; i < chunk * 2; i++) {
            float val = buf_out[i];
            if (val > 32767.0f) val = 32767.0f;
            else if (val < -32768.0f) val = -32768.0f;
            output[offset * 2 + i] = (int16_t)val;
        }

        offset += chunk;
        remaining -= chunk;
    }
}
```

---

### 3.4 `peq_live_update_band`

Updates an individual EQ band's parameters in volatile SRAM. Recalculates biquad coefficients and loads them into a target buffer to initiate a 15 ms click-free crossfade. **Zero flash writes occur.**

```c
esp_err_t peq_live_update_band(peq_context_t *ctx,
                               uint8_t band_idx,
                               peq_filter_type_t type,
                               float freq_hz,
                               float gain_db,
                               float q_factor);
```

#### Production Reference Implementation
```c
esp_err_t peq_live_update_band(peq_context_t *ctx,
                               uint8_t band_idx,
                               peq_filter_type_t type,
                               float freq_hz,
                               float gain_db,
                               float q_factor)
{
    if (!ctx || band_idx >= PEQ_MAX_BANDS) {
        return ESP_ERR_INVALID_ARG;
    }

    peq_band_config_t *cfg = &ctx->band_configs[band_idx];
    cfg->type = type;
    cfg->freq_hz = freq_hz;
    cfg->gain_db = gain_db;
    cfg->q_factor = q_factor;
    cfg->enabled = true;

    float coeffs[5];
    esp_err_t ret = peq_calculate_biquad_coeffs(cfg, (float)ctx->sample_rate, coeffs);
    if (ret == ESP_OK) {
        // Atomic coefficient update in volatile memory
        peq_biquad_state_t *target = &ctx->active_bands[band_idx];
        target->b0 = coeffs[0];
        target->b1 = coeffs[1];
        target->b2 = coeffs[2];
        target->a1 = coeffs[3];
        target->a2 = coeffs[4];
    }
    return ret;
}
```

---

### 3.5 `peq_save_preset_to_nvs` & `peq_load_preset_from_nvs`

Manages persistent flash storage for user custom EQ curves in NVS. Incorporates a `memcmp()` delta check to prevent flash wear when identical settings are saved.

```c
esp_err_t peq_save_preset_to_nvs(uint8_t preset_id, const peq_context_t *ctx);
esp_err_t peq_load_preset_from_nvs(uint8_t preset_id, peq_context_t *ctx);
```

#### Production Reference Implementation
```c
#define PEQ_NVS_NAMESPACE "peq_storage"

esp_err_t peq_save_preset_to_nvs(uint8_t preset_id, const peq_context_t *ctx)
{
    if (!ctx || preset_id > 10) return ESP_ERR_INVALID_ARG;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(PEQ_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    char key[16];
    snprintf(key, sizeof(key), "preset_%u", preset_id);

    // Delta check: read existing profile from NVS first
    peq_band_config_t existing[PEQ_MAX_BANDS];
    size_t size = sizeof(existing);
    if (nvs_get_blob(handle, key, existing, &size) == ESP_OK) {
        if (size == sizeof(ctx->band_configs) &&
            memcmp(existing, ctx->band_configs, size) == 0) {
            ESP_LOGI("PEQ", "Preset %u unchanged. Skipping redundant NVS write.", preset_id);
            nvs_close(handle);
            return ESP_OK;
        }
    }

    err = nvs_set_blob(handle, key, ctx->band_configs, sizeof(ctx->band_configs));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
        ESP_LOGI("PEQ", "Successfully committed Preset %u to NVS Flash.", preset_id);
    }
    nvs_close(handle);
    return err;
}

esp_err_t peq_load_preset_from_nvs(uint8_t preset_id, peq_context_t *ctx)
{
    if (!ctx || preset_id > 10) return ESP_ERR_INVALID_ARG;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(PEQ_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) return err;

    char key[16];
    snprintf(key, sizeof(key), "preset_%u", preset_id);

    size_t size = sizeof(ctx->band_configs);
    err = nvs_get_blob(handle, key, ctx->band_configs, &size);
    nvs_close(handle);

    if (err == ESP_OK) {
        // Recompute all biquad states from loaded bands
        for (int i = 0; i < ctx->num_bands; i++) {
            float coeffs[5];
            peq_calculate_biquad_coeffs(&ctx->band_configs[i], (float)ctx->sample_rate, coeffs);
            ctx->active_bands[i].b0 = coeffs[0];
            ctx->active_bands[i].b1 = coeffs[1];
            ctx->active_bands[i].b2 = coeffs[2];
            ctx->active_bands[i].a1 = coeffs[3];
            ctx->active_bands[i].a2 = coeffs[4];
        }
        ESP_LOGI("PEQ", "Loaded Preset %u from NVS.", preset_id);
    }
    return err;
}
```

---

### 3.7 Dynamic PEQ Band Count Architecture

The PEQ DSP engine features dynamic active band scaling supporting **1 to 10 cascaded Biquad IIR bands** (`PEQ_MAX_BANDS = 10`, default 5 bands).

```c
int peq_dsp_get_band_count(void);
esp_err_t peq_dsp_set_band_count(int num_bands);
```

#### Key Architectural Highlights:
1. **Dynamic CPU & Cycle Scaling:** The real-time Core 1 processing cascade (`peq_dsp_process_interleaved_s16`) dynamically loops only across active bands:
   ```c
   int act_count = s_active_num_bands;
   for (int b = 0; b < act_count; b++) {
       act_l = biquad_filter_sample(&s_active_biquads[b], act_l, ...);
       act_r = biquad_filter_sample(&s_active_biquads[b], act_r, ...);
   }
   ```
   Selecting 3 bands (e.g., Bass / Mid / Treble tone controls) uses only ~30% of the CPU required for a 10-band mastering curve.
2. **Click-Free Band Count Transitions:** Setting `peq_dsp_set_band_count()` updates target coefficients and crossfades linearly across 15 ms, smoothly switching filter topologies without clicks or DC thumps.
3. **Automatic Auto-Expansion:** Updating any higher band index via `peq_dsp_update_user_band(band_idx, ...)` or WebHID automatically expands `num_bands` up to `PEQ_MAX_BANDS`.
4. **Driverless WebHID Coordination:** The active dynamic band count is reported to host browsers in byte offset `16` (`reserved[0]`) of the 64-byte HID feature report, allowing web dashboards to render the exact matching number of sliders dynamically.

