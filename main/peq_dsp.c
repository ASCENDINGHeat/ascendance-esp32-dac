#include "peq_dsp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"

#define TAG "PEQ_DSP"
#define NVS_NAMESPACE "audio_peq"
#define NVS_KEY_USER  "user_profile"

typedef struct {
    float b0, b1, b2, a1, a2;
    float w1_l, w2_l;
    float w1_r, w2_r;
} biquad_state_t;

static peq_profile_t s_profiles[PEQ_PRESET_COUNT];
static peq_preset_id_t s_active_preset = PEQ_PRESET_FLAT;

static int s_active_num_bands = PEQ_DEFAULT_NUM_BANDS;
static int s_target_num_bands = PEQ_DEFAULT_NUM_BANDS;

static biquad_state_t s_active_biquads[PEQ_MAX_BANDS];
static biquad_state_t s_target_biquads[PEQ_MAX_BANDS];

static float s_active_preamp_linear = 1.0f;
static float s_target_preamp_linear = 1.0f;

static bool s_in_crossfade = false;
static float s_crossfade_progress = 0.0f;
static float s_crossfade_step = 0.0f; // 1.0 / (0.015s * sample_rate)

static int s_sample_rate = 44100;


static inline int16_t clamp_s16(float val)
{
    if (val > 32767.0f) return 32767;
    if (val < -32768.0f) return -32768;
    return (int16_t)roundf(val);
}

static void calculate_biquad(const peq_band_param_t *band, float sample_rate, biquad_state_t *out_bq)
{
    if (!band || !band->enabled || band->type == PEQ_FILTER_BYPASS || fabsf(band->gain_db) < 0.05f) {
        out_bq->b0 = 1.0f;
        out_bq->b1 = 0.0f;
        out_bq->b2 = 0.0f;
        out_bq->a1 = 0.0f;
        out_bq->a2 = 0.0f;
        return;
    }

    float f0 = band->freq_hz;
    if (f0 >= sample_rate * 0.49f) f0 = sample_rate * 0.49f;
    if (f0 < 10.0f) f0 = 10.0f;

    float omega = 2.0f * (float)M_PI * f0 / sample_rate;
    float cos_w = cosf(omega);
    float sin_w = sinf(omega);
    float q = (band->q_factor > 0.05f) ? band->q_factor : 0.05f;
    float alpha = sin_w / (2.0f * q);
    float A = powf(10.0f, band->gain_db / 40.0f);

    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f;
    float a0 = 1.0f, a1 = 0.0f, a2 = 0.0f;

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
        float sqrt_A = sqrtf(A);
        b0 = A * ((A + 1.0f) - (A - 1.0f) * cos_w + 2.0f * sqrt_A * alpha);
        b1 = 2.0f * A * ((A - 1.0f) - (A + 1.0f) * cos_w);
        b2 = A * ((A + 1.0f) - (A - 1.0f) * cos_w - 2.0f * sqrt_A * alpha);
        a0 = (A + 1.0f) + (A - 1.0f) * cos_w + 2.0f * sqrt_A * alpha;
        a1 = -2.0f * ((A - 1.0f) + (A + 1.0f) * cos_w);
        a2 = (A + 1.0f) + (A - 1.0f) * cos_w - 2.0f * sqrt_A * alpha;
        break;
    }

    case PEQ_FILTER_HIGH_SHELF: {
        float sqrt_A = sqrtf(A);
        b0 = A * ((A + 1.0f) + (A - 1.0f) * cos_w + 2.0f * sqrt_A * alpha);
        b1 = -2.0f * A * ((A - 1.0f) + (A + 1.0f) * cos_w);
        b2 = A * ((A + 1.0f) + (A - 1.0f) * cos_w - 2.0f * sqrt_A * alpha);
        a0 = (A + 1.0f) - (A - 1.0f) * cos_w + 2.0f * sqrt_A * alpha;
        a1 = 2.0f * ((A - 1.0f) - (A + 1.0f) * cos_w);
        a2 = (A + 1.0f) - (A - 1.0f) * cos_w - 2.0f * sqrt_A * alpha;
        break;
    }

    default:
        break;
    }

    // Normalize coefficients by a0
    out_bq->b0 = b0 / a0;
    out_bq->b1 = b1 / a0;
    out_bq->b2 = b2 / a0;
    out_bq->a1 = a1 / a0;
    out_bq->a2 = a2 / a0;
}

static void apply_profile_to_biquads(const peq_profile_t *prof, biquad_state_t *target, float *preamp_lin)
{
    *preamp_lin = powf(10.0f, prof->preamp_gain_db / 20.0f);
    int count = (prof->num_bands > 0 && prof->num_bands <= PEQ_MAX_BANDS) ? prof->num_bands : PEQ_DEFAULT_NUM_BANDS;
    for (int i = 0; i < PEQ_MAX_BANDS; i++) {
        if (i < count) {
            calculate_biquad(&prof->bands[i], (float)s_sample_rate, &target[i]);
        } else {
            target[i].b0 = 1.0f;
            target[i].b1 = 0.0f;
            target[i].b2 = 0.0f;
            target[i].a1 = 0.0f;
            target[i].a2 = 0.0f;
        }
    }
}

static void setup_factory_presets(void)
{
    // Preset 0: Flat / Bypass (0 dB)
    memset(&s_profiles[PEQ_PRESET_FLAT], 0, sizeof(peq_profile_t));
    strncpy(s_profiles[PEQ_PRESET_FLAT].name, "Flat / Bypass", sizeof(s_profiles[PEQ_PRESET_FLAT].name));
    s_profiles[PEQ_PRESET_FLAT].num_bands = PEQ_DEFAULT_NUM_BANDS;
    s_profiles[PEQ_PRESET_FLAT].preamp_gain_db = 0.0f;
    for (int i = 0; i < PEQ_MAX_BANDS; i++) {
        s_profiles[PEQ_PRESET_FLAT].bands[i].type = PEQ_FILTER_BYPASS;
        s_profiles[PEQ_PRESET_FLAT].bands[i].enabled = false;
    }

    // Preset 1: Warm Bass Boost (+4.5 dB)
    memset(&s_profiles[PEQ_PRESET_BASS_BOOST], 0, sizeof(peq_profile_t));
    strncpy(s_profiles[PEQ_PRESET_BASS_BOOST].name, "Bass Boost", sizeof(s_profiles[PEQ_PRESET_BASS_BOOST].name));
    s_profiles[PEQ_PRESET_BASS_BOOST].num_bands = PEQ_DEFAULT_NUM_BANDS;
    s_profiles[PEQ_PRESET_BASS_BOOST].preamp_gain_db = -2.5f; // Headroom to prevent clipping
    s_profiles[PEQ_PRESET_BASS_BOOST].bands[0] = (peq_band_param_t){PEQ_FILTER_LOW_SHELF, 80.0f, 4.5f, 0.71f, true};
    s_profiles[PEQ_PRESET_BASS_BOOST].bands[1] = (peq_band_param_t){PEQ_FILTER_PEAKING, 250.0f, 1.5f, 1.0f, true};
    s_profiles[PEQ_PRESET_BASS_BOOST].bands[2] = (peq_band_param_t){PEQ_FILTER_PEAKING, 1000.0f, 0.0f, 1.0f, false};
    s_profiles[PEQ_PRESET_BASS_BOOST].bands[3] = (peq_band_param_t){PEQ_FILTER_PEAKING, 4000.0f, -1.0f, 1.0f, true};
    s_profiles[PEQ_PRESET_BASS_BOOST].bands[4] = (peq_band_param_t){PEQ_FILTER_HIGH_SHELF, 10000.0f, 0.5f, 0.71f, true};
    for (int i = 5; i < PEQ_MAX_BANDS; i++) {
        s_profiles[PEQ_PRESET_BASS_BOOST].bands[i].type = PEQ_FILTER_BYPASS;
        s_profiles[PEQ_PRESET_BASS_BOOST].bands[i].enabled = false;
    }

    // Preset 2: Harman Target (OE2018 Curve)
    memset(&s_profiles[PEQ_PRESET_HARMAN], 0, sizeof(peq_profile_t));
    strncpy(s_profiles[PEQ_PRESET_HARMAN].name, "Harman Target", sizeof(s_profiles[PEQ_PRESET_HARMAN].name));
    s_profiles[PEQ_PRESET_HARMAN].num_bands = PEQ_DEFAULT_NUM_BANDS;
    s_profiles[PEQ_PRESET_HARMAN].preamp_gain_db = -4.0f; // Headroom for Harman bass & ear gain
    s_profiles[PEQ_PRESET_HARMAN].bands[0] = (peq_band_param_t){PEQ_FILTER_LOW_SHELF, 105.0f, 5.0f, 0.71f, true};
    s_profiles[PEQ_PRESET_HARMAN].bands[1] = (peq_band_param_t){PEQ_FILTER_PEAKING, 200.0f, -1.5f, 1.2f, true};
    s_profiles[PEQ_PRESET_HARMAN].bands[2] = (peq_band_param_t){PEQ_FILTER_PEAKING, 1200.0f, 1.0f, 1.4f, true};
    s_profiles[PEQ_PRESET_HARMAN].bands[3] = (peq_band_param_t){PEQ_FILTER_PEAKING, 3000.0f, 2.5f, 1.8f, true};
    s_profiles[PEQ_PRESET_HARMAN].bands[4] = (peq_band_param_t){PEQ_FILTER_HIGH_SHELF, 8000.0f, -1.0f, 0.71f, true};
    for (int i = 5; i < PEQ_MAX_BANDS; i++) {
        s_profiles[PEQ_PRESET_HARMAN].bands[i].type = PEQ_FILTER_BYPASS;
        s_profiles[PEQ_PRESET_HARMAN].bands[i].enabled = false;
    }

    // Preset 3: User Custom (Defaults to Flat until customized or loaded from NVS)
    memset(&s_profiles[PEQ_PRESET_USER_CUSTOM], 0, sizeof(peq_profile_t));
    strncpy(s_profiles[PEQ_PRESET_USER_CUSTOM].name, "User Custom", sizeof(s_profiles[PEQ_PRESET_USER_CUSTOM].name));
    s_profiles[PEQ_PRESET_USER_CUSTOM].num_bands = PEQ_DEFAULT_NUM_BANDS;
    s_profiles[PEQ_PRESET_USER_CUSTOM].preamp_gain_db = 0.0f;
    for (int i = 0; i < PEQ_MAX_BANDS; i++) {
        s_profiles[PEQ_PRESET_USER_CUSTOM].bands[i].type = PEQ_FILTER_BYPASS;
        s_profiles[PEQ_PRESET_USER_CUSTOM].bands[i].enabled = false;
    }
}

static void load_user_profile_nvs(void)
{
    nvs_handle_t nvs_h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_h);
    if (err == ESP_OK) {
        size_t required_size = sizeof(peq_profile_t);
        peq_profile_t loaded_prof;
        err = nvs_get_blob(nvs_h, NVS_KEY_USER, &loaded_prof, &required_size);
        if (err == ESP_OK && required_size == sizeof(peq_profile_t)) {
            if (loaded_prof.num_bands == 0 || loaded_prof.num_bands > PEQ_MAX_BANDS) {
                loaded_prof.num_bands = PEQ_DEFAULT_NUM_BANDS;
            }
            s_profiles[PEQ_PRESET_USER_CUSTOM] = loaded_prof;
            ESP_LOGI(TAG, "Loaded User Custom EQ profile from NVS: \"%s\" (%d bands)",
                     loaded_prof.name, loaded_prof.num_bands);
        }
        nvs_close(nvs_h);
    }
}


esp_err_t peq_dsp_init(int sample_rate)
{
    s_sample_rate = sample_rate;
    setup_factory_presets();
    load_user_profile_nvs();

    // 15ms crossfade duration
    s_crossfade_step = 1.0f / (0.015f * (float)s_sample_rate);

    // Apply default Flat preset immediately
    s_active_preset = PEQ_PRESET_FLAT;
    apply_profile_to_biquads(&s_profiles[PEQ_PRESET_FLAT], s_active_biquads, &s_active_preamp_linear);
    s_in_crossfade = false;

    ESP_LOGI(TAG, "Parametric EQ DSP Engine initialized at %d Hz (Active: %s)",
             s_sample_rate, s_profiles[s_active_preset].name);
    return ESP_OK;
}

void peq_dsp_set_sample_rate(int sample_rate)
{
    if (sample_rate <= 0 || sample_rate == s_sample_rate) return;
    s_sample_rate = sample_rate;
    s_crossfade_step = 1.0f / (0.015f * (float)s_sample_rate);

    // Recalculate active biquad coefficients for new sample rate
    apply_profile_to_biquads(&s_profiles[s_active_preset], s_active_biquads, &s_active_preamp_linear);
    if (s_in_crossfade) {
        s_in_crossfade = false; // Abort pending crossfade to avoid misaligned filter states
    }
}

void peq_dsp_select_preset(peq_preset_id_t preset_id)
{
    if (preset_id >= PEQ_PRESET_COUNT || preset_id == s_active_preset) return;

    ESP_LOGI(TAG, "Switching PEQ preset: \"%s\" (%d bands) -> \"%s\" (%d bands) (15ms crossfade)",
             s_profiles[s_active_preset].name, s_profiles[s_active_preset].num_bands,
             s_profiles[preset_id].name, s_profiles[preset_id].num_bands);

    // Prepare target biquad coefficients in double buffer
    s_target_num_bands = (s_profiles[preset_id].num_bands > 0 && s_profiles[preset_id].num_bands <= PEQ_MAX_BANDS)
                         ? s_profiles[preset_id].num_bands : PEQ_DEFAULT_NUM_BANDS;
    apply_profile_to_biquads(&s_profiles[preset_id], s_target_biquads, &s_target_preamp_linear);

    // Reset target filter state delay lines to prevent state injection pops
    for (int i = 0; i < PEQ_MAX_BANDS; i++) {
        s_target_biquads[i].w1_l = 0.0f;
        s_target_biquads[i].w2_l = 0.0f;
        s_target_biquads[i].w1_r = 0.0f;
        s_target_biquads[i].w2_r = 0.0f;
    }

    s_active_preset = preset_id;
    s_crossfade_progress = 0.0f;
    s_in_crossfade = true;
}

peq_preset_id_t peq_dsp_cycle_preset(void)
{
    peq_preset_id_t next = (s_active_preset + 1) % PEQ_PRESET_COUNT;
    peq_dsp_select_preset(next);
    return next;
}

peq_preset_id_t peq_dsp_get_current_preset(char *out_name)
{
    if (out_name) {
        strncpy(out_name, s_profiles[s_active_preset].name, 24);
    }
    return s_active_preset;
}

esp_err_t peq_dsp_update_user_band(int band_idx, const peq_band_param_t *param)
{
    if (band_idx < 0 || band_idx >= PEQ_MAX_BANDS || !param) return ESP_ERR_INVALID_ARG;

    // Dynamically expand band count if a higher band index is updated
    if (band_idx >= s_profiles[PEQ_PRESET_USER_CUSTOM].num_bands) {
        s_profiles[PEQ_PRESET_USER_CUSTOM].num_bands = (uint8_t)(band_idx + 1);
        ESP_LOGI(TAG, "PEQ User Custom band count dynamically expanded to %d", band_idx + 1);
    }
    s_profiles[PEQ_PRESET_USER_CUSTOM].bands[band_idx] = *param;

    if (s_active_preset == PEQ_PRESET_USER_CUSTOM) {
        s_target_num_bands = s_profiles[PEQ_PRESET_USER_CUSTOM].num_bands;
        apply_profile_to_biquads(&s_profiles[PEQ_PRESET_USER_CUSTOM], s_target_biquads, &s_target_preamp_linear);
        s_crossfade_progress = 0.0f;
        s_in_crossfade = true;
    }
    return ESP_OK;
}


esp_err_t peq_dsp_save_user_profile_nvs(void)
{
    nvs_handle_t nvs_h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_h);
    if (err != ESP_OK) return err;

    // Check if changed before writing flash (Rule 7: prevent flash wear)
    size_t size = sizeof(peq_profile_t);
    peq_profile_t existing;
    bool needs_write = true;
    if (nvs_get_blob(nvs_h, NVS_KEY_USER, &existing, &size) == ESP_OK && size == sizeof(peq_profile_t)) {
        if (memcmp(&existing, &s_profiles[PEQ_PRESET_USER_CUSTOM], sizeof(peq_profile_t)) == 0) {
            needs_write = false; // Identical, skip commit
        }
    }

    if (needs_write) {
        err = nvs_set_blob(nvs_h, NVS_KEY_USER, &s_profiles[PEQ_PRESET_USER_CUSTOM], sizeof(peq_profile_t));
        if (err == ESP_OK) {
            err = nvs_commit(nvs_h);
            ESP_LOGI(TAG, "User Custom EQ profile committed to NVS successfully.");
        }
    } else {
        ESP_LOGI(TAG, "User Custom profile unmodified, skipping flash write.");
    }
    nvs_close(nvs_h);
    return err;
}

esp_err_t peq_dsp_get_profile(peq_preset_id_t preset_id, peq_profile_t *out_profile)
{
    if (preset_id >= PEQ_PRESET_COUNT || !out_profile) return ESP_ERR_INVALID_ARG;
    memcpy(out_profile, &s_profiles[preset_id], sizeof(peq_profile_t));
    return ESP_OK;
}

esp_err_t peq_dsp_set_preamp(float gain_db)
{
    s_profiles[PEQ_PRESET_USER_CUSTOM].preamp_gain_db = gain_db;
    if (s_active_preset == PEQ_PRESET_USER_CUSTOM) {
        apply_profile_to_biquads(&s_profiles[PEQ_PRESET_USER_CUSTOM], s_target_biquads, &s_target_preamp_linear);
        s_crossfade_progress = 0.0f;
        s_in_crossfade = true;
    }
    return ESP_OK;
}

int peq_dsp_get_band_count(void)
{
    return s_active_num_bands;
}

esp_err_t peq_dsp_set_band_count(int num_bands)
{
    if (num_bands < 1 || num_bands > PEQ_MAX_BANDS) return ESP_ERR_INVALID_ARG;

    s_profiles[PEQ_PRESET_USER_CUSTOM].num_bands = (uint8_t)num_bands;
    if (s_active_preset == PEQ_PRESET_USER_CUSTOM) {
        s_target_num_bands = num_bands;
        apply_profile_to_biquads(&s_profiles[PEQ_PRESET_USER_CUSTOM], s_target_biquads, &s_target_preamp_linear);
        s_crossfade_progress = 0.0f;
        s_in_crossfade = true;
    }
    ESP_LOGI(TAG, "PEQ User Custom band count set to %d", num_bands);
    return ESP_OK;
}

// In-line Direct Form II Transposed biquad filter step for single sample
static inline float biquad_filter_sample(biquad_state_t *bq, float in, float *w1, float *w2)
{
    float out = bq->b0 * in + *w1;
    *w1 = bq->b1 * in - bq->a1 * out + *w2;
    *w2 = bq->b2 * in - bq->a2 * out;
    return out;
}

void peq_dsp_process_interleaved_s16(int16_t *samples, size_t num_frames)
{
    if (!samples || num_frames == 0) return;

    // Optimization: If Flat preset is selected and not crossfading, bypass processing
    if (s_active_preset == PEQ_PRESET_FLAT && !s_in_crossfade) {
        return;
    }

    for (size_t f = 0; f < num_frames; f++) {
        float in_l = (float)samples[2 * f];
        float in_r = (float)samples[2 * f + 1];

        // 1. Process active biquad cascade (dynamically scaled to s_active_num_bands)
        float act_l = in_l * s_active_preamp_linear;
        float act_r = in_r * s_active_preamp_linear;
        int act_count = s_active_num_bands;
        for (int b = 0; b < act_count; b++) {
            act_l = biquad_filter_sample(&s_active_biquads[b], act_l, &s_active_biquads[b].w1_l, &s_active_biquads[b].w2_l);
            act_r = biquad_filter_sample(&s_active_biquads[b], act_r, &s_active_biquads[b].w1_r, &s_active_biquads[b].w2_r);
        }

        float out_l = act_l;
        float out_r = act_r;

        // 2. If in crossfade, also process target cascade and linearly interpolate
        if (s_in_crossfade) {
            float tgt_l = in_l * s_target_preamp_linear;
            float tgt_r = in_r * s_target_preamp_linear;
            int tgt_count = s_target_num_bands;
            for (int b = 0; b < tgt_count; b++) {
                tgt_l = biquad_filter_sample(&s_target_biquads[b], tgt_l, &s_target_biquads[b].w1_l, &s_target_biquads[b].w2_l);
                tgt_r = biquad_filter_sample(&s_target_biquads[b], tgt_r, &s_target_biquads[b].w1_r, &s_target_biquads[b].w2_r);
            }

            float p = s_crossfade_progress;
            out_l = (1.0f - p) * act_l + p * tgt_l;
            out_r = (1.0f - p) * act_r + p * tgt_r;

            s_crossfade_progress += s_crossfade_step;
            if (s_crossfade_progress >= 1.0f) {
                // Crossfade complete: commit target biquads and band count to active
                memcpy(s_active_biquads, s_target_biquads, sizeof(s_active_biquads));
                s_active_preamp_linear = s_target_preamp_linear;
                s_active_num_bands = s_target_num_bands;
                s_in_crossfade = false;
            }
        }

        // 3. Strict saturation clamping against [-32768, 32767] (Invariant 8)
        samples[2 * f]     = clamp_s16(out_l);
        samples[2 * f + 1] = clamp_s16(out_r);
    }
}

