#include "peq_dsp.h"

#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <math.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "nvs.h"

#define TAG "PEQ_DSP"
#define NVS_NAMESPACE   "audio_peq"
#define NVS_KEY_USER_V1 "user_profile"
#define NVS_KEY_USER_V2 "user_cfg_v2"
#define PEQ_NVS_MAGIC_V2   0x50455132UL // "PEQ2"
#define PEQ_NVS_VERSION_V2 2U

#define CROSSFEED_MAX_DELAY 32

typedef struct {
    float b0, b1, b2, a1, a2;
    float w1_l, w2_l;
    float w1_r, w2_r;
} biquad_state_t;

typedef struct {
    float m_ll, m_rl;
    float m_lr, m_rr;
    bool  crossfeed_en;
    float crossfeed_gain;
    float cf_alpha;
    int   cf_delay_samples;
} mixer_rt_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint8_t  linked_stereo;
    uint8_t  reserved;
    peq_profile_t left;
    peq_profile_t right;
    peq_mixer_config_t mixer;
    uint32_t crc32;
} peq_nvs_v2_t;

static portMUX_TYPE s_peq_lock = portMUX_INITIALIZER_UNLOCKED;

static peq_profile_t s_profiles_l[PEQ_PRESET_COUNT];
static peq_profile_t s_user_right;
static bool s_linked_stereo = true;
static bool s_ab_bypass = false;
static peq_preset_id_t s_active_preset = PEQ_PRESET_FLAT;
static peq_mixer_config_t s_mixer;
static int s_sample_rate = 48000;
static uint32_t s_state_gen = 0;

// Staged pending updates (written on Core 0 under s_peq_lock, consumed by Core 1)
static biquad_state_t s_pending_biquads_l[PEQ_MAX_BANDS];
static biquad_state_t s_pending_biquads_r[PEQ_MAX_BANDS];
static int s_pending_num_bands_l = PEQ_DEFAULT_NUM_BANDS;
static int s_pending_num_bands_r = PEQ_DEFAULT_NUM_BANDS;
static float s_pending_preamp_linear = 1.0f;
static bool s_pending_is_flat = true;
static bool s_pending_reset_states = false;
static bool s_pending_update = false;
static float s_pending_crossfade_step = 0.0f;
static mixer_rt_t s_pending_mixer_rt;
static bool s_pending_mixer_update = false;

// Core 1 DSP runtime state (owned exclusively by audio_dsp_task after init)
static int s_active_num_bands_l = PEQ_DEFAULT_NUM_BANDS;
static int s_active_num_bands_r = PEQ_DEFAULT_NUM_BANDS;
static int s_target_num_bands_l = PEQ_DEFAULT_NUM_BANDS;
static int s_target_num_bands_r = PEQ_DEFAULT_NUM_BANDS;

static biquad_state_t s_active_biquads_l[PEQ_MAX_BANDS];
static biquad_state_t s_active_biquads_r[PEQ_MAX_BANDS];
static biquad_state_t s_target_biquads_l[PEQ_MAX_BANDS];
static biquad_state_t s_target_biquads_r[PEQ_MAX_BANDS];

static float s_active_preamp_linear = 1.0f;
static float s_target_preamp_linear = 1.0f;

static bool s_active_is_flat = true;
static bool s_target_is_flat = true;

static bool s_in_crossfade = false;
static float s_crossfade_progress = 0.0f;
static float s_crossfade_step = 0.0f;

static mixer_rt_t s_mixer_rt;
static float s_cf_delay_l[CROSSFEED_MAX_DELAY];
static float s_cf_delay_r[CROSSFEED_MAX_DELAY];
static int s_cf_idx = 0;
static float s_cf_lp_l = 0.0f;
static float s_cf_lp_r = 0.0f;

static uint32_t calc_crc32(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFUL;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc >> 1) ^ ((crc & 1U) ? 0xEDB88320UL : 0UL);
        }
    }
    return ~crc;
}

static inline int16_t clamp_s16(float val)
{
    if (!isfinite(val)) return 0;
    if (val > 32767.0f) return 32767;
    if (val < -32768.0f) return -32768;
    return (int16_t)roundf(val);
}

static inline int32_t clamp_s24_to_s32(float val)
{
    if (!isfinite(val)) return 0;
    if (val > 8388607.0f) return 0x7FFFFF00L;
    if (val < -8388608.0f) return (int32_t)0x80000000UL;
    return (int32_t)((uint32_t)(int32_t)lrintf(val) << 8);
}

static inline void set_passthrough_coeffs(biquad_state_t *out_bq)
{
    out_bq->b0 = 1.0f;
    out_bq->b1 = 0.0f;
    out_bq->b2 = 0.0f;
    out_bq->a1 = 0.0f;
    out_bq->a2 = 0.0f;
}

static bool filter_uses_gain(peq_filter_type_t type)
{
    return (type == PEQ_FILTER_PEAKING ||
            type == PEQ_FILTER_LOW_SHELF ||
            type == PEQ_FILTER_HIGH_SHELF);
}

static void calculate_biquad(const peq_band_param_t *band, float sample_rate, biquad_state_t *out_bq)
{
    if (!out_bq) return;

    if (!band || !band->enabled || band->type == PEQ_FILTER_BYPASS ||
        !isfinite(sample_rate) || sample_rate <= 0.0f ||
        !isfinite(band->freq_hz) || !isfinite(band->gain_db) || !isfinite(band->q_factor)) {
        set_passthrough_coeffs(out_bq);
        return;
    }

    if (filter_uses_gain(band->type) && fabsf(band->gain_db) < 0.05f) {
        set_passthrough_coeffs(out_bq);
        return;
    }

    float f0 = band->freq_hz;
    if (f0 < 10.0f) f0 = 10.0f;
    if (f0 >= sample_rate * 0.49f) f0 = sample_rate * 0.49f;

    float q = band->q_factor;
    if (q < 0.05f) q = 0.05f;
    if (q > 10.0f) q = 10.0f;

    float gain_db = band->gain_db;
    if (gain_db < -24.0f) gain_db = -24.0f;
    if (gain_db > 12.0f) gain_db = 12.0f;

    float omega = 2.0f * (float)M_PI * f0 / sample_rate;
    float cos_w = cosf(omega);
    float sin_w = sinf(omega);
    float alpha = sin_w / (2.0f * q);
    float A = powf(10.0f, gain_db / 40.0f);

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

    case PEQ_FILTER_HIGH_PASS:
        b0 = (1.0f + cos_w) * 0.5f;
        b1 = -(1.0f + cos_w);
        b2 = (1.0f + cos_w) * 0.5f;
        a0 = 1.0f + alpha;
        a1 = -2.0f * cos_w;
        a2 = 1.0f - alpha;
        break;

    case PEQ_FILTER_LOW_PASS:
        b0 = (1.0f - cos_w) * 0.5f;
        b1 = 1.0f - cos_w;
        b2 = (1.0f - cos_w) * 0.5f;
        a0 = 1.0f + alpha;
        a1 = -2.0f * cos_w;
        a2 = 1.0f - alpha;
        break;

    case PEQ_FILTER_NOTCH:
        b0 = 1.0f;
        b1 = -2.0f * cos_w;
        b2 = 1.0f;
        a0 = 1.0f + alpha;
        a1 = -2.0f * cos_w;
        a2 = 1.0f - alpha;
        break;

    case PEQ_FILTER_BAND_PASS:
        b0 = alpha;
        b1 = 0.0f;
        b2 = -alpha;
        a0 = 1.0f + alpha;
        a1 = -2.0f * cos_w;
        a2 = 1.0f - alpha;
        break;

    default:
        set_passthrough_coeffs(out_bq);
        return;
    }

    if (!isfinite(a0) || fabsf(a0) < 1e-12f) {
        set_passthrough_coeffs(out_bq);
        return;
    }

    float nb0 = b0 / a0;
    float nb1 = b1 / a0;
    float nb2 = b2 / a0;
    float na1 = a1 / a0;
    float na2 = a2 / a0;

    if (!isfinite(nb0) || !isfinite(nb1) || !isfinite(nb2) || !isfinite(na1) || !isfinite(na2)) {
        set_passthrough_coeffs(out_bq);
        return;
    }

    out_bq->b0 = nb0;
    out_bq->b1 = nb1;
    out_bq->b2 = nb2;
    out_bq->a1 = na1;
    out_bq->a2 = na2;
}

static bool biquad_is_passthrough(const biquad_state_t *bq)
{
    return (bq->b0 == 1.0f && bq->b1 == 0.0f && bq->b2 == 0.0f &&
            bq->a1 == 0.0f && bq->a2 == 0.0f);
}

static void apply_profile_to_biquads(const peq_profile_t *prof, float sample_rate, bool ab_bypass,
                                     biquad_state_t *target, float *preamp_lin,
                                     int *out_num_bands, bool *out_is_flat)
{
    memset(target, 0, sizeof(biquad_state_t) * PEQ_MAX_BANDS);

    float preamp = 1.0f;
    if (isfinite(prof->preamp_gain_db)) {
        float lin = powf(10.0f, prof->preamp_gain_db / 20.0f);
        if (isfinite(lin) && lin >= 0.0f) {
            preamp = lin;
        }
    }
    *preamp_lin = preamp;

    int count = (prof->num_bands > 0 && prof->num_bands <= PEQ_MAX_BANDS)
                ? prof->num_bands : PEQ_DEFAULT_NUM_BANDS;
    *out_num_bands = ab_bypass ? 0 : count;

    bool is_flat = (!isfinite(prof->preamp_gain_db) || fabsf(prof->preamp_gain_db) < 1e-4f);
    for (int i = 0; i < PEQ_MAX_BANDS; i++) {
        if (!ab_bypass && i < count) {
            calculate_biquad(&prof->bands[i], sample_rate, &target[i]);
            if (!biquad_is_passthrough(&target[i])) {
                is_flat = false;
            }
        } else {
            set_passthrough_coeffs(&target[i]);
        }
    }
    *out_is_flat = is_flat;
}

static void compute_mixer_rt(const peq_mixer_config_t *cfg, float sample_rate, mixer_rt_t *out)
{
    float m_ll = 1.0f, m_rl = 0.0f;
    float m_lr = 0.0f, m_rr = 1.0f;
    bool cf_en = false;
    float cf_gain = 0.0f;

    switch (cfg->mode) {
    case MIXER_MODE_MONO:
        m_ll = 0.5f; m_rl = 0.5f;
        m_lr = 0.5f; m_rr = 0.5f;
        break;
    case MIXER_MODE_SWAP:
        m_ll = 0.0f; m_rl = 1.0f;
        m_lr = 1.0f; m_rr = 0.0f;
        break;
    case MIXER_MODE_WIDTH: {
        float w = (float)cfg->width_pct / 100.0f;
        if (w < 0.0f) w = 0.0f;
        if (w > 2.0f) w = 2.0f;
        m_ll = 0.5f * (1.0f + w);
        m_rl = 0.5f * (1.0f - w);
        m_lr = 0.5f * (1.0f - w);
        m_rr = 0.5f * (1.0f + w);
        break;
    }
    case MIXER_MODE_CROSSFEED:
        cf_en = true;
        if (cfg->crossfeed_level >= 2) {
            cf_gain = 0.5012f; // -6.0 dB
        } else {
            cf_gain = 0.5957f; // -4.5 dB
        }
        break;
    case MIXER_MODE_CUSTOM_MATRIX:
        m_ll = isfinite(cfg->custom_matrix[0][0]) ? cfg->custom_matrix[0][0] : 1.0f;
        m_rl = isfinite(cfg->custom_matrix[0][1]) ? cfg->custom_matrix[0][1] : 0.0f;
        m_lr = isfinite(cfg->custom_matrix[1][0]) ? cfg->custom_matrix[1][0] : 0.0f;
        m_rr = isfinite(cfg->custom_matrix[1][1]) ? cfg->custom_matrix[1][1] : 1.0f;
        if (m_ll > 4.0f) m_ll = 4.0f; else if (m_ll < -4.0f) m_ll = -4.0f;
        if (m_rl > 4.0f) m_rl = 4.0f; else if (m_rl < -4.0f) m_rl = -4.0f;
        if (m_lr > 4.0f) m_lr = 4.0f; else if (m_lr < -4.0f) m_lr = -4.0f;
        if (m_rr > 4.0f) m_rr = 4.0f; else if (m_rr < -4.0f) m_rr = -4.0f;
        break;
    case MIXER_MODE_STEREO:
    default:
        if (cfg->crossfeed_level > 0) {
            cf_en = true;
            cf_gain = (cfg->crossfeed_level >= 2) ? 0.5012f : 0.5957f;
        }
        break;
    }

    // Apply stereo balance (-100..+100)
    int bal = cfg->balance;
    if (bal > 100) bal = 100;
    if (bal < -100) bal = -100;
    if (bal > 0) {
        float l_att = 1.0f - ((float)bal / 100.0f);
        m_ll *= l_att;
        m_rl *= l_att;
    } else if (bal < 0) {
        float r_att = 1.0f - ((float)(-bal) / 100.0f);
        m_lr *= r_att;
        m_rr *= r_att;
    }

    float sr = (sample_rate > 0.0f) ? sample_rate : 48000.0f;
    float alpha = expf(-2.0f * (float)M_PI * 700.0f / sr);
    int delay_n = (int)lroundf(0.00027f * sr);
    if (delay_n < 1) delay_n = 1;
    if (delay_n >= CROSSFEED_MAX_DELAY) delay_n = CROSSFEED_MAX_DELAY - 1;

    out->m_ll = m_ll;
    out->m_rl = m_rl;
    out->m_lr = m_lr;
    out->m_rr = m_rr;
    out->crossfeed_en = cf_en;
    out->crossfeed_gain = cf_gain;
    out->cf_alpha = alpha;
    out->cf_delay_samples = delay_n;
}

static void setup_default_mixer(peq_mixer_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->mode = MIXER_MODE_STEREO;
    cfg->balance = 0;
    cfg->width_pct = 100;
    cfg->crossfeed_level = 0;
    cfg->custom_matrix[0][0] = 1.0f;
    cfg->custom_matrix[0][1] = 0.0f;
    cfg->custom_matrix[1][0] = 0.0f;
    cfg->custom_matrix[1][1] = 1.0f;
}

static void setup_factory_presets(void)
{
    // Preset 0: Flat / Bypass (0 dB)
    memset(&s_profiles_l[PEQ_PRESET_FLAT], 0, sizeof(peq_profile_t));
    snprintf(s_profiles_l[PEQ_PRESET_FLAT].name, sizeof(s_profiles_l[PEQ_PRESET_FLAT].name), "Flat / Bypass");
    s_profiles_l[PEQ_PRESET_FLAT].num_bands = PEQ_DEFAULT_NUM_BANDS;
    s_profiles_l[PEQ_PRESET_FLAT].preamp_gain_db = 0.0f;
    for (int i = 0; i < PEQ_MAX_BANDS; i++) {
        s_profiles_l[PEQ_PRESET_FLAT].bands[i].type = PEQ_FILTER_BYPASS;
        s_profiles_l[PEQ_PRESET_FLAT].bands[i].enabled = false;
    }

    // Preset 1: Warm Bass Boost (+4.5 dB)
    memset(&s_profiles_l[PEQ_PRESET_BASS_BOOST], 0, sizeof(peq_profile_t));
    snprintf(s_profiles_l[PEQ_PRESET_BASS_BOOST].name, sizeof(s_profiles_l[PEQ_PRESET_BASS_BOOST].name), "Bass Boost");
    s_profiles_l[PEQ_PRESET_BASS_BOOST].num_bands = PEQ_DEFAULT_NUM_BANDS;
    s_profiles_l[PEQ_PRESET_BASS_BOOST].preamp_gain_db = -2.5f;
    s_profiles_l[PEQ_PRESET_BASS_BOOST].bands[0] = (peq_band_param_t){PEQ_FILTER_LOW_SHELF, 80.0f, 4.5f, 0.71f, true};
    s_profiles_l[PEQ_PRESET_BASS_BOOST].bands[1] = (peq_band_param_t){PEQ_FILTER_PEAKING, 250.0f, 1.5f, 1.0f, true};
    s_profiles_l[PEQ_PRESET_BASS_BOOST].bands[2] = (peq_band_param_t){PEQ_FILTER_PEAKING, 1000.0f, 0.0f, 1.0f, false};
    s_profiles_l[PEQ_PRESET_BASS_BOOST].bands[3] = (peq_band_param_t){PEQ_FILTER_PEAKING, 4000.0f, -1.0f, 1.0f, true};
    s_profiles_l[PEQ_PRESET_BASS_BOOST].bands[4] = (peq_band_param_t){PEQ_FILTER_HIGH_SHELF, 10000.0f, 0.5f, 0.71f, true};
    for (int i = 5; i < PEQ_MAX_BANDS; i++) {
        s_profiles_l[PEQ_PRESET_BASS_BOOST].bands[i].type = PEQ_FILTER_BYPASS;
        s_profiles_l[PEQ_PRESET_BASS_BOOST].bands[i].enabled = false;
    }

    // Preset 2: Harman Target (OE2018 Curve)
    memset(&s_profiles_l[PEQ_PRESET_HARMAN], 0, sizeof(peq_profile_t));
    snprintf(s_profiles_l[PEQ_PRESET_HARMAN].name, sizeof(s_profiles_l[PEQ_PRESET_HARMAN].name), "Harman Target");
    s_profiles_l[PEQ_PRESET_HARMAN].num_bands = PEQ_DEFAULT_NUM_BANDS;
    s_profiles_l[PEQ_PRESET_HARMAN].preamp_gain_db = -4.0f;
    s_profiles_l[PEQ_PRESET_HARMAN].bands[0] = (peq_band_param_t){PEQ_FILTER_LOW_SHELF, 105.0f, 5.0f, 0.71f, true};
    s_profiles_l[PEQ_PRESET_HARMAN].bands[1] = (peq_band_param_t){PEQ_FILTER_PEAKING, 200.0f, -1.5f, 1.2f, true};
    s_profiles_l[PEQ_PRESET_HARMAN].bands[2] = (peq_band_param_t){PEQ_FILTER_PEAKING, 1200.0f, 1.0f, 1.4f, true};
    s_profiles_l[PEQ_PRESET_HARMAN].bands[3] = (peq_band_param_t){PEQ_FILTER_PEAKING, 3000.0f, 2.5f, 1.8f, true};
    s_profiles_l[PEQ_PRESET_HARMAN].bands[4] = (peq_band_param_t){PEQ_FILTER_HIGH_SHELF, 8000.0f, -1.0f, 0.71f, true};
    for (int i = 5; i < PEQ_MAX_BANDS; i++) {
        s_profiles_l[PEQ_PRESET_HARMAN].bands[i].type = PEQ_FILTER_BYPASS;
        s_profiles_l[PEQ_PRESET_HARMAN].bands[i].enabled = false;
    }

    // Preset 3: User Custom (Defaults to Flat until customized or loaded from NVS)
    memset(&s_profiles_l[PEQ_PRESET_USER_CUSTOM], 0, sizeof(peq_profile_t));
    snprintf(s_profiles_l[PEQ_PRESET_USER_CUSTOM].name, sizeof(s_profiles_l[PEQ_PRESET_USER_CUSTOM].name), "User Custom");
    s_profiles_l[PEQ_PRESET_USER_CUSTOM].num_bands = PEQ_DEFAULT_NUM_BANDS;
    s_profiles_l[PEQ_PRESET_USER_CUSTOM].preamp_gain_db = 0.0f;
    for (int i = 0; i < PEQ_MAX_BANDS; i++) {
        s_profiles_l[PEQ_PRESET_USER_CUSTOM].bands[i].type = PEQ_FILTER_BYPASS;
        s_profiles_l[PEQ_PRESET_USER_CUSTOM].bands[i].enabled = false;
    }

    s_user_right = s_profiles_l[PEQ_PRESET_USER_CUSTOM];
    s_linked_stereo = true;
    setup_default_mixer(&s_mixer);
}

static void sanitize_profile(peq_profile_t *prof)
{
    if (prof->num_bands == 0 || prof->num_bands > PEQ_MAX_BANDS) {
        prof->num_bands = PEQ_DEFAULT_NUM_BANDS;
    }
    if (!isfinite(prof->preamp_gain_db)) {
        prof->preamp_gain_db = 0.0f;
    }
    prof->name[sizeof(prof->name) - 1] = '\0';
}

static void load_user_profile_nvs(void)
{
    nvs_handle_t nvs_h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_h);
    if (err != ESP_OK) return;

    // 1. Try NVS v2 (Dual-Mono L/R PEQ + Matrix/Crossfeed)
    size_t req_v2 = sizeof(peq_nvs_v2_t);
    peq_nvs_v2_t v2;
    if (nvs_get_blob(nvs_h, NVS_KEY_USER_V2, &v2, &req_v2) == ESP_OK &&
        req_v2 == sizeof(peq_nvs_v2_t) &&
        v2.magic == PEQ_NVS_MAGIC_V2 &&
        v2.version == PEQ_NVS_VERSION_V2) {
        uint32_t expected_crc = calc_crc32((const uint8_t *)&v2, offsetof(peq_nvs_v2_t, crc32));
        if (expected_crc == v2.crc32) {
            sanitize_profile(&v2.left);
            sanitize_profile(&v2.right);
            s_profiles_l[PEQ_PRESET_USER_CUSTOM] = v2.left;
            s_linked_stereo = (v2.linked_stereo != 0);
            s_user_right = s_linked_stereo ? v2.left : v2.right;
            if ((unsigned int)v2.mixer.mode < MIXER_MODE_COUNT) {
                s_mixer = v2.mixer;
            }
            ESP_LOGI(TAG, "Loaded NVS v2 profile (linked=%d, L=%d bands, R=%d bands, mixer=%d)",
                     s_linked_stereo ? 1 : 0, s_profiles_l[PEQ_PRESET_USER_CUSTOM].num_bands,
                     s_user_right.num_bands, s_mixer.mode);
            nvs_close(nvs_h);
            return;
        }
        ESP_LOGW(TAG, "NVS v2 CRC mismatch; checking legacy v1 profile.");
    }

    // 2. Fallback & auto-migrate legacy NVS v1 profile
    size_t req_v1 = sizeof(peq_profile_t);
    peq_profile_t loaded_v1;
    if (nvs_get_blob(nvs_h, NVS_KEY_USER_V1, &loaded_v1, &req_v1) == ESP_OK &&
        req_v1 == sizeof(peq_profile_t)) {
        sanitize_profile(&loaded_v1);
        s_profiles_l[PEQ_PRESET_USER_CUSTOM] = loaded_v1;
        s_user_right = loaded_v1;
        s_linked_stereo = true;
        ESP_LOGI(TAG, "Migrated legacy NVS v1 User Custom profile: \"%s\" (%d bands)",
                 loaded_v1.name, loaded_v1.num_bands);
    }

    nvs_close(nvs_h);
}

static void stage_pending_from_active(bool reset_states, bool stage_mixer)
{
    for (;;) {
        taskENTER_CRITICAL(&s_peq_lock);
        uint32_t gen = s_state_gen;
        int sr = s_sample_rate;
        peq_preset_id_t preset = s_active_preset;
        bool ab_bypass = s_ab_bypass;
        bool linked = s_linked_stereo;
        peq_profile_t prof_l = s_profiles_l[preset];
        peq_profile_t prof_r = (preset == PEQ_PRESET_USER_CUSTOM && !linked) ? s_user_right : prof_l;
        peq_mixer_config_t mixer_snap = s_mixer;
        taskEXIT_CRITICAL(&s_peq_lock);

        float step = (sr > 0) ? (1.0f / (0.015f * (float)sr)) : 1.0f;
        if (!isfinite(step) || step <= 0.0f) step = 1.0f;

        biquad_state_t local_bq_l[PEQ_MAX_BANDS];
        biquad_state_t local_bq_r[PEQ_MAX_BANDS];
        float local_preamp_l = 1.0f;
        float local_preamp_r = 1.0f;
        int local_nb_l = PEQ_DEFAULT_NUM_BANDS;
        int local_nb_r = PEQ_DEFAULT_NUM_BANDS;
        bool local_flat_l = true;
        bool local_flat_r = true;

        apply_profile_to_biquads(&prof_l, (float)sr, ab_bypass, local_bq_l,
                                 &local_preamp_l, &local_nb_l, &local_flat_l);
        apply_profile_to_biquads(&prof_r, (float)sr, ab_bypass, local_bq_r,
                                 &local_preamp_r, &local_nb_r, &local_flat_r);

        mixer_rt_t local_mixer;
        if (stage_mixer) {
            compute_mixer_rt(&mixer_snap, (float)sr, &local_mixer);
        }

        taskENTER_CRITICAL(&s_peq_lock);
        if (s_state_gen == gen) {
            s_pending_crossfade_step = step;
            memcpy(s_pending_biquads_l, local_bq_l, sizeof(s_pending_biquads_l));
            memcpy(s_pending_biquads_r, local_bq_r, sizeof(s_pending_biquads_r));
            s_pending_num_bands_l = local_nb_l;
            s_pending_num_bands_r = local_nb_r;
            s_pending_preamp_linear = local_preamp_l;
            s_pending_is_flat = local_flat_l && local_flat_r;
            s_pending_reset_states = (s_pending_update && s_pending_reset_states) || reset_states;
            s_pending_update = true;
            if (stage_mixer) {
                s_pending_mixer_rt = local_mixer;
                s_pending_mixer_update = true;
            }
            taskEXIT_CRITICAL(&s_peq_lock);
            return;
        }
        taskEXIT_CRITICAL(&s_peq_lock);
    }
}

esp_err_t peq_dsp_init(int sample_rate)
{
    if (sample_rate <= 0) sample_rate = 48000;

    setup_factory_presets();
    load_user_profile_nvs();

    float crossfade_step = 1.0f / (0.015f * (float)sample_rate);
    biquad_state_t init_bq[PEQ_MAX_BANDS];
    float init_preamp = 1.0f;
    int init_nb = PEQ_DEFAULT_NUM_BANDS;
    bool init_flat = true;

    apply_profile_to_biquads(&s_profiles_l[PEQ_PRESET_FLAT], (float)sample_rate, false,
                             init_bq, &init_preamp, &init_nb, &init_flat);

    mixer_rt_t init_mixer;
    compute_mixer_rt(&s_mixer, (float)sample_rate, &init_mixer);

    taskENTER_CRITICAL(&s_peq_lock);
    s_sample_rate = sample_rate;
    s_state_gen = 0;
    s_active_preset = PEQ_PRESET_FLAT;
    s_ab_bypass = false;

    s_crossfade_step = crossfade_step;
    s_pending_crossfade_step = crossfade_step;

    memcpy(s_active_biquads_l, init_bq, sizeof(s_active_biquads_l));
    memcpy(s_active_biquads_r, init_bq, sizeof(s_active_biquads_r));
    memcpy(s_target_biquads_l, init_bq, sizeof(s_target_biquads_l));
    memcpy(s_target_biquads_r, init_bq, sizeof(s_target_biquads_r));
    s_active_num_bands_l = init_nb;
    s_active_num_bands_r = init_nb;
    s_target_num_bands_l = init_nb;
    s_target_num_bands_r = init_nb;
    s_active_preamp_linear = init_preamp;
    s_target_preamp_linear = init_preamp;
    s_active_is_flat = init_flat;
    s_target_is_flat = init_flat;
    s_in_crossfade = false;
    s_crossfade_progress = 0.0f;
    s_pending_update = false;
    s_pending_reset_states = false;

    s_mixer_rt = init_mixer;
    s_pending_mixer_rt = init_mixer;
    s_pending_mixer_update = false;
    memset(s_cf_delay_l, 0, sizeof(s_cf_delay_l));
    memset(s_cf_delay_r, 0, sizeof(s_cf_delay_r));
    s_cf_idx = 0;
    s_cf_lp_l = 0.0f;
    s_cf_lp_r = 0.0f;
    taskEXIT_CRITICAL(&s_peq_lock);

    ESP_LOGI(TAG, "PEQ DSP Engine initialized at %d Hz (Active: %s, Mixer mode: %d)",
             sample_rate, s_profiles_l[PEQ_PRESET_FLAT].name, s_mixer.mode);
    return ESP_OK;
}

void peq_dsp_set_sample_rate(int sample_rate)
{
    if (sample_rate <= 0) return;

    taskENTER_CRITICAL(&s_peq_lock);
    if (sample_rate == s_sample_rate) {
        taskEXIT_CRITICAL(&s_peq_lock);
        return;
    }
    s_sample_rate = sample_rate;
    s_state_gen++;
    taskEXIT_CRITICAL(&s_peq_lock);

    stage_pending_from_active(true, true);
}

void peq_dsp_select_preset(peq_preset_id_t preset_id)
{
    if ((unsigned int)preset_id >= PEQ_PRESET_COUNT) return;

    peq_profile_t prev_prof, next_prof;
    taskENTER_CRITICAL(&s_peq_lock);
    if (preset_id == s_active_preset) {
        taskEXIT_CRITICAL(&s_peq_lock);
        return;
    }
    prev_prof = s_profiles_l[s_active_preset];
    next_prof = s_profiles_l[preset_id];
    s_active_preset = preset_id;
    s_state_gen++;
    taskEXIT_CRITICAL(&s_peq_lock);

    prev_prof.name[sizeof(prev_prof.name) - 1] = '\0';
    next_prof.name[sizeof(next_prof.name) - 1] = '\0';
    ESP_LOGI(TAG, "Switching PEQ preset: \"%s\" -> \"%s\" (15ms crossfade)",
             prev_prof.name, next_prof.name);

    stage_pending_from_active(true, false);
}

peq_preset_id_t peq_dsp_cycle_preset(void)
{
    taskENTER_CRITICAL(&s_peq_lock);
    peq_preset_id_t next = (peq_preset_id_t)((s_active_preset + 1) % PEQ_PRESET_COUNT);
    taskEXIT_CRITICAL(&s_peq_lock);

    peq_dsp_select_preset(next);
    return next;
}

peq_preset_id_t peq_dsp_get_current_preset(char *out_name)
{
    peq_preset_id_t preset;
    char name_buf[24];

    taskENTER_CRITICAL(&s_peq_lock);
    preset = s_active_preset;
    if (out_name) {
        memcpy(name_buf, s_profiles_l[preset].name, sizeof(name_buf));
    }
    taskEXIT_CRITICAL(&s_peq_lock);

    if (out_name) {
        memcpy(out_name, name_buf, sizeof(name_buf) - 1);
        out_name[sizeof(name_buf) - 1] = '\0';
    }
    return preset;
}

esp_err_t peq_dsp_update_user_band_ch(peq_channel_t ch, int band_idx, const peq_band_param_t *param)
{
    if (band_idx < 0 || band_idx >= PEQ_MAX_BANDS || !param) return ESP_ERR_INVALID_ARG;
    if (param->enabled && (!isfinite(param->freq_hz) || !isfinite(param->gain_db) || !isfinite(param->q_factor))) {
        return ESP_ERR_INVALID_ARG;
    }

    bool is_active_user = false;
    taskENTER_CRITICAL(&s_peq_lock);
    bool write_l = (ch == PEQ_CH_LINKED || ch == PEQ_CH_LEFT || s_linked_stereo);
    bool write_r = (ch == PEQ_CH_LINKED || ch == PEQ_CH_RIGHT || s_linked_stereo);

    if (write_l) {
        if (band_idx >= s_profiles_l[PEQ_PRESET_USER_CUSTOM].num_bands) {
            s_profiles_l[PEQ_PRESET_USER_CUSTOM].num_bands = (uint8_t)(band_idx + 1);
        }
        s_profiles_l[PEQ_PRESET_USER_CUSTOM].bands[band_idx] = *param;
    }
    if (write_r) {
        if (band_idx >= s_user_right.num_bands) {
            s_user_right.num_bands = (uint8_t)(band_idx + 1);
        }
        s_user_right.bands[band_idx] = *param;
    }

    s_state_gen++;
    is_active_user = (s_active_preset == PEQ_PRESET_USER_CUSTOM);
    taskEXIT_CRITICAL(&s_peq_lock);

    if (is_active_user) {
        stage_pending_from_active(false, false);
    }
    return ESP_OK;
}

esp_err_t peq_dsp_update_user_band(int band_idx, const peq_band_param_t *param)
{
    return peq_dsp_update_user_band_ch(PEQ_CH_LINKED, band_idx, param);
}

esp_err_t peq_dsp_save_user_profile_nvs(void)
{
    peq_nvs_v2_t v2;
    memset(&v2, 0, sizeof(v2));
    v2.magic = PEQ_NVS_MAGIC_V2;
    v2.version = PEQ_NVS_VERSION_V2;

    taskENTER_CRITICAL(&s_peq_lock);
    v2.linked_stereo = s_linked_stereo ? 1 : 0;
    v2.left = s_profiles_l[PEQ_PRESET_USER_CUSTOM];
    v2.right = s_linked_stereo ? v2.left : s_user_right;
    v2.mixer = s_mixer;
    taskEXIT_CRITICAL(&s_peq_lock);

    v2.crc32 = calc_crc32((const uint8_t *)&v2, offsetof(peq_nvs_v2_t, crc32));

    nvs_handle_t nvs_h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_h);
    if (err != ESP_OK) return err;

    size_t size = sizeof(peq_nvs_v2_t);
    peq_nvs_v2_t existing;
    bool needs_write = true;
    if (nvs_get_blob(nvs_h, NVS_KEY_USER_V2, &existing, &size) == ESP_OK &&
        size == sizeof(peq_nvs_v2_t) &&
        memcmp(&existing, &v2, sizeof(peq_nvs_v2_t)) == 0) {
        needs_write = false;
    }

    if (needs_write) {
        err = nvs_set_blob(nvs_h, NVS_KEY_USER_V2, &v2, sizeof(peq_nvs_v2_t));
        if (err == ESP_OK) {
            err = nvs_commit(nvs_h);
            ESP_LOGI(TAG, "NVS v2 User Custom profile & mixer committed successfully.");
        }
    } else {
        ESP_LOGI(TAG, "NVS v2 configuration unmodified, skipping flash write.");
    }
    nvs_close(nvs_h);
    return err;
}

esp_err_t peq_dsp_get_profile_ch(peq_preset_id_t preset_id, peq_channel_t ch, peq_profile_t *out_profile)
{
    if ((unsigned int)preset_id >= PEQ_PRESET_COUNT || !out_profile) return ESP_ERR_INVALID_ARG;

    taskENTER_CRITICAL(&s_peq_lock);
    if (preset_id == PEQ_PRESET_USER_CUSTOM && ch == PEQ_CH_RIGHT && !s_linked_stereo) {
        *out_profile = s_user_right;
    } else {
        *out_profile = s_profiles_l[preset_id];
    }
    taskEXIT_CRITICAL(&s_peq_lock);
    return ESP_OK;
}

esp_err_t peq_dsp_get_profile(peq_preset_id_t preset_id, peq_profile_t *out_profile)
{
    return peq_dsp_get_profile_ch(preset_id, PEQ_CH_LEFT, out_profile);
}

esp_err_t peq_dsp_get_user_dual_mono(bool *out_linked, peq_profile_t *out_left, peq_profile_t *out_right)
{
    taskENTER_CRITICAL(&s_peq_lock);
    if (out_linked) *out_linked = s_linked_stereo;
    if (out_left)   *out_left = s_profiles_l[PEQ_PRESET_USER_CUSTOM];
    if (out_right)  *out_right = s_linked_stereo ? s_profiles_l[PEQ_PRESET_USER_CUSTOM] : s_user_right;
    taskEXIT_CRITICAL(&s_peq_lock);
    return ESP_OK;
}

esp_err_t peq_dsp_set_linked_stereo(bool linked)
{
    bool is_active_user = false;
    taskENTER_CRITICAL(&s_peq_lock);
    if (s_linked_stereo != linked) {
        s_linked_stereo = linked;
        if (linked) {
            s_user_right = s_profiles_l[PEQ_PRESET_USER_CUSTOM];
        }
        s_state_gen++;
    }
    is_active_user = (s_active_preset == PEQ_PRESET_USER_CUSTOM);
    taskEXIT_CRITICAL(&s_peq_lock);

    if (is_active_user) {
        stage_pending_from_active(false, false);
    }
    ESP_LOGI(TAG, "Dual-Mono Stereo Link: %s", linked ? "LINKED" : "INDEPENDENT L/R");
    return ESP_OK;
}

bool peq_dsp_is_linked_stereo(void)
{
    taskENTER_CRITICAL(&s_peq_lock);
    bool linked = s_linked_stereo;
    taskEXIT_CRITICAL(&s_peq_lock);
    return linked;
}

esp_err_t peq_dsp_set_preamp(float gain_db)
{
    if (!isfinite(gain_db)) return ESP_ERR_INVALID_ARG;
    if (gain_db < -12.0f) gain_db = -12.0f;
    if (gain_db > 0.0f) gain_db = 0.0f;

    bool is_active_user = false;
    taskENTER_CRITICAL(&s_peq_lock);
    s_profiles_l[PEQ_PRESET_USER_CUSTOM].preamp_gain_db = gain_db;
    s_user_right.preamp_gain_db = gain_db;
    s_state_gen++;
    is_active_user = (s_active_preset == PEQ_PRESET_USER_CUSTOM);
    taskEXIT_CRITICAL(&s_peq_lock);

    if (is_active_user) {
        stage_pending_from_active(false, false);
    }
    return ESP_OK;
}

int peq_dsp_get_band_count_ch(peq_channel_t ch)
{
    taskENTER_CRITICAL(&s_peq_lock);
    int count;
    if (s_active_preset == PEQ_PRESET_USER_CUSTOM && ch == PEQ_CH_RIGHT && !s_linked_stereo) {
        count = s_user_right.num_bands;
    } else {
        count = s_profiles_l[s_active_preset].num_bands;
    }
    taskEXIT_CRITICAL(&s_peq_lock);
    if (count < 1 || count > PEQ_MAX_BANDS) count = PEQ_DEFAULT_NUM_BANDS;
    return count;
}

int peq_dsp_get_band_count(void)
{
    return peq_dsp_get_band_count_ch(PEQ_CH_LEFT);
}

esp_err_t peq_dsp_set_band_count_ch(peq_channel_t ch, int num_bands)
{
    if (num_bands < 1 || num_bands > PEQ_MAX_BANDS) return ESP_ERR_INVALID_ARG;

    bool is_active_user = false;
    taskENTER_CRITICAL(&s_peq_lock);
    bool write_l = (ch == PEQ_CH_LINKED || ch == PEQ_CH_LEFT || s_linked_stereo);
    bool write_r = (ch == PEQ_CH_LINKED || ch == PEQ_CH_RIGHT || s_linked_stereo);
    if (write_l) s_profiles_l[PEQ_PRESET_USER_CUSTOM].num_bands = (uint8_t)num_bands;
    if (write_r) s_user_right.num_bands = (uint8_t)num_bands;
    s_state_gen++;
    is_active_user = (s_active_preset == PEQ_PRESET_USER_CUSTOM);
    taskEXIT_CRITICAL(&s_peq_lock);

    if (is_active_user) {
        stage_pending_from_active(false, false);
    }
    return ESP_OK;
}

esp_err_t peq_dsp_set_band_count(int num_bands)
{
    return peq_dsp_set_band_count_ch(PEQ_CH_LINKED, num_bands);
}

void peq_dsp_set_ab_bypass(bool bypass)
{
    taskENTER_CRITICAL(&s_peq_lock);
    if (s_ab_bypass == bypass) {
        taskEXIT_CRITICAL(&s_peq_lock);
        return;
    }
    s_ab_bypass = bypass;
    s_state_gen++;
    taskEXIT_CRITICAL(&s_peq_lock);

    ESP_LOGI(TAG, "Instant A/B Bypass: %s", bypass ? "BYPASSED (Flat)" : "EQ ACTIVE");
    stage_pending_from_active(false, false);
}

bool peq_dsp_get_ab_bypass(void)
{
    taskENTER_CRITICAL(&s_peq_lock);
    bool bypass = s_ab_bypass;
    taskEXIT_CRITICAL(&s_peq_lock);
    return bypass;
}

esp_err_t peq_dsp_set_mixer(const peq_mixer_config_t *cfg)
{
    if (!cfg || (unsigned int)cfg->mode >= MIXER_MODE_COUNT) return ESP_ERR_INVALID_ARG;

    taskENTER_CRITICAL(&s_peq_lock);
    s_mixer = *cfg;
    s_state_gen++;
    taskEXIT_CRITICAL(&s_peq_lock);

    stage_pending_from_active(false, true);
    ESP_LOGI(TAG, "Mixer updated: mode=%d bal=%d width=%u%% crossfeed=%u",
             cfg->mode, cfg->balance, cfg->width_pct, cfg->crossfeed_level);
    return ESP_OK;
}

esp_err_t peq_dsp_get_mixer(peq_mixer_config_t *out_cfg)
{
    if (!out_cfg) return ESP_ERR_INVALID_ARG;
    taskENTER_CRITICAL(&s_peq_lock);
    *out_cfg = s_mixer;
    taskEXIT_CRITICAL(&s_peq_lock);
    return ESP_OK;
}

// In-line Direct Form II Transposed biquad filter step for single sample
static inline float biquad_filter_sample(biquad_state_t *bq, float in, float *w1, float *w2)
{
    float out = bq->b0 * in + *w1;
    *w1 = bq->b1 * in - bq->a1 * out + *w2;
    *w2 = bq->b2 * in - bq->a2 * out;
    if (fabsf(*w1) < 1e-15f) *w1 = 0.0f;
    if (fabsf(*w2) < 1e-15f) *w2 = 0.0f;
    return out;
}

static void consume_pending_updates_core1(void)
{
    taskENTER_CRITICAL(&s_peq_lock);
    if (s_pending_update) {
        for (int i = 0; i < PEQ_MAX_BANDS; i++) {
            s_target_biquads_l[i].b0 = s_pending_biquads_l[i].b0;
            s_target_biquads_l[i].b1 = s_pending_biquads_l[i].b1;
            s_target_biquads_l[i].b2 = s_pending_biquads_l[i].b2;
            s_target_biquads_l[i].a1 = s_pending_biquads_l[i].a1;
            s_target_biquads_l[i].a2 = s_pending_biquads_l[i].a2;

            s_target_biquads_r[i].b0 = s_pending_biquads_r[i].b0;
            s_target_biquads_r[i].b1 = s_pending_biquads_r[i].b1;
            s_target_biquads_r[i].b2 = s_pending_biquads_r[i].b2;
            s_target_biquads_r[i].a1 = s_pending_biquads_r[i].a1;
            s_target_biquads_r[i].a2 = s_pending_biquads_r[i].a2;

            if (s_pending_reset_states) {
                s_target_biquads_l[i].w1_l = 0.0f;
                s_target_biquads_l[i].w2_l = 0.0f;
                s_target_biquads_r[i].w1_r = 0.0f;
                s_target_biquads_r[i].w2_r = 0.0f;
            } else if (!s_in_crossfade) {
                s_target_biquads_l[i].w1_l = s_active_biquads_l[i].w1_l;
                s_target_biquads_l[i].w2_l = s_active_biquads_l[i].w2_l;
                s_target_biquads_r[i].w1_r = s_active_biquads_r[i].w1_r;
                s_target_biquads_r[i].w2_r = s_active_biquads_r[i].w2_r;
            }
        }
        s_target_num_bands_l = s_pending_num_bands_l;
        s_target_num_bands_r = s_pending_num_bands_r;
        s_target_preamp_linear = s_pending_preamp_linear;
        s_target_is_flat = s_pending_is_flat;
        s_crossfade_step = s_pending_crossfade_step;
        s_crossfade_progress = 0.0f;
        s_in_crossfade = true;
        s_pending_update = false;
        s_pending_reset_states = false;
    }
    if (s_pending_mixer_update) {
        s_mixer_rt = s_pending_mixer_rt;
        s_pending_mixer_update = false;
    }
    taskEXIT_CRITICAL(&s_peq_lock);
}

static inline void process_stereo_sample(float in_l, float in_r, float *out_l_ptr, float *out_r_ptr)
{
    float out_l = in_l;
    float out_r = in_r;

    if (!(s_active_is_flat && !s_in_crossfade)) {
        float act_l = in_l * s_active_preamp_linear;
        float act_r = in_r * s_active_preamp_linear;

        for (int b = 0; b < s_active_num_bands_l; b++) {
            act_l = biquad_filter_sample(&s_active_biquads_l[b], act_l,
                                         &s_active_biquads_l[b].w1_l, &s_active_biquads_l[b].w2_l);
        }
        for (int b = 0; b < s_active_num_bands_r; b++) {
            act_r = biquad_filter_sample(&s_active_biquads_r[b], act_r,
                                         &s_active_biquads_r[b].w1_r, &s_active_biquads_r[b].w2_r);
        }

        out_l = act_l;
        out_r = act_r;

        if (s_in_crossfade) {
            float tgt_l = in_l * s_target_preamp_linear;
            float tgt_r = in_r * s_target_preamp_linear;

            for (int b = 0; b < s_target_num_bands_l; b++) {
                tgt_l = biquad_filter_sample(&s_target_biquads_l[b], tgt_l,
                                             &s_target_biquads_l[b].w1_l, &s_target_biquads_l[b].w2_l);
            }
            for (int b = 0; b < s_target_num_bands_r; b++) {
                tgt_r = biquad_filter_sample(&s_target_biquads_r[b], tgt_r,
                                             &s_target_biquads_r[b].w1_r, &s_target_biquads_r[b].w2_r);
            }

            float p = s_crossfade_progress;
            out_l = (1.0f - p) * act_l + p * tgt_l;
            out_r = (1.0f - p) * act_r + p * tgt_r;

            s_crossfade_progress += s_crossfade_step;
            if (s_crossfade_progress >= 1.0f) {
                memcpy(s_active_biquads_l, s_target_biquads_l, sizeof(s_active_biquads_l));
                memcpy(s_active_biquads_r, s_target_biquads_r, sizeof(s_active_biquads_r));
                s_active_preamp_linear = s_target_preamp_linear;
                s_active_num_bands_l = s_target_num_bands_l;
                s_active_num_bands_r = s_target_num_bands_r;
                s_active_is_flat = s_target_is_flat;
                s_in_crossfade = false;

                int clear_l = s_active_is_flat ? 0 : s_active_num_bands_l;
                for (int i = clear_l; i < PEQ_MAX_BANDS; i++) {
                    s_active_biquads_l[i].w1_l = 0.0f;
                    s_active_biquads_l[i].w2_l = 0.0f;
                }
                int clear_r = s_active_is_flat ? 0 : s_active_num_bands_r;
                for (int i = clear_r; i < PEQ_MAX_BANDS; i++) {
                    s_active_biquads_r[i].w1_r = 0.0f;
                    s_active_biquads_r[i].w2_r = 0.0f;
                }
            }
        }
    }

    // 2x2 Channel Matrix & Chu Moy Crossfeed
    float mx_l = s_mixer_rt.m_ll * out_l + s_mixer_rt.m_rl * out_r;
    float mx_r = s_mixer_rt.m_lr * out_l + s_mixer_rt.m_rr * out_r;

    if (s_mixer_rt.crossfeed_en) {
        s_cf_delay_l[s_cf_idx] = mx_l;
        s_cf_delay_r[s_cf_idx] = mx_r;
        int read_idx = (s_cf_idx + CROSSFEED_MAX_DELAY - s_mixer_rt.cf_delay_samples) & (CROSSFEED_MAX_DELAY - 1);
        float del_l = s_cf_delay_l[read_idx];
        float del_r = s_cf_delay_r[read_idx];
        s_cf_idx = (s_cf_idx + 1) & (CROSSFEED_MAX_DELAY - 1);

        float a = s_mixer_rt.cf_alpha;
        s_cf_lp_l = (1.0f - a) * del_l + a * s_cf_lp_l;
        s_cf_lp_r = (1.0f - a) * del_r + a * s_cf_lp_r;
        if (fabsf(s_cf_lp_l) < 1e-15f) s_cf_lp_l = 0.0f;
        if (fabsf(s_cf_lp_r) < 1e-15f) s_cf_lp_r = 0.0f;

        mx_l += s_mixer_rt.crossfeed_gain * s_cf_lp_r;
        mx_r += s_mixer_rt.crossfeed_gain * s_cf_lp_l;
    }

    *out_l_ptr = mx_l;
    *out_r_ptr = mx_r;
}

void peq_dsp_process_interleaved_s16(int16_t *samples, size_t num_frames)
{
    if (!samples || num_frames == 0) return;

    consume_pending_updates_core1();

    bool mixer_identity = (!s_mixer_rt.crossfeed_en &&
                           s_mixer_rt.m_ll == 1.0f && s_mixer_rt.m_rl == 0.0f &&
                           s_mixer_rt.m_lr == 0.0f && s_mixer_rt.m_rr == 1.0f);
    if (s_active_is_flat && !s_in_crossfade && mixer_identity) {
        return;
    }

    for (size_t f = 0; f < num_frames; f++) {
        float out_l, out_r;
        process_stereo_sample((float)samples[2 * f], (float)samples[2 * f + 1], &out_l, &out_r);
        samples[2 * f]     = clamp_s16(out_l);
        samples[2 * f + 1] = clamp_s16(out_r);
    }
}

void peq_dsp_process_interleaved_s24_to_s32(const uint8_t *src_s24_packed, int32_t *dst_s32,
                                            size_t num_frames, float volume_factor)
{
    if (!src_s24_packed || !dst_s32 || num_frames == 0) return;

    consume_pending_updates_core1();

    for (size_t f = 0; f < num_frames; f++) {
        const uint8_t *p = &src_s24_packed[f * 6];
        int32_t s24_l = (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)(int8_t)p[2] << 16));
        int32_t s24_r = (int32_t)((uint32_t)p[3] | ((uint32_t)p[4] << 8) | ((uint32_t)(int8_t)p[5] << 16));

        float out_l, out_r;
        process_stereo_sample((float)s24_l, (float)s24_r, &out_l, &out_r);
        dst_s32[2 * f]     = clamp_s24_to_s32(out_l * volume_factor);
        dst_s32[2 * f + 1] = clamp_s24_to_s32(out_r * volume_factor);
    }
}

void peq_dsp_process_interleaved_s16_to_s32(const int16_t *src_s16, int32_t *dst_s32,
                                            size_t num_frames, float volume_factor)
{
    if (!src_s16 || !dst_s32 || num_frames == 0) return;

    consume_pending_updates_core1();

    for (size_t f = 0; f < num_frames; f++) {
        float out_l, out_r;
        process_stereo_sample((float)src_s16[2 * f] * 256.0f,
                              (float)src_s16[2 * f + 1] * 256.0f,
                              &out_l, &out_r);
        dst_s32[2 * f]     = clamp_s24_to_s32(out_l * volume_factor);
        dst_s32[2 * f + 1] = clamp_s24_to_s32(out_r * volume_factor);
    }
}
