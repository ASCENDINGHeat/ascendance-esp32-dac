#include "peq_dsp.h"

#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <math.h>

#include "esp_log.h"
#include "esp_dsp.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "nvs.h"

#define TAG "PEQ_DSP"
#define NVS_NAMESPACE       "audio_peq"
#define NVS_KEY_USER_V1     "user_profile"
#define NVS_KEY_USER_V2     "user_cfg_v2"
#define NVS_KEY_META_V3     "nvs_meta_v3"
#define NVS_KEY_FIR_L       "fir_taps_l"
#define NVS_KEY_FIR_R       "fir_taps_r"
#define NVS_KEY_HARM_V1     "harm_cfg_v1"

#define PEQ_NVS_MAGIC_V2    0x50455132UL // "PEQ2"
#define PEQ_NVS_VERSION_V2  2U
#define PEQ_NVS_MAGIC_V3    0x50455133UL // "PEQ3"
#define PEQ_NVS_VERSION_V3  3U
#define PEQ_META_MAGIC_V3   0x4D455433UL // "MET3"
#define PEQ_META_VERSION_V3 3U
#define PEQ_FIR_MAGIC       0x46495231UL // "FIR1"
#define PEQ_HARM_MAGIC_V1   0x48415231UL // "HAR1"
#define PEQ_HARM_VERSION_V1 1U

#define CROSSFEED_MAX_DELAY 32
#define FIR_BLOCK_MAX       128
#define LA_MAX_FRAMES       144          // 1.5 ms @ 96 kHz
#define FULLSCALE_S24_F     8388608.0f
#define PEAK_CEILING_S24    7918595.0f   // -0.5 dBFS
#define SOFT_KNEE_S24       8196846.0f   // -0.2 dBFS

#define HARM_LUT_SIZE       4096U
#define HARM_LUT_MAX_IN     2.0f
#define HARM_HB_SECTIONS    3
#define HARM_BASS_MAX_MIX   0.65f
#define HARM_EXC_MAX_MIX    0.55f

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

typedef struct {
    float x1_l, y1_l;
    float x1_r, y1_r;
    float r;
} dc_block_state_t;

typedef struct {
    float up_s0[HARM_HB_SECTIONS];
    float up_s1[HARM_HB_SECTIONS];
    float dn_s0[HARM_HB_SECTIONS];
    float dn_s1[HARM_HB_SECTIONS];
} hb_os2x_state_t;

typedef struct {
    bool               bass_active;
    bool               exc_active;
    peq_exciter_mode_t exc_mode;
    float              bass_mix;
    float              exc_mix;
    bool               use_oversampling;
    biquad_state_t     bass_pre_lpf;
    biquad_state_t     bass_post_lpf;
    biquad_state_t     exc_pre_hpf;
    biquad_state_t     exc_post_hpf;
    dc_block_state_t   bass_dc;
    dc_block_state_t   exc_dc;
    hb_os2x_state_t    bass_os_l;
    hb_os2x_state_t    bass_os_r;
    hb_os2x_state_t    exc_os_l;
    hb_os2x_state_t    exc_os_r;
} harm_rt_t;

typedef struct {
    bool          linked_stereo;
    peq_profile_t left;
    peq_profile_t right;
} peq_slot_t;

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

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint8_t  linked_stereo;
    uint8_t  reserved;
    peq_profile_t left;
    peq_profile_t right;
    uint32_t crc32;
} peq_nvs_slot_v3_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint8_t  active_slot;
    uint8_t  limiter_enabled;
    uint8_t  loudness_mode;
    uint8_t  oled_view;
    uint8_t  fir_enabled;
    uint8_t  reserved;
    uint16_t auto_mute_sec;
    uint16_t oled_dim_sec;
    peq_mixer_config_t mixer;
    uint32_t crc32;
} peq_nvs_meta_v3_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t num_taps;
    uint16_t reserved;
    float    taps[PEQ_FIR_MAX_TAPS_48K];
    uint32_t crc32;
} peq_fir_nvs_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint8_t  bass_enabled;
    uint8_t  bass_amount_pct;
    uint16_t bass_freq_hz;
    uint8_t  exciter_enabled;
    uint8_t  exciter_mode;
    uint8_t  exciter_amount_pct;
    uint8_t  reserved;
    uint16_t exciter_freq_hz;
    uint32_t crc32;
} peq_harm_nvs_v1_t;

static portMUX_TYPE s_peq_lock = portMUX_INITIALIZER_UNLOCKED;

static peq_slot_t s_slots[PEQ_USER_SLOT_COUNT];
static bool s_ab_bypass = false;
static peq_preset_id_t s_active_preset = PEQ_SLOT_0;
static peq_mixer_config_t s_mixer;
static int s_sample_rate = 48000;
static uint32_t s_state_gen = 0;

/* v0.2.0 Studio Dynamics & Power Configuration (protected by s_peq_lock) */
static bool                s_limiter_enabled = true;
static peq_loudness_mode_t s_loudness_mode   = PEQ_LOUDNESS_OFF;
static int                 s_volume_pct      = 50;
static uint8_t             s_oled_view       = 0;
static uint16_t            s_auto_mute_sec   = 10;
static uint16_t            s_oled_dim_sec    = 60;
static bool                s_fir_enabled     = false;

/* Psychoacoustic Bass Enhancer & Harmonic Exciter Configuration & PSRAM LUTs */
static peq_harmonics_cfg_t s_harm_cfg;
static float *s_harm_lut_bass = NULL;
static float *s_harm_lut_exciter[PEQ_EXCITER_MODE_COUNT] = { NULL, NULL, NULL };

/* 13th-order elliptic 2x half-band polyphase allpass coefficients (>94 dB stopband) */
static const float s_hb_a0[HARM_HB_SECTIONS] = { 0.04536216f, 0.33714969f, 0.70806414f };
static const float s_hb_a1[HARM_HB_SECTIONS] = { 0.16808748f, 0.52237785f, 0.89744559f };

/* Uploaded FIR impulse responses in DRAM */
static float    s_fir_upload_l[PEQ_FIR_MAX_TAPS_48K];
static float    s_fir_upload_r[PEQ_FIR_MAX_TAPS_48K];
static uint16_t s_fir_taps_l = 0;
static uint16_t s_fir_taps_r = 0;
static float    s_fir_stage_buf_l[PEQ_FIR_MAX_TAPS_48K];
static float    s_fir_stage_buf_r[PEQ_FIR_MAX_TAPS_48K];
static uint16_t s_fir_stage_total_l = 0;
static uint16_t s_fir_stage_total_r = 0;
static bool     s_pending_fir_update = false;

/* Staged pending updates (written on Core 0 under s_peq_lock, consumed by Core 1) */
static biquad_state_t s_pending_biquads_l[PEQ_MAX_BANDS];
static biquad_state_t s_pending_biquads_r[PEQ_MAX_BANDS];
static biquad_state_t s_pending_loudness_bq[2]; // [0]=LowShelf 80Hz, [1]=HighShelf 10kHz
static int s_pending_num_bands_l = PEQ_DEFAULT_NUM_BANDS;
static int s_pending_num_bands_r = PEQ_DEFAULT_NUM_BANDS;
static float s_pending_preamp_linear = 1.0f;
static bool s_pending_is_flat = true;
static bool s_pending_loudness_active = false;
static bool s_pending_reset_states = false;
static bool s_pending_update = false;
static float s_pending_crossfade_step = 0.0f;
static mixer_rt_t s_pending_mixer_rt;
static bool s_pending_mixer_update = false;
static bool s_pending_limiter_enabled = true;
static bool s_pending_wake_ramp = false;
static harm_rt_t s_harm_pending;
static bool s_harm_pending_update = false;
static bool s_harm_pending_reset_states = false;
static float s_harm_pending_crossfade_step = 0.0f;

/* Core 1 DSP runtime state (owned exclusively by audio_dsp_task after init) */
static int s_active_num_bands_l = PEQ_DEFAULT_NUM_BANDS;
static int s_active_num_bands_r = PEQ_DEFAULT_NUM_BANDS;
static int s_target_num_bands_l = PEQ_DEFAULT_NUM_BANDS;
static int s_target_num_bands_r = PEQ_DEFAULT_NUM_BANDS;

static biquad_state_t s_active_biquads_l[PEQ_MAX_BANDS];
static biquad_state_t s_active_biquads_r[PEQ_MAX_BANDS];
static biquad_state_t s_target_biquads_l[PEQ_MAX_BANDS];
static biquad_state_t s_target_biquads_r[PEQ_MAX_BANDS];

static biquad_state_t s_active_loudness_l[2];
static biquad_state_t s_active_loudness_r[2];
static biquad_state_t s_target_loudness_l[2];
static biquad_state_t s_target_loudness_r[2];
static bool s_active_loudness_en = false;
static bool s_target_loudness_en = false;

static float s_active_preamp_linear = 1.0f;
static float s_target_preamp_linear = 1.0f;

static bool s_active_is_flat = true;
static bool s_target_is_flat = true;

static bool s_in_crossfade = false;
static float s_crossfade_progress = 0.0f;
static float s_crossfade_step = 0.0f;

static harm_rt_t s_harm_active;
static harm_rt_t s_harm_target;
static bool s_harm_in_crossfade = false;
static float s_harm_crossfade_progress = 0.0f;
static float s_harm_crossfade_step = 0.0f;

static mixer_rt_t s_mixer_rt;
static float s_cf_delay_l[CROSSFEED_MAX_DELAY];
static float s_cf_delay_r[CROSSFEED_MAX_DELAY];
static int s_cf_idx = 0;
static float s_cf_lp_l = 0.0f;
static float s_cf_lp_r = 0.0f;

/* Core 1 ESP-DSP SIMD FIR runtime buffers (16-byte aligned for PIE SIMD) */
static __attribute__((aligned(16))) float s_fir_coeffs_l[PEQ_FIR_MAX_TAPS_48K];
static __attribute__((aligned(16))) float s_fir_coeffs_r[PEQ_FIR_MAX_TAPS_48K];
static __attribute__((aligned(16))) float s_fir_delay_l[PEQ_FIR_MAX_TAPS_48K + 16];
static __attribute__((aligned(16))) float s_fir_delay_r[PEQ_FIR_MAX_TAPS_48K + 16];
static __attribute__((aligned(16))) float s_fir_in_l[FIR_BLOCK_MAX];
static __attribute__((aligned(16))) float s_fir_in_r[FIR_BLOCK_MAX];
static __attribute__((aligned(16))) float s_fir_out_l[FIR_BLOCK_MAX];
static __attribute__((aligned(16))) float s_fir_out_r[FIR_BLOCK_MAX];
static fir_f32_t s_fir_rt_l;
static fir_f32_t s_fir_rt_r;
static bool      s_fir_rt_active_l = false;
static bool      s_fir_rt_active_r = false;
static bool      s_fir_rt_en       = false;

/* Core 1 Lookahead Limiter runtime state */
static float s_la_buf_l[LA_MAX_FRAMES];
static float s_la_buf_r[LA_MAX_FRAMES];
static int   s_la_pos        = 0;
static int   s_la_len        = 72; // 1.5 ms @ 48 kHz
static float s_la_gain       = 1.0f;
static float s_la_target_g   = 1.0f;
static int   s_la_hold_cnt   = 0;
static float s_la_att_coeff  = 0.05f;
static float s_la_rel_coeff  = 0.0002f;
static bool  s_limiter_rt_en = true;
static int   s_rt_sr         = 48000;

/* Core 1 Soft Wake Ramp, Per-Sample Volume Smoother, TPDF Dither & Silence Tracker */
static float    s_wake_ramp_gain   = 1.0f;
static float    s_wake_ramp_step   = 0.02f;
static float    s_vol_smooth       = -1.0f;    // Negative sentinel snaps to initial volume on first block
static float    s_vol_smooth_coeff = 0.00208f; // ~10 ms 1-pole time constant @ 48 kHz
static uint32_t s_dither_rng       = 0x13579BDFUL;
static uint64_t s_silence_samples  = 0;

/* Telemetry VU & Silence snapshot (written on Core 1, read on Core 0 under s_peq_lock) */
static float    s_vu_peak_lin_l   = 0.0f;
static float    s_vu_peak_lin_r   = 0.0f;
static float    s_vu_rms_lin_l    = 0.0f;
static float    s_vu_rms_lin_r    = 0.0f;
static bool     s_vu_lim_active   = false;
static bool     s_vu_clip_latched = false;
static uint32_t s_vu_silence_ms   = 0;

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

static inline float soft_clip_s24(float val, bool *clipped)
{
    if (!isfinite(val)) return 0.0f;
    float av = fabsf(val);
    if (av <= SOFT_KNEE_S24) {
        return val;
    }
    if (av >= FULLSCALE_S24_F) {
        if (clipped) *clipped = true;
    }
    /* Smooth asymptotic tanh-like soft saturation from -0.2 dBFS toward 0 dBFS */
    float span = 8388607.0f - SOFT_KNEE_S24;
    float excess = (av - SOFT_KNEE_S24) / span;
    float compressed = SOFT_KNEE_S24 + span * (excess / (1.0f + excess));
    if (compressed > 8388607.0f) compressed = 8388607.0f;
    return (val >= 0.0f) ? compressed : -compressed;
}

/* Fast Core 1 xorshift32 TPDF dither: triangular PDF in [-1.0, +1.0) LSB at bit 24 */
static inline float tpdf_dither_lsb(void)
{
    uint32_t x = s_dither_rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    uint32_t r1 = x;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s_dither_rng = x;
    return ((float)(int32_t)r1 - (float)(int32_t)x) * (1.0f / 4294967296.0f);
}

static inline int32_t clamp_s24_to_s32(float val)
{
    if (!isfinite(val)) return 0;
    if (val > 8388607.0f) return 0x7FFFFF00L;
    if (val < -8388608.0f) return (int32_t)0x80000000UL;
    return (int32_t)((uint32_t)(int32_t)lrintf(val) << 8);
}

static inline int32_t clamp_s24_to_s32_dither(float val, bool apply_dither)
{
    if (!isfinite(val)) return 0;
    if (apply_dither) {
        val += tpdf_dither_lsb();
    }
    return clamp_s24_to_s32(val);
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

    double f0 = (double)band->freq_hz;
    double sr = (double)sample_rate;
    if (f0 < 10.0) f0 = 10.0;
    if (f0 >= sr * 0.49) f0 = sr * 0.49;

    double q = (double)band->q_factor;
    if (q < 0.05) q = 0.05;
    if (q > 10.0) q = 10.0;

    double gain_db = (double)band->gain_db;
    if (gain_db < -24.0) gain_db = -24.0;
    if (gain_db > 12.0) gain_db = 12.0;

    double omega = 2.0 * M_PI * f0 / sr;
    double cos_w = cos(omega);
    double sin_w = sin(omega);
    double alpha = sin_w / (2.0 * q);
    double A = pow(10.0, gain_db / 40.0);

    double b0 = 1.0, b1 = 0.0, b2 = 0.0;
    double a0 = 1.0, a1 = 0.0, a2 = 0.0;

    switch (band->type) {
    case PEQ_FILTER_PEAKING:
        b0 = 1.0 + alpha * A;
        b1 = -2.0 * cos_w;
        b2 = 1.0 - alpha * A;
        a0 = 1.0 + alpha / A;
        a1 = -2.0 * cos_w;
        a2 = 1.0 - alpha / A;
        break;

    case PEQ_FILTER_LOW_SHELF: {
        double sqrt_A = sqrt(A);
        b0 = A * ((A + 1.0) - (A - 1.0) * cos_w + 2.0 * sqrt_A * alpha);
        b1 = 2.0 * A * ((A - 1.0) - (A + 1.0) * cos_w);
        b2 = A * ((A + 1.0) - (A - 1.0) * cos_w - 2.0 * sqrt_A * alpha);
        a0 = (A + 1.0) + (A - 1.0) * cos_w + 2.0 * sqrt_A * alpha;
        a1 = -2.0 * ((A - 1.0) + (A + 1.0) * cos_w);
        a2 = (A + 1.0) + (A - 1.0) * cos_w - 2.0 * sqrt_A * alpha;
        break;
    }

    case PEQ_FILTER_HIGH_SHELF: {
        double sqrt_A = sqrt(A);
        b0 = A * ((A + 1.0) + (A - 1.0) * cos_w + 2.0 * sqrt_A * alpha);
        b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * cos_w);
        b2 = A * ((A + 1.0) + (A - 1.0) * cos_w - 2.0 * sqrt_A * alpha);
        a0 = (A + 1.0) - (A - 1.0) * cos_w + 2.0 * sqrt_A * alpha;
        a1 = 2.0 * ((A - 1.0) - (A + 1.0) * cos_w);
        a2 = (A + 1.0) - (A - 1.0) * cos_w - 2.0 * sqrt_A * alpha;
        break;
    }

    case PEQ_FILTER_HIGH_PASS: {
        double cos_half = cos(0.5 * omega);
        double one_plus_cos = 2.0 * cos_half * cos_half;
        b0 = 0.5 * one_plus_cos;
        b1 = -one_plus_cos;
        b2 = 0.5 * one_plus_cos;
        a0 = 1.0 + alpha;
        a1 = -2.0 * cos_w;
        a2 = 1.0 - alpha;
        break;
    }

    case PEQ_FILTER_LOW_PASS: {
        double sin_half = sin(0.5 * omega);
        double one_minus_cos = 2.0 * sin_half * sin_half;
        b0 = 0.5 * one_minus_cos;
        b1 = one_minus_cos;
        b2 = 0.5 * one_minus_cos;
        a0 = 1.0 + alpha;
        a1 = -2.0 * cos_w;
        a2 = 1.0 - alpha;
        break;
    }

    case PEQ_FILTER_NOTCH:
        b0 = 1.0;
        b1 = -2.0 * cos_w;
        b2 = 1.0;
        a0 = 1.0 + alpha;
        a1 = -2.0 * cos_w;
        a2 = 1.0 - alpha;
        break;

    case PEQ_FILTER_BAND_PASS:
        b0 = alpha;
        b1 = 0.0;
        b2 = -alpha;
        a0 = 1.0 + alpha;
        a1 = -2.0 * cos_w;
        a2 = 1.0 - alpha;
        break;

    default:
        set_passthrough_coeffs(out_bq);
        return;
    }

    if (!isfinite(a0) || fabs(a0) < 1e-12) {
        set_passthrough_coeffs(out_bq);
        return;
    }

    float nb0 = (float)(b0 / a0);
    float nb1 = (float)(b1 / a0);
    float nb2 = (float)(b2 / a0);
    float na1 = (float)(a1 / a0);
    float na2 = (float)(a2 / a0);

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

static void compute_loudness_biquads(peq_loudness_mode_t mode, int vol_pct, float sample_rate,
                                     bool ab_bypass, biquad_state_t out_loud[2], bool *out_active)
{
    memset(out_loud, 0, sizeof(biquad_state_t) * 2);
    set_passthrough_coeffs(&out_loud[0]);
    set_passthrough_coeffs(&out_loud[1]);

    if (ab_bypass || mode == PEQ_LOUDNESS_OFF || vol_pct >= 80) {
        if (out_active) *out_active = false;
        return;
    }

    float c = (80.0f - (float)vol_pct) / 60.0f;
    if (c < 0.0f) c = 0.0f;
    if (c > 1.0f) c = 1.0f;

    float max_bass_db = (mode >= PEQ_LOUDNESS_FULL) ? 6.5f : 4.0f;
    float max_treb_db = (mode >= PEQ_LOUDNESS_FULL) ? 2.5f : 1.5f;

    peq_band_param_t bass = {
        .type = PEQ_FILTER_LOW_SHELF,
        .freq_hz = 80.0f,
        .gain_db = max_bass_db * c,
        .q_factor = 0.65f,
        .enabled = true,
    };
    peq_band_param_t treb = {
        .type = PEQ_FILTER_HIGH_SHELF,
        .freq_hz = 10000.0f,
        .gain_db = max_treb_db * c,
        .q_factor = 0.71f,
        .enabled = true,
    };

    calculate_biquad(&bass, sample_rate, &out_loud[0]);
    calculate_biquad(&treb, sample_rate, &out_loud[1]);
    if (out_active) {
        *out_active = (!biquad_is_passthrough(&out_loud[0]) || !biquad_is_passthrough(&out_loud[1]));
    }
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
        cf_gain = (cfg->crossfeed_level >= 2) ? 0.5012f : 0.5957f;
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

    if (cf_en && cf_gain > 0.0f) {
        // Compensate coherent low-frequency crossfeed sum (~-1.8 to -2.0 dB) so toggling Crossfeed stays loudness-neutral
        float cf_norm = 1.0f / (1.0f + 0.5f * cf_gain);
        m_ll *= cf_norm;
        m_rl *= cf_norm;
        m_lr *= cf_norm;
        m_rr *= cf_norm;
    }

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

static void clamp_harmonics_cfg(peq_harmonics_cfg_t *cfg)
{
    if (!cfg) return;
    cfg->bass_enabled = cfg->bass_enabled ? true : false;
    if (cfg->bass_amount_pct > 100U) cfg->bass_amount_pct = 100U;
    if (cfg->bass_freq_hz < 50U) cfg->bass_freq_hz = 50U;
    if (cfg->bass_freq_hz > 150U) cfg->bass_freq_hz = 150U;

    cfg->exciter_enabled = cfg->exciter_enabled ? true : false;
    if ((unsigned int)cfg->exciter_mode >= (unsigned int)PEQ_EXCITER_MODE_COUNT) {
        cfg->exciter_mode = PEQ_EXCITER_TUBE;
    }
    if (cfg->exciter_amount_pct > 100U) cfg->exciter_amount_pct = 100U;
    if (cfg->exciter_freq_hz < 1500U) cfg->exciter_freq_hz = 1500U;
    if (cfg->exciter_freq_hz > 8000U) cfg->exciter_freq_hz = 8000U;
}

static void setup_default_harmonics(peq_harmonics_cfg_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->bass_enabled = false;
    cfg->bass_amount_pct = 50U;
    cfg->bass_freq_hz = 80U;
    cfg->exciter_enabled = false;
    cfg->exciter_mode = PEQ_EXCITER_TUBE;
    cfg->exciter_amount_pct = 35U;
    cfg->exciter_freq_hz = 3500U;
}

static void calc_butterworth_lpf(double fc, double sr, biquad_state_t *out)
{
    memset(out, 0, sizeof(*out));
    if (sr <= 0.0 || fc <= 0.0) {
        set_passthrough_coeffs(out);
        return;
    }
    if (fc >= sr * 0.49) fc = sr * 0.49;

    double w = 2.0 * M_PI * fc / sr;
    double cos_w = cos(w);
    double sin_w = sin(w);
    double sin_half = sin(0.5 * w);
    double one_minus_cos = 2.0 * sin_half * sin_half;
    double alpha = sin_w / (2.0 * 0.7071067811865476);
    double a0 = 1.0 + alpha;

    out->b0 = (float)((0.5 * one_minus_cos) / a0);
    out->b1 = (float)(one_minus_cos / a0);
    out->b2 = out->b0;
    out->a1 = (float)((-2.0 * cos_w) / a0);
    out->a2 = (float)((1.0 - alpha) / a0);
}

static void calc_butterworth_hpf(double fc, double sr, biquad_state_t *out)
{
    memset(out, 0, sizeof(*out));
    if (sr <= 0.0 || fc <= 0.0) {
        set_passthrough_coeffs(out);
        return;
    }
    if (fc >= sr * 0.49) fc = sr * 0.49;

    double w = 2.0 * M_PI * fc / sr;
    double cos_w = cos(w);
    double sin_w = sin(w);
    double cos_half = cos(0.5 * w);
    double one_plus_cos = 2.0 * cos_half * cos_half;
    double alpha = sin_w / (2.0 * 0.7071067811865476);
    double a0 = 1.0 + alpha;

    out->b0 = (float)((0.5 * one_plus_cos) / a0);
    out->b1 = (float)((-one_plus_cos) / a0);
    out->b2 = out->b0;
    out->a1 = (float)((-2.0 * cos_w) / a0);
    out->a2 = (float)((1.0 - alpha) / a0);
}

static void compute_harm_rt(const peq_harmonics_cfg_t *cfg, int sample_rate,
                            bool ab_bypass, harm_rt_t *out)
{
    memset(out, 0, sizeof(*out));
    int sr = (sample_rate > 0) ? sample_rate : 48000;
    out->use_oversampling = (sr <= 48000);

    bool bass_on = (!ab_bypass && cfg->bass_enabled && cfg->bass_amount_pct > 0U);
    bool exc_on  = (!ab_bypass && cfg->exciter_enabled && cfg->exciter_amount_pct > 0U);
    out->bass_active = bass_on;
    out->exc_active  = exc_on;
    out->exc_mode    = ((unsigned int)cfg->exciter_mode < (unsigned int)PEQ_EXCITER_MODE_COUNT)
                       ? cfg->exciter_mode : PEQ_EXCITER_TUBE;
    out->bass_mix    = bass_on ? (((float)cfg->bass_amount_pct / 100.0f) * HARM_BASS_MAX_MIX) : 0.0f;
    out->exc_mix     = exc_on  ? (((float)cfg->exciter_amount_pct / 100.0f) * HARM_EXC_MAX_MIX) : 0.0f;

    float r_dc = 1.0f - (2.0f * (float)M_PI * 20.0f / (float)sr);
    if (r_dc < 0.990f) r_dc = 0.990f;
    if (r_dc > 0.9995f) r_dc = 0.9995f;
    out->bass_dc.r = r_dc;
    out->exc_dc.r  = r_dc;

    if (bass_on) {
        double bass_fc = (double)cfg->bass_freq_hz;
        double post_fc = bass_fc * 3.5;
        if (post_fc > 450.0) post_fc = 450.0;
        calc_butterworth_lpf(bass_fc, (double)sr, &out->bass_pre_lpf);
        calc_butterworth_lpf(post_fc, (double)sr, &out->bass_post_lpf);
    } else {
        set_passthrough_coeffs(&out->bass_pre_lpf);
        set_passthrough_coeffs(&out->bass_post_lpf);
    }

    if (exc_on) {
        double exc_fc = (double)cfg->exciter_freq_hz;
        double post_fc = exc_fc * 0.85;
        calc_butterworth_hpf(exc_fc, (double)sr, &out->exc_pre_hpf);
        calc_butterworth_hpf(post_fc, (double)sr, &out->exc_post_hpf);
    } else {
        set_passthrough_coeffs(&out->exc_pre_hpf);
        set_passthrough_coeffs(&out->exc_post_hpf);
    }
}

static float *alloc_harm_lut_once(size_t count)
{
    size_t bytes = count * sizeof(float);
    float *ptr = (float *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    if (!ptr) {
        ptr = (float *)malloc(bytes);
    }
    return ptr;
}

static void init_harmonics_luts_once(void)
{
    if (!s_harm_lut_bass) {
        s_harm_lut_bass = alloc_harm_lut_once(HARM_LUT_SIZE);
        if (s_harm_lut_bass) {
            for (uint32_t i = 0; i < HARM_LUT_SIZE; i++) {
                float x = -HARM_LUT_MAX_IN + HARM_LUT_MAX_IN * ((float)i / 2047.0f);
                float asym = (x * x) / (1.0f + fabsf(x));
                float odd = x - tanhf(1.8f * x) / 1.8f;
                s_harm_lut_bass[i] = tanhf(0.85f * asym + 1.15f * odd);
            }
            s_harm_lut_bass[2047] = 0.0f;
        }
    }

    for (int m = 0; m < (int)PEQ_EXCITER_MODE_COUNT; m++) {
        if (!s_harm_lut_exciter[m]) {
            s_harm_lut_exciter[m] = alloc_harm_lut_once(HARM_LUT_SIZE);
            if (s_harm_lut_exciter[m]) {
                for (uint32_t i = 0; i < HARM_LUT_SIZE; i++) {
                    float x = -HARM_LUT_MAX_IN + HARM_LUT_MAX_IN * ((float)i / 2047.0f);
                    float y = 0.0f;
                    if (m == (int)PEQ_EXCITER_TUBE) {
                        float u = 2.5f * x;
                        float asym = 0.40f * (u * u) / (1.0f + 0.35f * u * u);
                        y = tanhf(u + asym) - 0.75f * tanhf(u);
                    } else if (m == (int)PEQ_EXCITER_TAPE) {
                        float u = 2.0f * x;
                        y = tanhf(u) - 0.45f * tanhf(0.5f * u);
                    } else {
                        float u = 2.0f * x;
                        float asym = 0.20f * (u * u) / (1.0f + 0.40f * u * u);
                        y = 0.50f * tanhf(u + asym + 0.25f * tanhf(2.0f * u)) - 0.25f * tanhf(u);
                    }
                    s_harm_lut_exciter[m][i] = y;
                }
                s_harm_lut_exciter[m][2047] = 0.0f;
            }
        }
    }
}

static void init_flat_profile(peq_profile_t *prof, const char *slot_name)
{
    memset(prof, 0, sizeof(*prof));
    snprintf(prof->name, sizeof(prof->name), "%.23s", slot_name ? slot_name : "User");
    prof->num_bands = PEQ_DEFAULT_NUM_BANDS;
    prof->preamp_gain_db = 0.0f;

    /* 5 ISO starter bands seeded @ 0.0 dB Reference Flat (passthrough in DSP when 0.0 dB) */
    prof->bands[0] = (peq_band_param_t){PEQ_FILTER_LOW_SHELF,   80.0f,   0.0f, 0.71f, true};
    prof->bands[1] = (peq_band_param_t){PEQ_FILTER_PEAKING,     250.0f,  0.0f, 1.00f, true};
    prof->bands[2] = (peq_band_param_t){PEQ_FILTER_PEAKING,     1000.0f, 0.0f, 1.00f, true};
    prof->bands[3] = (peq_band_param_t){PEQ_FILTER_PEAKING,     4000.0f, 0.0f, 1.00f, true};
    prof->bands[4] = (peq_band_param_t){PEQ_FILTER_HIGH_SHELF,  10000.0f,0.0f, 0.71f, true};
    for (int i = 5; i < PEQ_MAX_BANDS; i++) {
        prof->bands[i] = (peq_band_param_t){PEQ_FILTER_BYPASS, 1000.0f, 0.0f, 1.00f, false};
    }
}

static void setup_default_slots(void)
{
    for (int i = 0; i < PEQ_USER_SLOT_COUNT; i++) {
        char default_name[PEQ_SLOT_NAME_MAX];
        snprintf(default_name, sizeof(default_name), "Slot %d", i + 1);
        s_slots[i].linked_stereo = true;
        init_flat_profile(&s_slots[i].left, default_name);
        init_flat_profile(&s_slots[i].right, default_name);
    }
    s_active_preset = PEQ_SLOT_0;
    s_limiter_enabled = true;
    s_loudness_mode = PEQ_LOUDNESS_OFF;
    s_oled_view = 0;
    s_auto_mute_sec = 10;
    s_oled_dim_sec = 60;
    s_fir_enabled = false;
    s_fir_taps_l = 0;
    s_fir_taps_r = 0;
    setup_default_mixer(&s_mixer);
    setup_default_harmonics(&s_harm_cfg);
}

static void sanitize_profile(peq_profile_t *prof)
{
    if (prof->num_bands == 0 || prof->num_bands > PEQ_MAX_BANDS) {
        prof->num_bands = PEQ_DEFAULT_NUM_BANDS;
    }
    if (!isfinite(prof->preamp_gain_db)) {
        prof->preamp_gain_db = 0.0f;
    }
    if (prof->preamp_gain_db < -12.0f) prof->preamp_gain_db = -12.0f;
    if (prof->preamp_gain_db > 0.0f) prof->preamp_gain_db = 0.0f;
    prof->name[sizeof(prof->name) - 1] = '\0';
}

static esp_err_t save_slot_nvs_internal(int slot_id)
{
    if (slot_id < 0 || slot_id >= PEQ_USER_SLOT_COUNT) return ESP_ERR_INVALID_ARG;

    peq_nvs_slot_v3_t v3;
    memset(&v3, 0, sizeof(v3));
    v3.magic = PEQ_NVS_MAGIC_V3;
    v3.version = PEQ_NVS_VERSION_V3;

    taskENTER_CRITICAL(&s_peq_lock);
    v3.linked_stereo = s_slots[slot_id].linked_stereo ? 1 : 0;
    v3.left = s_slots[slot_id].left;
    v3.right = s_slots[slot_id].linked_stereo ? s_slots[slot_id].left : s_slots[slot_id].right;
    taskEXIT_CRITICAL(&s_peq_lock);

    v3.crc32 = calc_crc32((const uint8_t *)&v3, offsetof(peq_nvs_slot_v3_t, crc32));

    char key[16];
    snprintf(key, sizeof(key), "peq_slot_%d", slot_id);

    nvs_handle_t nvs_h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_h);
    if (err != ESP_OK) return err;

    size_t sz = sizeof(peq_nvs_slot_v3_t);
    peq_nvs_slot_v3_t existing;
    bool needs_write = true;
    if (nvs_get_blob(nvs_h, key, &existing, &sz) == ESP_OK &&
        sz == sizeof(peq_nvs_slot_v3_t) &&
        memcmp(&existing, &v3, sizeof(peq_nvs_slot_v3_t)) == 0) {
        needs_write = false;
    }

    if (needs_write) {
        err = nvs_set_blob(nvs_h, key, &v3, sizeof(peq_nvs_slot_v3_t));
        if (err == ESP_OK) {
            err = nvs_commit(nvs_h);
            ESP_LOGI(TAG, "Committed NVS v3 %s (\"%s\")", key, s_slots[slot_id].left.name);
        }
    }
    nvs_close(nvs_h);
    return err;
}

static esp_err_t save_meta_nvs_internal(void)
{
    peq_nvs_meta_v3_t meta;
    memset(&meta, 0, sizeof(meta));
    meta.magic = PEQ_META_MAGIC_V3;
    meta.version = PEQ_META_VERSION_V3;

    taskENTER_CRITICAL(&s_peq_lock);
    meta.active_slot = (uint8_t)s_active_preset;
    meta.limiter_enabled = s_limiter_enabled ? 1 : 0;
    meta.loudness_mode = (uint8_t)s_loudness_mode;
    meta.oled_view = s_oled_view;
    meta.fir_enabled = s_fir_enabled ? 1 : 0;
    meta.auto_mute_sec = s_auto_mute_sec;
    meta.oled_dim_sec = s_oled_dim_sec;
    meta.mixer = s_mixer;
    taskEXIT_CRITICAL(&s_peq_lock);

    meta.crc32 = calc_crc32((const uint8_t *)&meta, offsetof(peq_nvs_meta_v3_t, crc32));

    nvs_handle_t nvs_h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_h);
    if (err != ESP_OK) return err;

    size_t sz = sizeof(peq_nvs_meta_v3_t);
    peq_nvs_meta_v3_t existing;
    bool needs_write = true;
    if (nvs_get_blob(nvs_h, NVS_KEY_META_V3, &existing, &sz) == ESP_OK &&
        sz == sizeof(peq_nvs_meta_v3_t) &&
        memcmp(&existing, &meta, sizeof(peq_nvs_meta_v3_t)) == 0) {
        needs_write = false;
    }

    if (needs_write) {
        err = nvs_set_blob(nvs_h, NVS_KEY_META_V3, &meta, sizeof(peq_nvs_meta_v3_t));
        if (err == ESP_OK) {
            err = nvs_commit(nvs_h);
        }
    }
    nvs_close(nvs_h);
    return err;
}

static void save_fir_nvs_internal(peq_channel_t ch)
{
    const char *key = (ch == PEQ_CH_RIGHT) ? NVS_KEY_FIR_R : NVS_KEY_FIR_L;
    peq_fir_nvs_t blob;
    memset(&blob, 0, sizeof(blob));
    blob.magic = PEQ_FIR_MAGIC;

    taskENTER_CRITICAL(&s_peq_lock);
    uint16_t count = (ch == PEQ_CH_RIGHT) ? s_fir_taps_r : s_fir_taps_l;
    if (count > PEQ_FIR_MAX_TAPS_48K) count = PEQ_FIR_MAX_TAPS_48K;
    blob.num_taps = count;
    if (count > 0) {
        const float *src = (ch == PEQ_CH_RIGHT) ? s_fir_upload_r : s_fir_upload_l;
        memcpy(blob.taps, src, sizeof(float) * count);
    }
    taskEXIT_CRITICAL(&s_peq_lock);

    blob.crc32 = calc_crc32((const uint8_t *)&blob, offsetof(peq_fir_nvs_t, crc32));

    nvs_handle_t nvs_h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_h) != ESP_OK) return;

    size_t sz = sizeof(peq_fir_nvs_t);
    peq_fir_nvs_t existing;
    bool needs_write = true;
    if (nvs_get_blob(nvs_h, key, &existing, &sz) == ESP_OK &&
        sz == sizeof(peq_fir_nvs_t) &&
        memcmp(&existing, &blob, sizeof(peq_fir_nvs_t)) == 0) {
        needs_write = false;
    }
    if (needs_write) {
        if (nvs_set_blob(nvs_h, key, &blob, sizeof(peq_fir_nvs_t)) == ESP_OK) {
            nvs_commit(nvs_h);
        }
    }
    nvs_close(nvs_h);
}

static esp_err_t save_harm_nvs_internal(void)
{
    peq_harm_nvs_v1_t blob;
    memset(&blob, 0, sizeof(blob));
    blob.magic = PEQ_HARM_MAGIC_V1;
    blob.version = PEQ_HARM_VERSION_V1;

    taskENTER_CRITICAL(&s_peq_lock);
    peq_harmonics_cfg_t snap = s_harm_cfg;
    taskEXIT_CRITICAL(&s_peq_lock);

    clamp_harmonics_cfg(&snap);
    blob.bass_enabled = snap.bass_enabled ? 1U : 0U;
    blob.bass_amount_pct = snap.bass_amount_pct;
    blob.bass_freq_hz = snap.bass_freq_hz;
    blob.exciter_enabled = snap.exciter_enabled ? 1U : 0U;
    blob.exciter_mode = (uint8_t)snap.exciter_mode;
    blob.exciter_amount_pct = snap.exciter_amount_pct;
    blob.exciter_freq_hz = snap.exciter_freq_hz;
    blob.crc32 = calc_crc32((const uint8_t *)&blob, offsetof(peq_harm_nvs_v1_t, crc32));

    nvs_handle_t nvs_h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_h);
    if (err != ESP_OK) return err;

    size_t sz = sizeof(peq_harm_nvs_v1_t);
    peq_harm_nvs_v1_t existing;
    bool needs_write = true;
    if (nvs_get_blob(nvs_h, NVS_KEY_HARM_V1, &existing, &sz) == ESP_OK &&
        sz == sizeof(peq_harm_nvs_v1_t) &&
        memcmp(&existing, &blob, sizeof(peq_harm_nvs_v1_t)) == 0) {
        needs_write = false;
    }

    if (needs_write) {
        err = nvs_set_blob(nvs_h, NVS_KEY_HARM_V1, &blob, sizeof(peq_harm_nvs_v1_t));
        if (err == ESP_OK) {
            err = nvs_commit(nvs_h);
            ESP_LOGI(TAG, "Committed NVS %s (bass=%u/%u%%@%uHz, exc=%u/%u/%u%%@%uHz)",
                     NVS_KEY_HARM_V1,
                     (unsigned)blob.bass_enabled, (unsigned)blob.bass_amount_pct, (unsigned)blob.bass_freq_hz,
                     (unsigned)blob.exciter_enabled, (unsigned)blob.exciter_mode,
                     (unsigned)blob.exciter_amount_pct, (unsigned)blob.exciter_freq_hz);
        }
    }
    nvs_close(nvs_h);
    return err;
}

static void load_user_profile_nvs(void)
{
    nvs_handle_t nvs_h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_h);
    if (err != ESP_OK) return;

    bool found_v3 = false;

    /* 1. Load NVS v3 global metadata */
    size_t req_meta = sizeof(peq_nvs_meta_v3_t);
    peq_nvs_meta_v3_t meta;
    if (nvs_get_blob(nvs_h, NVS_KEY_META_V3, &meta, &req_meta) == ESP_OK &&
        req_meta == sizeof(peq_nvs_meta_v3_t) &&
        meta.magic == PEQ_META_MAGIC_V3 &&
        meta.version == PEQ_META_VERSION_V3 &&
        calc_crc32((const uint8_t *)&meta, offsetof(peq_nvs_meta_v3_t, crc32)) == meta.crc32) {
        found_v3 = true;
        if (meta.active_slot < PEQ_USER_SLOT_COUNT) {
            s_active_preset = (peq_preset_id_t)meta.active_slot;
        }
        s_limiter_enabled = (meta.limiter_enabled != 0);
        s_loudness_mode = (meta.loudness_mode < PEQ_LOUDNESS_COUNT)
                          ? (peq_loudness_mode_t)meta.loudness_mode : PEQ_LOUDNESS_OFF;
        s_oled_view = (meta.oled_view < 4) ? meta.oled_view : 0;
        s_fir_enabled = (meta.fir_enabled != 0);
        s_auto_mute_sec = meta.auto_mute_sec;
        s_oled_dim_sec = meta.oled_dim_sec;

        peq_mixer_config_t tmp_mixer = meta.mixer;
        if ((unsigned int)tmp_mixer.mode < MIXER_MODE_COUNT) {
            s_mixer = tmp_mixer;
        }
    }

    /* 2. Load each NVS v3 user slot (0..7) */
    for (int i = 0; i < PEQ_USER_SLOT_COUNT; i++) {
        char key[16];
        snprintf(key, sizeof(key), "peq_slot_%d", i);
        size_t req_slot = sizeof(peq_nvs_slot_v3_t);
        peq_nvs_slot_v3_t slot_blob;
        if (nvs_get_blob(nvs_h, key, &slot_blob, &req_slot) == ESP_OK &&
            req_slot == sizeof(peq_nvs_slot_v3_t) &&
            slot_blob.magic == PEQ_NVS_MAGIC_V3 &&
            slot_blob.version == PEQ_NVS_VERSION_V3 &&
            calc_crc32((const uint8_t *)&slot_blob, offsetof(peq_nvs_slot_v3_t, crc32)) == slot_blob.crc32) {
            peq_profile_t tmp_l = slot_blob.left;
            peq_profile_t tmp_r = slot_blob.right;
            sanitize_profile(&tmp_l);
            sanitize_profile(&tmp_r);
            s_slots[i].linked_stereo = (slot_blob.linked_stereo != 0);
            s_slots[i].left = tmp_l;
            s_slots[i].right = s_slots[i].linked_stereo ? tmp_l : tmp_r;
            if (i == 0) found_v3 = true;
        }
    }

    /* 3. Load optional FIR taps */
    size_t req_fir = sizeof(peq_fir_nvs_t);
    peq_fir_nvs_t fir_blob;
    if (nvs_get_blob(nvs_h, NVS_KEY_FIR_L, &fir_blob, &req_fir) == ESP_OK &&
        req_fir == sizeof(peq_fir_nvs_t) &&
        fir_blob.magic == PEQ_FIR_MAGIC &&
        fir_blob.num_taps <= PEQ_FIR_MAX_TAPS_48K &&
        calc_crc32((const uint8_t *)&fir_blob, offsetof(peq_fir_nvs_t, crc32)) == fir_blob.crc32) {
        s_fir_taps_l = fir_blob.num_taps;
        memcpy(s_fir_upload_l, fir_blob.taps, sizeof(float) * PEQ_FIR_MAX_TAPS_48K);
    }
    req_fir = sizeof(peq_fir_nvs_t);
    if (nvs_get_blob(nvs_h, NVS_KEY_FIR_R, &fir_blob, &req_fir) == ESP_OK &&
        req_fir == sizeof(peq_fir_nvs_t) &&
        fir_blob.magic == PEQ_FIR_MAGIC &&
        fir_blob.num_taps <= PEQ_FIR_MAX_TAPS_48K &&
        calc_crc32((const uint8_t *)&fir_blob, offsetof(peq_fir_nvs_t, crc32)) == fir_blob.crc32) {
        s_fir_taps_r = fir_blob.num_taps;
        memcpy(s_fir_upload_r, fir_blob.taps, sizeof(float) * PEQ_FIR_MAX_TAPS_48K);
    }

    /* 3b. Load global harmonics configuration (harm_cfg_v1) */
    size_t req_harm = sizeof(peq_harm_nvs_v1_t);
    peq_harm_nvs_v1_t harm_blob;
    if (nvs_get_blob(nvs_h, NVS_KEY_HARM_V1, &harm_blob, &req_harm) == ESP_OK &&
        req_harm == sizeof(peq_harm_nvs_v1_t) &&
        harm_blob.magic == PEQ_HARM_MAGIC_V1 &&
        harm_blob.version == PEQ_HARM_VERSION_V1 &&
        calc_crc32((const uint8_t *)&harm_blob, offsetof(peq_harm_nvs_v1_t, crc32)) == harm_blob.crc32) {
        peq_harmonics_cfg_t loaded_harm;
        memset(&loaded_harm, 0, sizeof(loaded_harm));
        loaded_harm.bass_enabled = (harm_blob.bass_enabled != 0U);
        loaded_harm.bass_amount_pct = harm_blob.bass_amount_pct;
        loaded_harm.bass_freq_hz = harm_blob.bass_freq_hz;
        loaded_harm.exciter_enabled = (harm_blob.exciter_enabled != 0U);
        loaded_harm.exciter_mode = (peq_exciter_mode_t)harm_blob.exciter_mode;
        loaded_harm.exciter_amount_pct = harm_blob.exciter_amount_pct;
        loaded_harm.exciter_freq_hz = harm_blob.exciter_freq_hz;
        clamp_harmonics_cfg(&loaded_harm);
        s_harm_cfg = loaded_harm;
    }

    /* 4. Migrate legacy v0.1.0 NVS v2 / v1 into Slot 0 if NVS v3 absent */
    if (!found_v3) {
        size_t req_v2 = sizeof(peq_nvs_v2_t);
        peq_nvs_v2_t v2;
        if (nvs_get_blob(nvs_h, NVS_KEY_USER_V2, &v2, &req_v2) == ESP_OK &&
            req_v2 == sizeof(peq_nvs_v2_t) &&
            v2.magic == PEQ_NVS_MAGIC_V2 &&
            v2.version == PEQ_NVS_VERSION_V2 &&
            calc_crc32((const uint8_t *)&v2, offsetof(peq_nvs_v2_t, crc32)) == v2.crc32) {
            peq_profile_t tmp_l = v2.left;
            peq_profile_t tmp_r = v2.right;
            peq_mixer_config_t tmp_m = v2.mixer;
            sanitize_profile(&tmp_l);
            sanitize_profile(&tmp_r);
            s_slots[0].linked_stereo = (v2.linked_stereo != 0);
            s_slots[0].left = tmp_l;
            s_slots[0].right = s_slots[0].linked_stereo ? tmp_l : tmp_r;
            if ((unsigned int)tmp_m.mode < MIXER_MODE_COUNT) {
                s_mixer = tmp_m;
            }
            ESP_LOGI(TAG, "Migrated v0.1.0 NVS v2 profile into Slot 0 (\"%s\")", s_slots[0].left.name);
        } else {
            size_t req_v1 = sizeof(peq_profile_t);
            peq_profile_t loaded_v1;
            if (nvs_get_blob(nvs_h, NVS_KEY_USER_V1, &loaded_v1, &req_v1) == ESP_OK &&
                req_v1 == sizeof(peq_profile_t)) {
                sanitize_profile(&loaded_v1);
                s_slots[0].linked_stereo = true;
                s_slots[0].left = loaded_v1;
                s_slots[0].right = loaded_v1;
                ESP_LOGI(TAG, "Migrated legacy NVS v1 profile into Slot 0 (\"%s\")", s_slots[0].left.name);
            }
        }
    }

    nvs_close(nvs_h);
}

static void stage_pending_from_active_ex(bool reset_states, bool stage_mixer, bool stage_fir, bool stage_harm)
{
    for (;;) {
        taskENTER_CRITICAL(&s_peq_lock);
        uint32_t gen = s_state_gen;
        int sr = s_sample_rate;
        peq_preset_id_t slot = s_active_preset;
        bool ab_bypass = s_ab_bypass;
        bool linked = s_slots[slot].linked_stereo;
        peq_profile_t prof_l = s_slots[slot].left;
        peq_profile_t prof_r = linked ? prof_l : s_slots[slot].right;
        peq_mixer_config_t mixer_snap = s_mixer;
        peq_loudness_mode_t loud_mode = s_loudness_mode;
        int vol_pct = s_volume_pct;
        bool lim_en = s_limiter_enabled;
        peq_harmonics_cfg_t harm_snap = s_harm_cfg;
        taskEXIT_CRITICAL(&s_peq_lock);

        float step = (sr > 0) ? (1.0f / (0.015f * (float)sr)) : 1.0f;
        if (!isfinite(step) || step <= 0.0f) step = 1.0f;

        biquad_state_t local_bq_l[PEQ_MAX_BANDS];
        biquad_state_t local_bq_r[PEQ_MAX_BANDS];
        biquad_state_t local_loud[2];
        float local_preamp_l = 1.0f;
        float local_preamp_r = 1.0f;
        int local_nb_l = PEQ_DEFAULT_NUM_BANDS;
        int local_nb_r = PEQ_DEFAULT_NUM_BANDS;
        bool local_flat_l = true;
        bool local_flat_r = true;
        bool local_loud_en = false;

        apply_profile_to_biquads(&prof_l, (float)sr, ab_bypass, local_bq_l,
                                 &local_preamp_l, &local_nb_l, &local_flat_l);
        apply_profile_to_biquads(&prof_r, (float)sr, ab_bypass, local_bq_r,
                                 &local_preamp_r, &local_nb_r, &local_flat_r);
        compute_loudness_biquads(loud_mode, vol_pct, (float)sr, ab_bypass,
                                 local_loud, &local_loud_en);

        mixer_rt_t local_mixer;
        if (stage_mixer) {
            compute_mixer_rt(&mixer_snap, (float)sr, &local_mixer);
        }

        harm_rt_t local_harm;
        if (stage_harm) {
            compute_harm_rt(&harm_snap, sr, ab_bypass, &local_harm);
        }

        taskENTER_CRITICAL(&s_peq_lock);
        if (s_state_gen == gen) {
            s_pending_crossfade_step = step;
            memcpy(s_pending_biquads_l, local_bq_l, sizeof(s_pending_biquads_l));
            memcpy(s_pending_biquads_r, local_bq_r, sizeof(s_pending_biquads_r));
            memcpy(s_pending_loudness_bq, local_loud, sizeof(s_pending_loudness_bq));
            s_pending_num_bands_l = local_nb_l;
            s_pending_num_bands_r = local_nb_r;
            s_pending_preamp_linear = local_preamp_l;
            s_pending_is_flat = local_flat_l && local_flat_r && !local_loud_en;
            s_pending_loudness_active = local_loud_en;
            s_pending_limiter_enabled = lim_en;
            s_pending_reset_states = (s_pending_update && s_pending_reset_states) || reset_states;
            s_pending_update = true;
            if (stage_mixer) {
                s_pending_mixer_rt = local_mixer;
                s_pending_mixer_update = true;
            }
            if (stage_fir) {
                s_pending_fir_update = true;
            }
            if (stage_harm) {
                s_harm_pending = local_harm;
                s_harm_pending_crossfade_step = step;
                s_harm_pending_reset_states = (s_harm_pending_update && s_harm_pending_reset_states) || reset_states;
                s_harm_pending_update = true;
            }
            taskEXIT_CRITICAL(&s_peq_lock);
            return;
        }
        taskEXIT_CRITICAL(&s_peq_lock);
    }
}

static void stage_pending_from_active(bool reset_states, bool stage_mixer, bool stage_fir)
{
    stage_pending_from_active_ex(reset_states, stage_mixer, stage_fir, false);
}

static void stage_pending_harmonics(bool reset_states)
{
    for (;;) {
        taskENTER_CRITICAL(&s_peq_lock);
        uint32_t gen = s_state_gen;
        int sr = s_sample_rate;
        bool ab_bypass = s_ab_bypass;
        peq_harmonics_cfg_t harm_snap = s_harm_cfg;
        taskEXIT_CRITICAL(&s_peq_lock);

        float step = (sr > 0) ? (1.0f / (0.015f * (float)sr)) : 1.0f;
        if (!isfinite(step) || step <= 0.0f) step = 1.0f;

        harm_rt_t local_harm;
        compute_harm_rt(&harm_snap, sr, ab_bypass, &local_harm);

        taskENTER_CRITICAL(&s_peq_lock);
        if (s_state_gen == gen) {
            s_harm_pending = local_harm;
            s_harm_pending_crossfade_step = step;
            s_harm_pending_reset_states = (s_harm_pending_update && s_harm_pending_reset_states) || reset_states;
            s_harm_pending_update = true;
            taskEXIT_CRITICAL(&s_peq_lock);
            return;
        }
        taskEXIT_CRITICAL(&s_peq_lock);
    }
}

static void configure_limiter_rt_for_sr(int sample_rate)
{
    if (sample_rate <= 0) sample_rate = 48000;
    s_rt_sr = sample_rate;

    int la = (int)lroundf(0.0015f * (float)sample_rate);
    if (la < 16) la = 16;
    if (la > LA_MAX_FRAMES) la = LA_MAX_FRAMES;
    s_la_len = la;
    s_la_pos = 0;
    memset(s_la_buf_l, 0, sizeof(s_la_buf_l));
    memset(s_la_buf_r, 0, sizeof(s_la_buf_r));
    s_la_gain = 1.0f;
    s_la_target_g = 1.0f;
    s_la_hold_cnt = 0;

    s_la_att_coeff = 1.0f - expf(-1.0f / (0.0004f * (float)sample_rate));
    s_la_rel_coeff = 1.0f - expf(-1.0f / (0.1000f * (float)sample_rate));
    s_vol_smooth_coeff = 1.0f - expf(-1.0f / (0.0100f * (float)sample_rate)); // 10 ms zipper-free volume ramp
    s_wake_ramp_step = 1.0f / (0.001f * (float)sample_rate);
    if (s_wake_ramp_step <= 0.0f) s_wake_ramp_step = 0.02f;
}

static void rebuild_fir_rt_locked(int sr, bool en, uint16_t taps_l, uint16_t taps_r)
{
    uint16_t max_taps = peq_dsp_get_fir_max_taps_for_sr(sr);

    memset(s_fir_coeffs_l, 0, sizeof(s_fir_coeffs_l));
    memset(s_fir_coeffs_r, 0, sizeof(s_fir_coeffs_r));
    memset(s_fir_delay_l, 0, sizeof(s_fir_delay_l));
    memset(s_fir_delay_r, 0, sizeof(s_fir_delay_r));

    s_fir_rt_en = en;

    if (en && taps_l > 0) {
        uint16_t use_l = (taps_l > max_taps) ? max_taps : taps_l;
        uint16_t pad_l = (uint16_t)((use_l + 3U) & ~3U);
        if (pad_l < 4) pad_l = 4;
        if (pad_l > max_taps) pad_l = max_taps;
        memcpy(s_fir_coeffs_l, s_fir_upload_l, sizeof(float) * use_l);
        dsps_fir_init_f32(&s_fir_rt_l, s_fir_coeffs_l, s_fir_delay_l, (int)pad_l);
        s_fir_rt_active_l = true;
    } else {
        s_fir_rt_active_l = false;
    }

    if (en && taps_r > 0) {
        uint16_t use_r = (taps_r > max_taps) ? max_taps : taps_r;
        uint16_t pad_r = (uint16_t)((use_r + 3U) & ~3U);
        if (pad_r < 4) pad_r = 4;
        if (pad_r > max_taps) pad_r = max_taps;
        memcpy(s_fir_coeffs_r, s_fir_upload_r, sizeof(float) * use_r);
        dsps_fir_init_f32(&s_fir_rt_r, s_fir_coeffs_r, s_fir_delay_r, (int)pad_r);
        s_fir_rt_active_r = true;
    } else {
        s_fir_rt_active_r = false;
    }
}

esp_err_t peq_dsp_init(int sample_rate)
{
    if (sample_rate <= 0) sample_rate = 48000;

    setup_default_slots();
    load_user_profile_nvs();
    init_harmonics_luts_once();

    float crossfade_step = 1.0f / (0.015f * (float)sample_rate);
    biquad_state_t init_bq_l[PEQ_MAX_BANDS];
    biquad_state_t init_bq_r[PEQ_MAX_BANDS];
    biquad_state_t init_loud[2];
    float init_preamp_l = 1.0f, init_preamp_r = 1.0f;
    int init_nb_l = PEQ_DEFAULT_NUM_BANDS, init_nb_r = PEQ_DEFAULT_NUM_BANDS;
    bool init_flat_l = true, init_flat_r = true, init_loud_en = false;

    peq_profile_t *p_l = &s_slots[s_active_preset].left;
    peq_profile_t *p_r = s_slots[s_active_preset].linked_stereo ? p_l : &s_slots[s_active_preset].right;

    apply_profile_to_biquads(p_l, (float)sample_rate, false,
                             init_bq_l, &init_preamp_l, &init_nb_l, &init_flat_l);
    apply_profile_to_biquads(p_r, (float)sample_rate, false,
                             init_bq_r, &init_preamp_r, &init_nb_r, &init_flat_r);
    compute_loudness_biquads(s_loudness_mode, s_volume_pct, (float)sample_rate, false,
                             init_loud, &init_loud_en);

    mixer_rt_t init_mixer;
    compute_mixer_rt(&s_mixer, (float)sample_rate, &init_mixer);

    harm_rt_t init_harm;
    compute_harm_rt(&s_harm_cfg, sample_rate, false, &init_harm);

    configure_limiter_rt_for_sr(sample_rate);
    rebuild_fir_rt_locked(sample_rate, s_fir_enabled, s_fir_taps_l, s_fir_taps_r);

    taskENTER_CRITICAL(&s_peq_lock);
    s_sample_rate = sample_rate;
    s_state_gen = 0;
    s_ab_bypass = false;

    s_crossfade_step = crossfade_step;
    s_pending_crossfade_step = crossfade_step;

    memcpy(s_active_biquads_l, init_bq_l, sizeof(s_active_biquads_l));
    memcpy(s_active_biquads_r, init_bq_r, sizeof(s_active_biquads_r));
    memcpy(s_target_biquads_l, init_bq_l, sizeof(s_target_biquads_l));
    memcpy(s_target_biquads_r, init_bq_r, sizeof(s_target_biquads_r));
    memcpy(s_active_loudness_l, init_loud, sizeof(s_active_loudness_l));
    memcpy(s_active_loudness_r, init_loud, sizeof(s_active_loudness_r));
    memcpy(s_target_loudness_l, init_loud, sizeof(s_target_loudness_l));
    memcpy(s_target_loudness_r, init_loud, sizeof(s_target_loudness_r));
    s_active_loudness_en = init_loud_en;
    s_target_loudness_en = init_loud_en;

    s_active_num_bands_l = init_nb_l;
    s_active_num_bands_r = init_nb_r;
    s_target_num_bands_l = init_nb_l;
    s_target_num_bands_r = init_nb_r;
    s_active_preamp_linear = init_preamp_l;
    s_target_preamp_linear = init_preamp_l;
    s_active_is_flat = init_flat_l && init_flat_r && !init_loud_en;
    s_target_is_flat = s_active_is_flat;
    s_in_crossfade = false;
    s_crossfade_progress = 0.0f;
    s_pending_update = false;
    s_pending_reset_states = false;
    s_pending_fir_update = false;
    s_pending_limiter_enabled = s_limiter_enabled;
    s_limiter_rt_en = s_limiter_enabled;

    s_harm_active = init_harm;
    s_harm_target = init_harm;
    s_harm_pending = init_harm;
    s_harm_in_crossfade = false;
    s_harm_crossfade_progress = 0.0f;
    s_harm_crossfade_step = crossfade_step;
    s_harm_pending_crossfade_step = crossfade_step;
    s_harm_pending_update = false;
    s_harm_pending_reset_states = false;

    s_mixer_rt = init_mixer;
    s_pending_mixer_rt = init_mixer;
    s_pending_mixer_update = false;
    memset(s_cf_delay_l, 0, sizeof(s_cf_delay_l));
    memset(s_cf_delay_r, 0, sizeof(s_cf_delay_r));
    s_cf_idx = 0;
    s_cf_lp_l = 0.0f;
    s_cf_lp_r = 0.0f;
    s_silence_samples = 0;
    s_wake_ramp_gain = 1.0f;
    taskEXIT_CRITICAL(&s_peq_lock);

    ESP_LOGI(TAG, "PEQ DSP v0.2.0 init @ %d Hz (Slot %d: \"%s\", Lim=%d, Loud=%d, FIR=%d, Bass=%d, Exc=%d)",
             sample_rate, s_active_preset, s_slots[s_active_preset].left.name,
             s_limiter_enabled ? 1 : 0, (int)s_loudness_mode, s_fir_enabled ? 1 : 0,
             s_harm_cfg.bass_enabled ? 1 : 0, s_harm_cfg.exciter_enabled ? 1 : 0);
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

    stage_pending_from_active_ex(true, true, true, true);
}

void peq_dsp_select_preset(peq_preset_id_t preset_id)
{
    if ((unsigned int)preset_id >= PEQ_USER_SLOT_COUNT) return;

    peq_profile_t prev_prof, next_prof;
    taskENTER_CRITICAL(&s_peq_lock);
    if (preset_id == s_active_preset) {
        taskEXIT_CRITICAL(&s_peq_lock);
        return;
    }
    prev_prof = s_slots[s_active_preset].left;
    next_prof = s_slots[preset_id].left;
    s_active_preset = preset_id;
    s_state_gen++;
    taskEXIT_CRITICAL(&s_peq_lock);

    prev_prof.name[sizeof(prev_prof.name) - 1] = '\0';
    next_prof.name[sizeof(next_prof.name) - 1] = '\0';
    ESP_LOGI(TAG, "Switching PEQ slot %d -> %d (\"%s\" -> \"%s\")",
             s_active_preset, (int)preset_id, prev_prof.name, next_prof.name);

    stage_pending_from_active(true, false, false);
}

peq_preset_id_t peq_dsp_cycle_preset(void)
{
    taskENTER_CRITICAL(&s_peq_lock);
    peq_preset_id_t next = (peq_preset_id_t)((s_active_preset + 1) % PEQ_USER_SLOT_COUNT);
    taskEXIT_CRITICAL(&s_peq_lock);

    peq_dsp_select_preset(next);
    return next;
}

peq_preset_id_t peq_dsp_get_current_preset(char *out_name)
{
    peq_preset_id_t preset;
    char name_buf[PEQ_SLOT_NAME_MAX];

    taskENTER_CRITICAL(&s_peq_lock);
    preset = s_active_preset;
    if (out_name) {
        memcpy(name_buf, s_slots[preset].left.name, sizeof(name_buf));
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

    taskENTER_CRITICAL(&s_peq_lock);
    peq_slot_t *slot = &s_slots[s_active_preset];
    bool write_l = (ch == PEQ_CH_LINKED || ch == PEQ_CH_LEFT || slot->linked_stereo);
    bool write_r = (ch == PEQ_CH_LINKED || ch == PEQ_CH_RIGHT || slot->linked_stereo);

    if (write_l) {
        if (band_idx >= slot->left.num_bands) {
            slot->left.num_bands = (uint8_t)(band_idx + 1);
        }
        slot->left.bands[band_idx] = *param;
    }
    if (write_r) {
        if (band_idx >= slot->right.num_bands) {
            slot->right.num_bands = (uint8_t)(band_idx + 1);
        }
        slot->right.bands[band_idx] = *param;
    }
    s_state_gen++;
    taskEXIT_CRITICAL(&s_peq_lock);

    stage_pending_from_active(false, false, false);
    return ESP_OK;
}

esp_err_t peq_dsp_update_user_band(int band_idx, const peq_band_param_t *param)
{
    return peq_dsp_update_user_band_ch(PEQ_CH_LINKED, band_idx, param);
}

esp_err_t peq_dsp_save_user_profile_nvs(void)
{
    int slot;
    taskENTER_CRITICAL(&s_peq_lock);
    slot = (int)s_active_preset;
    taskEXIT_CRITICAL(&s_peq_lock);

    esp_err_t err = save_slot_nvs_internal(slot);
    if (err != ESP_OK) return err;
    err = save_meta_nvs_internal();
    if (err != ESP_OK) return err;
    return save_harm_nvs_internal();
}

esp_err_t peq_dsp_rename_slot(int slot_id, const char *new_name)
{
    if (slot_id < 0 || slot_id >= PEQ_USER_SLOT_COUNT || !new_name) return ESP_ERR_INVALID_ARG;

    taskENTER_CRITICAL(&s_peq_lock);
    snprintf(s_slots[slot_id].left.name, PEQ_SLOT_NAME_MAX, "%.23s", new_name);
    snprintf(s_slots[slot_id].right.name, PEQ_SLOT_NAME_MAX, "%.23s", new_name);
    if (s_slots[slot_id].left.name[0] == '\0') {
        snprintf(s_slots[slot_id].left.name, PEQ_SLOT_NAME_MAX, "Slot %d", slot_id + 1);
        snprintf(s_slots[slot_id].right.name, PEQ_SLOT_NAME_MAX, "Slot %d", slot_id + 1);
    }
    taskEXIT_CRITICAL(&s_peq_lock);

    return save_slot_nvs_internal(slot_id);
}

esp_err_t peq_dsp_reset_slot_to_flat(int slot_id)
{
    if (slot_id < 0 || slot_id >= PEQ_USER_SLOT_COUNT) return ESP_ERR_INVALID_ARG;

    char default_name[PEQ_SLOT_NAME_MAX];
    snprintf(default_name, sizeof(default_name), "Slot %d", slot_id + 1);

    bool is_active = false;
    taskENTER_CRITICAL(&s_peq_lock);
    s_slots[slot_id].linked_stereo = true;
    init_flat_profile(&s_slots[slot_id].left, default_name);
    init_flat_profile(&s_slots[slot_id].right, default_name);
    s_state_gen++;
    is_active = ((int)s_active_preset == slot_id);
    taskEXIT_CRITICAL(&s_peq_lock);

    if (is_active) {
        stage_pending_from_active(true, false, false);
    }
    return save_slot_nvs_internal(slot_id);
}

esp_err_t peq_dsp_get_profile_ch(peq_preset_id_t preset_id, peq_channel_t ch, peq_profile_t *out_profile)
{
    if ((unsigned int)preset_id >= PEQ_USER_SLOT_COUNT || !out_profile) return ESP_ERR_INVALID_ARG;

    taskENTER_CRITICAL(&s_peq_lock);
    if (ch == PEQ_CH_RIGHT && !s_slots[preset_id].linked_stereo) {
        *out_profile = s_slots[preset_id].right;
    } else {
        *out_profile = s_slots[preset_id].left;
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
    const peq_slot_t *slot = &s_slots[s_active_preset];
    if (out_linked) *out_linked = slot->linked_stereo;
    if (out_left)   *out_left = slot->left;
    if (out_right)  *out_right = slot->linked_stereo ? slot->left : slot->right;
    taskEXIT_CRITICAL(&s_peq_lock);
    return ESP_OK;
}

esp_err_t peq_dsp_set_linked_stereo(bool linked)
{
    taskENTER_CRITICAL(&s_peq_lock);
    peq_slot_t *slot = &s_slots[s_active_preset];
    if (slot->linked_stereo != linked) {
        slot->linked_stereo = linked;
        if (linked) {
            slot->right = slot->left;
        }
        s_state_gen++;
    }
    taskEXIT_CRITICAL(&s_peq_lock);

    stage_pending_from_active(false, false, false);
    ESP_LOGI(TAG, "Slot %d Dual-Mono Link: %s", (int)s_active_preset, linked ? "LINKED" : "INDEPENDENT L/R");
    return ESP_OK;
}

bool peq_dsp_is_linked_stereo(void)
{
    taskENTER_CRITICAL(&s_peq_lock);
    bool linked = s_slots[s_active_preset].linked_stereo;
    taskEXIT_CRITICAL(&s_peq_lock);
    return linked;
}

esp_err_t peq_dsp_set_preamp(float gain_db)
{
    if (!isfinite(gain_db)) return ESP_ERR_INVALID_ARG;
    if (gain_db < -12.0f) gain_db = -12.0f;
    if (gain_db > 0.0f) gain_db = 0.0f;

    taskENTER_CRITICAL(&s_peq_lock);
    s_slots[s_active_preset].left.preamp_gain_db = gain_db;
    s_slots[s_active_preset].right.preamp_gain_db = gain_db;
    s_state_gen++;
    taskEXIT_CRITICAL(&s_peq_lock);

    stage_pending_from_active(false, false, false);
    return ESP_OK;
}

int peq_dsp_get_band_count_ch(peq_channel_t ch)
{
    taskENTER_CRITICAL(&s_peq_lock);
    const peq_slot_t *slot = &s_slots[s_active_preset];
    int count = (ch == PEQ_CH_RIGHT && !slot->linked_stereo)
                ? slot->right.num_bands : slot->left.num_bands;
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

    taskENTER_CRITICAL(&s_peq_lock);
    peq_slot_t *slot = &s_slots[s_active_preset];
    bool write_l = (ch == PEQ_CH_LINKED || ch == PEQ_CH_LEFT || slot->linked_stereo);
    bool write_r = (ch == PEQ_CH_LINKED || ch == PEQ_CH_RIGHT || slot->linked_stereo);
    if (write_l) slot->left.num_bands = (uint8_t)num_bands;
    if (write_r) slot->right.num_bands = (uint8_t)num_bands;
    s_state_gen++;
    taskEXIT_CRITICAL(&s_peq_lock);

    stage_pending_from_active(false, false, false);
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
    stage_pending_from_active_ex(false, false, false, true);
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

    stage_pending_from_active(false, true, false);
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

/* ---- v0.2.0 Studio Dynamics, Loudness, FIR, Curve, VU & Auto-Mute APIs ---- */

void peq_dsp_set_limiter_enabled(bool enabled)
{
    taskENTER_CRITICAL(&s_peq_lock);
    if (s_limiter_enabled == enabled) {
        taskEXIT_CRITICAL(&s_peq_lock);
        return;
    }
    s_limiter_enabled = enabled;
    s_state_gen++;
    taskEXIT_CRITICAL(&s_peq_lock);
    stage_pending_from_active(false, false, false);
}

bool peq_dsp_is_limiter_enabled(void)
{
    taskENTER_CRITICAL(&s_peq_lock);
    bool en = s_limiter_enabled;
    taskEXIT_CRITICAL(&s_peq_lock);
    return en;
}

void peq_dsp_set_loudness_mode(peq_loudness_mode_t mode)
{
    if ((unsigned int)mode >= PEQ_LOUDNESS_COUNT) return;
    taskENTER_CRITICAL(&s_peq_lock);
    if (s_loudness_mode == mode) {
        taskEXIT_CRITICAL(&s_peq_lock);
        return;
    }
    s_loudness_mode = mode;
    s_state_gen++;
    taskEXIT_CRITICAL(&s_peq_lock);
    stage_pending_from_active(false, false, false);
}

peq_loudness_mode_t peq_dsp_get_loudness_mode(void)
{
    taskENTER_CRITICAL(&s_peq_lock);
    peq_loudness_mode_t mode = s_loudness_mode;
    taskEXIT_CRITICAL(&s_peq_lock);
    return mode;
}

void peq_dsp_notify_volume_pct(int volume_pct)
{
    if (volume_pct < 0) volume_pct = 0;
    if (volume_pct > 100) volume_pct = 100;

    bool dirty = false;
    taskENTER_CRITICAL(&s_peq_lock);
    if (s_volume_pct != volume_pct) {
        s_volume_pct = volume_pct;
        if (s_loudness_mode != PEQ_LOUDNESS_OFF) {
            s_state_gen++;
            dirty = true;
        }
    }
    taskEXIT_CRITICAL(&s_peq_lock);

    if (dirty) {
        stage_pending_from_active(false, false, false);
    }
}

void peq_dsp_set_oled_view(uint8_t view)
{
    if (view >= 4) view = 0;
    taskENTER_CRITICAL(&s_peq_lock);
    s_oled_view = view;
    taskEXIT_CRITICAL(&s_peq_lock);
}

uint8_t peq_dsp_get_oled_view(void)
{
    taskENTER_CRITICAL(&s_peq_lock);
    uint8_t v = s_oled_view;
    taskEXIT_CRITICAL(&s_peq_lock);
    return v;
}

void peq_dsp_set_auto_mute_sec(uint16_t sec)
{
    if (sec > 3600) sec = 3600;
    taskENTER_CRITICAL(&s_peq_lock);
    s_auto_mute_sec = sec;
    taskEXIT_CRITICAL(&s_peq_lock);
}

uint16_t peq_dsp_get_auto_mute_sec(void)
{
    taskENTER_CRITICAL(&s_peq_lock);
    uint16_t s = s_auto_mute_sec;
    taskEXIT_CRITICAL(&s_peq_lock);
    return s;
}

void peq_dsp_set_oled_dim_sec(uint16_t sec)
{
    if (sec > 3600) sec = 3600;
    taskENTER_CRITICAL(&s_peq_lock);
    s_oled_dim_sec = sec;
    taskEXIT_CRITICAL(&s_peq_lock);
}

uint16_t peq_dsp_get_oled_dim_sec(void)
{
    taskENTER_CRITICAL(&s_peq_lock);
    uint16_t s = s_oled_dim_sec;
    taskEXIT_CRITICAL(&s_peq_lock);
    return s;
}

void peq_dsp_get_dynamics_cfg(peq_dynamics_cfg_t *out_cfg)
{
    if (!out_cfg) return;
    taskENTER_CRITICAL(&s_peq_lock);
    out_cfg->limiter_enabled = s_limiter_enabled;
    out_cfg->loudness_mode = s_loudness_mode;
    out_cfg->oled_view = s_oled_view;
    out_cfg->auto_mute_sec = s_auto_mute_sec;
    out_cfg->oled_dim_sec = s_oled_dim_sec;
    out_cfg->fir_enabled = s_fir_enabled;
    out_cfg->fir_taps_l = s_fir_taps_l;
    out_cfg->fir_taps_r = s_fir_taps_r;
    taskEXIT_CRITICAL(&s_peq_lock);
}

void peq_dsp_set_harmonics_cfg(const peq_harmonics_cfg_t *cfg)
{
    if (!cfg) return;
    peq_harmonics_cfg_t clamped = *cfg;
    clamp_harmonics_cfg(&clamped);

    taskENTER_CRITICAL(&s_peq_lock);
    bool changed = (s_harm_cfg.bass_enabled != clamped.bass_enabled ||
                    s_harm_cfg.bass_amount_pct != clamped.bass_amount_pct ||
                    s_harm_cfg.bass_freq_hz != clamped.bass_freq_hz ||
                    s_harm_cfg.exciter_enabled != clamped.exciter_enabled ||
                    s_harm_cfg.exciter_mode != clamped.exciter_mode ||
                    s_harm_cfg.exciter_amount_pct != clamped.exciter_amount_pct ||
                    s_harm_cfg.exciter_freq_hz != clamped.exciter_freq_hz);
    if (!changed) {
        taskEXIT_CRITICAL(&s_peq_lock);
        return;
    }
    s_harm_cfg = clamped;
    s_state_gen++;
    taskEXIT_CRITICAL(&s_peq_lock);

    stage_pending_harmonics(false);
}

void peq_dsp_get_harmonics_cfg(peq_harmonics_cfg_t *out_cfg)
{
    if (!out_cfg) return;
    taskENTER_CRITICAL(&s_peq_lock);
    *out_cfg = s_harm_cfg;
    taskEXIT_CRITICAL(&s_peq_lock);
}

uint16_t peq_dsp_get_fir_max_taps_for_sr(int sample_rate)
{
    return (sample_rate > 48000) ? PEQ_FIR_MAX_TAPS_96K : PEQ_FIR_MAX_TAPS_48K;
}

uint16_t peq_dsp_get_fir_tap_count(peq_channel_t ch)
{
    taskENTER_CRITICAL(&s_peq_lock);
    uint16_t n = (ch == PEQ_CH_RIGHT) ? s_fir_taps_r : s_fir_taps_l;
    taskEXIT_CRITICAL(&s_peq_lock);
    return n;
}

esp_err_t peq_dsp_set_fir_enabled(bool enabled)
{
    taskENTER_CRITICAL(&s_peq_lock);
    s_fir_enabled = enabled;
    s_state_gen++;
    taskEXIT_CRITICAL(&s_peq_lock);
    stage_pending_from_active(false, false, true);
    return ESP_OK;
}

bool peq_dsp_is_fir_enabled(void)
{
    taskENTER_CRITICAL(&s_peq_lock);
    bool en = s_fir_enabled;
    taskEXIT_CRITICAL(&s_peq_lock);
    return en;
}

esp_err_t peq_dsp_upload_fir_chunk(peq_channel_t ch, uint16_t total_taps, uint16_t offset,
                                   uint8_t count, const float *taps, bool commit_nvs)
{
    if (total_taps > PEQ_FIR_MAX_TAPS_48K) return ESP_ERR_INVALID_ARG;

    if (total_taps == 0) {
        taskENTER_CRITICAL(&s_peq_lock);
        if (ch == PEQ_CH_LINKED || ch == PEQ_CH_LEFT) {
            s_fir_taps_l = 0;
            memset(s_fir_upload_l, 0, sizeof(s_fir_upload_l));
        }
        if (ch == PEQ_CH_LINKED || ch == PEQ_CH_RIGHT) {
            s_fir_taps_r = 0;
            memset(s_fir_upload_r, 0, sizeof(s_fir_upload_r));
        }
        if (s_fir_taps_l == 0 && s_fir_taps_r == 0) {
            s_fir_enabled = false;
        }
        s_state_gen++;
        taskEXIT_CRITICAL(&s_peq_lock);
        stage_pending_from_active(false, false, true);
        if (commit_nvs) {
            if (ch == PEQ_CH_LINKED || ch == PEQ_CH_LEFT) save_fir_nvs_internal(PEQ_CH_LEFT);
            if (ch == PEQ_CH_LINKED || ch == PEQ_CH_RIGHT) save_fir_nvs_internal(PEQ_CH_RIGHT);
        }
        return ESP_OK;
    }

    if (!taps || (uint32_t)offset + (uint32_t)count > total_taps) return ESP_ERR_INVALID_ARG;

    for (uint8_t i = 0; i < count; i++) {
        if (!isfinite(taps[i])) return ESP_ERR_INVALID_ARG;
    }

    bool write_l = (ch == PEQ_CH_LINKED || ch == PEQ_CH_LEFT);
    bool write_r = (ch == PEQ_CH_LINKED || ch == PEQ_CH_RIGHT);

    if (write_l) {
        if (offset == 0) {
            memset(s_fir_stage_buf_l, 0, sizeof(s_fir_stage_buf_l));
            s_fir_stage_total_l = total_taps;
        }
        memcpy(&s_fir_stage_buf_l[offset], taps, sizeof(float) * count);
    }
    if (write_r) {
        if (offset == 0) {
            memset(s_fir_stage_buf_r, 0, sizeof(s_fir_stage_buf_r));
            s_fir_stage_total_r = total_taps;
        }
        memcpy(&s_fir_stage_buf_r[offset], taps, sizeof(float) * count);
    }

    if ((uint32_t)offset + (uint32_t)count >= total_taps) {
        taskENTER_CRITICAL(&s_peq_lock);
        if (write_l) {
            s_fir_taps_l = total_taps;
            memcpy(s_fir_upload_l, s_fir_stage_buf_l, sizeof(s_fir_upload_l));
        }
        if (write_r) {
            s_fir_taps_r = total_taps;
            memcpy(s_fir_upload_r, s_fir_stage_buf_r, sizeof(s_fir_upload_r));
        }
        s_fir_enabled = true;
        s_state_gen++;
        taskEXIT_CRITICAL(&s_peq_lock);

        stage_pending_from_active(false, false, true);
        if (commit_nvs) {
            if (write_l) save_fir_nvs_internal(PEQ_CH_LEFT);
            if (write_r) save_fir_nvs_internal(PEQ_CH_RIGHT);
            save_meta_nvs_internal();
        }
    }
    return ESP_OK;
}

void peq_dsp_eval_curve_db(float *out_db, int num_points, float min_hz, float max_hz)
{
    if (!out_db || num_points <= 0) return;

    taskENTER_CRITICAL(&s_peq_lock);
    int sr = s_sample_rate;
    peq_profile_t prof = s_slots[s_active_preset].left;
    bool ab_bypass = s_ab_bypass;
    peq_loudness_mode_t loud_mode = s_loudness_mode;
    int vol_pct = s_volume_pct;
    taskEXIT_CRITICAL(&s_peq_lock);

    if (sr <= 0) sr = 48000;
    biquad_state_t bq[PEQ_MAX_BANDS];
    biquad_state_t loud_bq[2];
    float preamp_lin = 1.0f;
    int nb = PEQ_DEFAULT_NUM_BANDS;
    bool flat = true;
    bool loud_en = false;

    apply_profile_to_biquads(&prof, (float)sr, ab_bypass, bq, &preamp_lin, &nb, &flat);
    compute_loudness_biquads(loud_mode, vol_pct, (float)sr, ab_bypass, loud_bq, &loud_en);

    float preamp_db = isfinite(prof.preamp_gain_db) ? prof.preamp_gain_db : 0.0f;
    float log_min = logf((min_hz > 5.0f) ? min_hz : 20.0f);
    float log_max = logf((max_hz < (float)sr * 0.49f) ? max_hz : ((float)sr * 0.49f));

    for (int i = 0; i < num_points; i++) {
        float t = (num_points > 1) ? ((float)i / (float)(num_points - 1)) : 0.0f;
        float f = expf(log_min + t * (log_max - log_min));
        float w = 2.0f * (float)M_PI * f / (float)sr;
        float cw = cosf(w), sw = sinf(w);
        float c2w = cosf(2.0f * w), s2w = sinf(2.0f * w);

        double mag_sq = 1.0;
        for (int b = 0; b < nb; b++) {
            if (biquad_is_passthrough(&bq[b])) continue;
            double nr = (double)bq[b].b0 + (double)bq[b].b1 * cw + (double)bq[b].b2 * c2w;
            double ni = -((double)bq[b].b1 * sw + (double)bq[b].b2 * s2w);
            double dr = 1.0 + (double)bq[b].a1 * cw + (double)bq[b].a2 * c2w;
            double di = -((double)bq[b].a1 * sw + (double)bq[b].a2 * s2w);
            double denom = dr * dr + di * di;
            if (denom > 1e-18) {
                mag_sq *= (nr * nr + ni * ni) / denom;
            }
        }
        if (loud_en) {
            for (int l = 0; l < 2; l++) {
                if (biquad_is_passthrough(&loud_bq[l])) continue;
                double nr = (double)loud_bq[l].b0 + (double)loud_bq[l].b1 * cw + (double)loud_bq[l].b2 * c2w;
                double ni = -((double)loud_bq[l].b1 * sw + (double)loud_bq[l].b2 * s2w);
                double dr = 1.0 + (double)loud_bq[l].a1 * cw + (double)loud_bq[l].a2 * c2w;
                double di = -((double)loud_bq[l].a1 * sw + (double)loud_bq[l].a2 * s2w);
                double denom = dr * dr + di * di;
                if (denom > 1e-18) {
                    mag_sq *= (nr * nr + ni * ni) / denom;
                }
            }
        }
        float db = (mag_sq > 1e-12) ? (10.0f * log10f((float)mag_sq)) : -60.0f;
        out_db[i] = db + preamp_db;
    }
}

void peq_dsp_get_vu_metrics(peq_vu_metrics_t *out_vu, bool clear_clip)
{
    if (!out_vu) return;
    taskENTER_CRITICAL(&s_peq_lock);
    float pk_l = s_vu_peak_lin_l / FULLSCALE_S24_F;
    float pk_r = s_vu_peak_lin_r / FULLSCALE_S24_F;
    float rm_l = s_vu_rms_lin_l  / FULLSCALE_S24_F;
    float rm_r = s_vu_rms_lin_r  / FULLSCALE_S24_F;
    out_vu->limiter_active = s_vu_lim_active;
    out_vu->clip_latched   = s_vu_clip_latched;
    out_vu->silence_ms     = s_vu_silence_ms;
    /* Reset inter-frame peak/limiter accumulators so each 100 ms UI frame captures true max */
    s_vu_peak_lin_l = 0.0f;
    s_vu_peak_lin_r = 0.0f;
    s_vu_lim_active = false;
    if (clear_clip) {
        s_vu_clip_latched = false;
    }
    taskEXIT_CRITICAL(&s_peq_lock);

    out_vu->peak_db_l = (pk_l > 1e-5f) ? (20.0f * log10f(pk_l)) : -96.0f;
    out_vu->peak_db_r = (pk_r > 1e-5f) ? (20.0f * log10f(pk_r)) : -96.0f;
    out_vu->rms_db_l  = (rm_l > 1e-5f) ? (20.0f * log10f(rm_l)) : -96.0f;
    out_vu->rms_db_r  = (rm_r > 1e-5f) ? (20.0f * log10f(rm_r)) : -96.0f;
}

void peq_dsp_clear_vu_metrics(void)
{
    taskENTER_CRITICAL(&s_peq_lock);
    s_vu_peak_lin_l = 0.0f;
    s_vu_peak_lin_r = 0.0f;
    s_vu_rms_lin_l  = 0.0f;
    s_vu_rms_lin_r  = 0.0f;
    s_vu_lim_active = false;
    taskEXIT_CRITICAL(&s_peq_lock);
}

void peq_dsp_trigger_wake_ramp(void)
{
    taskENTER_CRITICAL(&s_peq_lock);
    s_pending_wake_ramp = true;
    taskEXIT_CRITICAL(&s_peq_lock);
}

bool peq_dsp_is_signal_silent(void)
{
    taskENTER_CRITICAL(&s_peq_lock);
    uint16_t sec = s_auto_mute_sec;
    uint32_t sil_ms = s_vu_silence_ms;
    taskEXIT_CRITICAL(&s_peq_lock);
    return (sec > 0 && sil_ms >= (uint32_t)sec * 1000UL);
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

static inline float dc_block_sample(dc_block_state_t *dc, float in, bool is_right)
{
    float *x1 = is_right ? &dc->x1_r : &dc->x1_l;
    float *y1 = is_right ? &dc->y1_r : &dc->y1_l;
    float out = in - *x1 + dc->r * (*y1);
    *x1 = in;
    *y1 = (fabsf(out) < 1e-15f) ? 0.0f : out;
    return out;
}

static inline float eval_harm_lut(const float *lut, float x)
{
    if (!lut || !isfinite(x)) return 0.0f;
    if (x <= -HARM_LUT_MAX_IN) return lut[0];
    if (x >= HARM_LUT_MAX_IN) return lut[HARM_LUT_SIZE - 1U];
    float pos = (x + HARM_LUT_MAX_IN) * (2047.0f / HARM_LUT_MAX_IN);
    if (pos < 0.0f) pos = 0.0f;
    uint32_t idx = (uint32_t)pos;
    if (idx >= HARM_LUT_SIZE - 1U) {
        return lut[HARM_LUT_SIZE - 1U];
    }
    float frac = pos - (float)idx;
    float y0 = lut[idx];
    float y1 = lut[idx + 1U];
    return y0 + frac * (y1 - y0);
}

static inline float hb_allpass_cascade(float x, float *state, const float *coeffs)
{
    float v = x;
    for (int k = 0; k < HARM_HB_SECTIONS; k++) {
        float t = v - coeffs[k] * state[k];
        float y = coeffs[k] * t + state[k];
        if (fabsf(t) < 1e-15f) t = 0.0f;
        state[k] = t;
        v = y;
    }
    return v;
}

static inline float harm_waveshape_sample(const float *lut, float in,
                                          hb_os2x_state_t *os, bool use_oversampling)
{
    if (!lut) return 0.0f;
    if (!use_oversampling) {
        return eval_harm_lut(lut, in);
    }
    float y0 = hb_allpass_cascade(in, os->up_s0, s_hb_a0);
    float y1 = hb_allpass_cascade(in, os->up_s1, s_hb_a1);
    float w0 = eval_harm_lut(lut, y0);
    float w1 = eval_harm_lut(lut, y1);
    return 0.5f * (hb_allpass_cascade(w1, os->dn_s0, s_hb_a0) +
                   hb_allpass_cascade(w0, os->dn_s1, s_hb_a1));
}

static inline void stage_harm_biquad_target(biquad_state_t *tgt, const biquad_state_t *pend,
                                            const biquad_state_t *act, bool reset_states,
                                            bool in_crossfade)
{
    tgt->b0 = pend->b0;
    tgt->b1 = pend->b1;
    tgt->b2 = pend->b2;
    tgt->a1 = pend->a1;
    tgt->a2 = pend->a2;
    if (reset_states) {
        tgt->w1_l = 0.0f;
        tgt->w2_l = 0.0f;
        tgt->w1_r = 0.0f;
        tgt->w2_r = 0.0f;
    } else if (!in_crossfade) {
        tgt->w1_l = act->w1_l;
        tgt->w2_l = act->w2_l;
        tgt->w1_r = act->w1_r;
        tgt->w2_r = act->w2_r;
    }
}

static inline void reset_harm_bass_states(harm_rt_t *rt)
{
    rt->bass_pre_lpf.w1_l = 0.0f;  rt->bass_pre_lpf.w2_l = 0.0f;
    rt->bass_pre_lpf.w1_r = 0.0f;  rt->bass_pre_lpf.w2_r = 0.0f;
    rt->bass_post_lpf.w1_l = 0.0f; rt->bass_post_lpf.w2_l = 0.0f;
    rt->bass_post_lpf.w1_r = 0.0f; rt->bass_post_lpf.w2_r = 0.0f;
    rt->bass_dc.x1_l = 0.0f;       rt->bass_dc.y1_l = 0.0f;
    rt->bass_dc.x1_r = 0.0f;       rt->bass_dc.y1_r = 0.0f;
    memset(&rt->bass_os_l, 0, sizeof(rt->bass_os_l));
    memset(&rt->bass_os_r, 0, sizeof(rt->bass_os_r));
}

static inline void reset_harm_exc_states(harm_rt_t *rt)
{
    rt->exc_pre_hpf.w1_l = 0.0f;  rt->exc_pre_hpf.w2_l = 0.0f;
    rt->exc_pre_hpf.w1_r = 0.0f;  rt->exc_pre_hpf.w2_r = 0.0f;
    rt->exc_post_hpf.w1_l = 0.0f; rt->exc_post_hpf.w2_l = 0.0f;
    rt->exc_post_hpf.w1_r = 0.0f; rt->exc_post_hpf.w2_r = 0.0f;
    rt->exc_dc.x1_l = 0.0f;       rt->exc_dc.y1_l = 0.0f;
    rt->exc_dc.x1_r = 0.0f;       rt->exc_dc.y1_r = 0.0f;
    memset(&rt->exc_os_l, 0, sizeof(rt->exc_os_l));
    memset(&rt->exc_os_r, 0, sizeof(rt->exc_os_r));
}

static void consume_pending_updates_core1(void)
{
    int sr_for_rebuild = 0;
    bool fir_en_snap = false;
    uint16_t fir_tl = 0, fir_tr = 0;
    bool do_fir = false;

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
        for (int l = 0; l < 2; l++) {
            s_target_loudness_l[l].b0 = s_pending_loudness_bq[l].b0;
            s_target_loudness_l[l].b1 = s_pending_loudness_bq[l].b1;
            s_target_loudness_l[l].b2 = s_pending_loudness_bq[l].b2;
            s_target_loudness_l[l].a1 = s_pending_loudness_bq[l].a1;
            s_target_loudness_l[l].a2 = s_pending_loudness_bq[l].a2;

            s_target_loudness_r[l].b0 = s_pending_loudness_bq[l].b0;
            s_target_loudness_r[l].b1 = s_pending_loudness_bq[l].b1;
            s_target_loudness_r[l].b2 = s_pending_loudness_bq[l].b2;
            s_target_loudness_r[l].a1 = s_pending_loudness_bq[l].a1;
            s_target_loudness_r[l].a2 = s_pending_loudness_bq[l].a2;

            if (s_pending_reset_states) {
                s_target_loudness_l[l].w1_l = 0.0f;
                s_target_loudness_l[l].w2_l = 0.0f;
                s_target_loudness_r[l].w1_r = 0.0f;
                s_target_loudness_r[l].w2_r = 0.0f;
            } else if (!s_in_crossfade) {
                s_target_loudness_l[l].w1_l = s_active_loudness_l[l].w1_l;
                s_target_loudness_l[l].w2_l = s_active_loudness_l[l].w2_l;
                s_target_loudness_r[l].w1_r = s_active_loudness_r[l].w1_r;
                s_target_loudness_r[l].w2_r = s_active_loudness_r[l].w2_r;
            }
        }
        s_target_num_bands_l = s_pending_num_bands_l;
        s_target_num_bands_r = s_pending_num_bands_r;
        s_target_preamp_linear = s_pending_preamp_linear;
        s_target_is_flat = s_pending_is_flat;
        s_target_loudness_en = s_pending_loudness_active;
        s_limiter_rt_en = s_pending_limiter_enabled;
        s_crossfade_step = s_pending_crossfade_step;
        s_crossfade_progress = 0.0f;
        s_in_crossfade = true;
        s_pending_update = false;
        s_pending_reset_states = false;

        if (s_sample_rate != s_rt_sr) {
            sr_for_rebuild = s_sample_rate;
        }
    }
    if (s_harm_pending_update) {
        bool rst = s_harm_pending_reset_states;
        s_harm_target.bass_active = s_harm_pending.bass_active;
        s_harm_target.exc_active = s_harm_pending.exc_active;
        s_harm_target.exc_mode = s_harm_pending.exc_mode;
        s_harm_target.bass_mix = s_harm_pending.bass_mix;
        s_harm_target.exc_mix = s_harm_pending.exc_mix;
        s_harm_target.use_oversampling = s_harm_pending.use_oversampling;

        stage_harm_biquad_target(&s_harm_target.bass_pre_lpf, &s_harm_pending.bass_pre_lpf,
                                 &s_harm_active.bass_pre_lpf, rst, s_harm_in_crossfade);
        stage_harm_biquad_target(&s_harm_target.bass_post_lpf, &s_harm_pending.bass_post_lpf,
                                 &s_harm_active.bass_post_lpf, rst, s_harm_in_crossfade);
        stage_harm_biquad_target(&s_harm_target.exc_pre_hpf, &s_harm_pending.exc_pre_hpf,
                                 &s_harm_active.exc_pre_hpf, rst, s_harm_in_crossfade);
        stage_harm_biquad_target(&s_harm_target.exc_post_hpf, &s_harm_pending.exc_post_hpf,
                                 &s_harm_active.exc_post_hpf, rst, s_harm_in_crossfade);

        s_harm_target.bass_dc.r = s_harm_pending.bass_dc.r;
        s_harm_target.exc_dc.r = s_harm_pending.exc_dc.r;
        if (rst) {
            s_harm_target.bass_dc.x1_l = 0.0f; s_harm_target.bass_dc.y1_l = 0.0f;
            s_harm_target.bass_dc.x1_r = 0.0f; s_harm_target.bass_dc.y1_r = 0.0f;
            s_harm_target.exc_dc.x1_l = 0.0f;  s_harm_target.exc_dc.y1_l = 0.0f;
            s_harm_target.exc_dc.x1_r = 0.0f;  s_harm_target.exc_dc.y1_r = 0.0f;
            memset(&s_harm_target.bass_os_l, 0, sizeof(s_harm_target.bass_os_l));
            memset(&s_harm_target.bass_os_r, 0, sizeof(s_harm_target.bass_os_r));
            memset(&s_harm_target.exc_os_l, 0, sizeof(s_harm_target.exc_os_l));
            memset(&s_harm_target.exc_os_r, 0, sizeof(s_harm_target.exc_os_r));
        } else if (!s_harm_in_crossfade) {
            s_harm_target.bass_dc.x1_l = s_harm_active.bass_dc.x1_l;
            s_harm_target.bass_dc.y1_l = s_harm_active.bass_dc.y1_l;
            s_harm_target.bass_dc.x1_r = s_harm_active.bass_dc.x1_r;
            s_harm_target.bass_dc.y1_r = s_harm_active.bass_dc.y1_r;
            s_harm_target.exc_dc.x1_l = s_harm_active.exc_dc.x1_l;
            s_harm_target.exc_dc.y1_l = s_harm_active.exc_dc.y1_l;
            s_harm_target.exc_dc.x1_r = s_harm_active.exc_dc.x1_r;
            s_harm_target.exc_dc.y1_r = s_harm_active.exc_dc.y1_r;
            s_harm_target.bass_os_l = s_harm_active.bass_os_l;
            s_harm_target.bass_os_r = s_harm_active.bass_os_r;
            s_harm_target.exc_os_l = s_harm_active.exc_os_l;
            s_harm_target.exc_os_r = s_harm_active.exc_os_r;
        }

        s_harm_crossfade_step = s_harm_pending_crossfade_step;
        s_harm_crossfade_progress = 0.0f;
        s_harm_in_crossfade = true;
        s_harm_pending_update = false;
        s_harm_pending_reset_states = false;
    }
    if (s_pending_mixer_update) {
        s_mixer_rt = s_pending_mixer_rt;
        s_pending_mixer_update = false;
    }
    if (s_pending_fir_update) {
        do_fir = true;
        sr_for_rebuild = s_sample_rate;
        fir_en_snap = s_fir_enabled;
        fir_tl = s_fir_taps_l;
        fir_tr = s_fir_taps_r;
        s_pending_fir_update = false;
    }
    if (s_pending_wake_ramp) {
        s_wake_ramp_gain = 0.0f;
        s_pending_wake_ramp = false;
    }
    taskEXIT_CRITICAL(&s_peq_lock);

    if (sr_for_rebuild > 0 && sr_for_rebuild != s_rt_sr) {
        configure_limiter_rt_for_sr(sr_for_rebuild);
    }
    if (do_fir) {
        rebuild_fir_rt_locked(sr_for_rebuild, fir_en_snap, fir_tl, fir_tr);
    }
}

static inline void process_peq_loudness_sample(float in_l, float in_r, float *out_l_ptr, float *out_r_ptr)
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
        if (s_active_loudness_en) {
            act_l = biquad_filter_sample(&s_active_loudness_l[0], act_l,
                                         &s_active_loudness_l[0].w1_l, &s_active_loudness_l[0].w2_l);
            act_l = biquad_filter_sample(&s_active_loudness_l[1], act_l,
                                         &s_active_loudness_l[1].w1_l, &s_active_loudness_l[1].w2_l);
            act_r = biquad_filter_sample(&s_active_loudness_r[0], act_r,
                                         &s_active_loudness_r[0].w1_r, &s_active_loudness_r[0].w2_r);
            act_r = biquad_filter_sample(&s_active_loudness_r[1], act_r,
                                         &s_active_loudness_r[1].w1_r, &s_active_loudness_r[1].w2_r);
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
            if (s_target_loudness_en) {
                tgt_l = biquad_filter_sample(&s_target_loudness_l[0], tgt_l,
                                             &s_target_loudness_l[0].w1_l, &s_target_loudness_l[0].w2_l);
                tgt_l = biquad_filter_sample(&s_target_loudness_l[1], tgt_l,
                                             &s_target_loudness_l[1].w1_l, &s_target_loudness_l[1].w2_l);
                tgt_r = biquad_filter_sample(&s_target_loudness_r[0], tgt_r,
                                             &s_target_loudness_r[0].w1_r, &s_target_loudness_r[0].w2_r);
                tgt_r = biquad_filter_sample(&s_target_loudness_r[1], tgt_r,
                                             &s_target_loudness_r[1].w1_r, &s_target_loudness_r[1].w2_r);
            }

            float p = s_crossfade_progress;
            out_l = (1.0f - p) * act_l + p * tgt_l;
            out_r = (1.0f - p) * act_r + p * tgt_r;

            s_crossfade_progress += s_crossfade_step;
            if (s_crossfade_progress >= 1.0f) {
                memcpy(s_active_biquads_l, s_target_biquads_l, sizeof(s_active_biquads_l));
                memcpy(s_active_biquads_r, s_target_biquads_r, sizeof(s_active_biquads_r));
                memcpy(s_active_loudness_l, s_target_loudness_l, sizeof(s_active_loudness_l));
                memcpy(s_active_loudness_r, s_target_loudness_r, sizeof(s_active_loudness_r));
                s_active_preamp_linear = s_target_preamp_linear;
                s_active_num_bands_l = s_target_num_bands_l;
                s_active_num_bands_r = s_target_num_bands_r;
                s_active_is_flat = s_target_is_flat;
                s_active_loudness_en = s_target_loudness_en;
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

    *out_l_ptr = out_l;
    *out_r_ptr = out_r;
}

static inline void process_harm_rt_sample(harm_rt_t *rt, float in_l, float in_r,
                                          float inv_fs, float fs,
                                          float *out_l_ptr, float *out_r_ptr)
{
    if (!rt->bass_active && !rt->exc_active) {
        *out_l_ptr = in_l;
        *out_r_ptr = in_r;
        return;
    }

    float x_l = in_l * inv_fs;
    float x_r = in_r * inv_fs;
    float add_l = 0.0f;
    float add_r = 0.0f;

    if (rt->bass_active && s_harm_lut_bass) {
        float pre_l = biquad_filter_sample(&rt->bass_pre_lpf, x_l,
                                           &rt->bass_pre_lpf.w1_l, &rt->bass_pre_lpf.w2_l);
        float pre_r = biquad_filter_sample(&rt->bass_pre_lpf, x_r,
                                           &rt->bass_pre_lpf.w1_r, &rt->bass_pre_lpf.w2_r);
        float ws_l = harm_waveshape_sample(s_harm_lut_bass, pre_l,
                                           &rt->bass_os_l, rt->use_oversampling);
        float ws_r = harm_waveshape_sample(s_harm_lut_bass, pre_r,
                                           &rt->bass_os_r, rt->use_oversampling);
        float post_l = biquad_filter_sample(&rt->bass_post_lpf, ws_l,
                                            &rt->bass_post_lpf.w1_l, &rt->bass_post_lpf.w2_l);
        float post_r = biquad_filter_sample(&rt->bass_post_lpf, ws_r,
                                            &rt->bass_post_lpf.w1_r, &rt->bass_post_lpf.w2_r);
        float dc_l = dc_block_sample(&rt->bass_dc, post_l, false);
        float dc_r = dc_block_sample(&rt->bass_dc, post_r, true);
        add_l += rt->bass_mix * dc_l;
        add_r += rt->bass_mix * dc_r;
    }

    if (rt->exc_active) {
        uint32_t m = (uint32_t)rt->exc_mode;
        const float *exc_lut = (m < (uint32_t)PEQ_EXCITER_MODE_COUNT) ? s_harm_lut_exciter[m] : NULL;
        if (exc_lut) {
            float pre_l = biquad_filter_sample(&rt->exc_pre_hpf, x_l,
                                               &rt->exc_pre_hpf.w1_l, &rt->exc_pre_hpf.w2_l);
            float pre_r = biquad_filter_sample(&rt->exc_pre_hpf, x_r,
                                               &rt->exc_pre_hpf.w1_r, &rt->exc_pre_hpf.w2_r);
            float ws_l = harm_waveshape_sample(exc_lut, pre_l,
                                               &rt->exc_os_l, rt->use_oversampling);
            float ws_r = harm_waveshape_sample(exc_lut, pre_r,
                                               &rt->exc_os_r, rt->use_oversampling);
            float post_l = biquad_filter_sample(&rt->exc_post_hpf, ws_l,
                                                &rt->exc_post_hpf.w1_l, &rt->exc_post_hpf.w2_l);
            float post_r = biquad_filter_sample(&rt->exc_post_hpf, ws_r,
                                                &rt->exc_post_hpf.w1_r, &rt->exc_post_hpf.w2_r);
            float dc_l = dc_block_sample(&rt->exc_dc, post_l, false);
            float dc_r = dc_block_sample(&rt->exc_dc, post_r, true);
            add_l += rt->exc_mix * dc_l;
            add_r += rt->exc_mix * dc_r;
        }
    }

    *out_l_ptr = in_l + add_l * fs;
    *out_r_ptr = in_r + add_r * fs;
}

static inline void process_harmonics_sample(float in_l, float in_r,
                                            float inv_fs, float fs,
                                            float *out_l_ptr, float *out_r_ptr)
{
    bool active_any = (s_harm_active.bass_active || s_harm_active.exc_active);
    if (!active_any && !s_harm_in_crossfade) {
        *out_l_ptr = in_l;
        *out_r_ptr = in_r;
        return;
    }

    float act_l, act_r;
    process_harm_rt_sample(&s_harm_active, in_l, in_r, inv_fs, fs, &act_l, &act_r);

    if (!s_harm_in_crossfade) {
        *out_l_ptr = act_l;
        *out_r_ptr = act_r;
        return;
    }

    float tgt_l, tgt_r;
    process_harm_rt_sample(&s_harm_target, in_l, in_r, inv_fs, fs, &tgt_l, &tgt_r);

    float p = s_harm_crossfade_progress;
    *out_l_ptr = (1.0f - p) * act_l + p * tgt_l;
    *out_r_ptr = (1.0f - p) * act_r + p * tgt_r;

    s_harm_crossfade_progress += s_harm_crossfade_step;
    if (s_harm_crossfade_progress >= 1.0f) {
        s_harm_active = s_harm_target;
        s_harm_in_crossfade = false;
        if (!s_harm_active.bass_active) {
            reset_harm_bass_states(&s_harm_active);
        }
        if (!s_harm_active.exc_active) {
            reset_harm_exc_states(&s_harm_active);
        }
    }
}

static inline void process_mixer_sample(float in_l, float in_r, float *out_l_ptr, float *out_r_ptr)
{
    float mx_l = s_mixer_rt.m_ll * in_l + s_mixer_rt.m_rl * in_r;
    float mx_r = s_mixer_rt.m_lr * in_l + s_mixer_rt.m_rr * in_r;

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

static inline void apply_limiter_and_ramp(float in_l, float in_r, float *out_l, float *out_r,
                                          bool *lim_active, bool *clipped)
{
    float l = in_l;
    float r = in_r;

    if (s_limiter_rt_en) {
        float abs_pk = fmaxf(fabsf(in_l), fabsf(in_r));
        float req_g = (abs_pk > PEAK_CEILING_S24) ? (PEAK_CEILING_S24 / abs_pk) : 1.0f;

        if (req_g < s_la_target_g) {
            s_la_target_g = req_g;
            s_la_hold_cnt = s_la_len;
        } else if (s_la_hold_cnt > 0) {
            s_la_hold_cnt--;
        } else {
            s_la_target_g = 1.0f;
        }

        if (s_la_target_g < s_la_gain) {
            s_la_gain += s_la_att_coeff * (s_la_target_g - s_la_gain);
        } else {
            s_la_gain += s_la_rel_coeff * (s_la_target_g - s_la_gain);
        }
        if (s_la_gain > 1.0f) s_la_gain = 1.0f;
        if (s_la_gain < 0.0f) s_la_gain = 0.0f;

        float del_l = s_la_buf_l[s_la_pos];
        float del_r = s_la_buf_r[s_la_pos];
        s_la_buf_l[s_la_pos] = in_l;
        s_la_buf_r[s_la_pos] = in_r;
        s_la_pos++;
        if (s_la_pos >= s_la_len) s_la_pos = 0;

        l = del_l * s_la_gain;
        r = del_r * s_la_gain;
        if (s_la_gain < 0.999f && lim_active) *lim_active = true;
    }

    if (s_wake_ramp_gain < 1.0f) {
        l *= s_wake_ramp_gain;
        r *= s_wake_ramp_gain;
        s_wake_ramp_gain += s_wake_ramp_step;
        if (s_wake_ramp_gain > 1.0f) s_wake_ramp_gain = 1.0f;
    }

    *out_l = soft_clip_s24(l, clipped);
    *out_r = soft_clip_s24(r, clipped);
}

void peq_dsp_process_interleaved_s16(int16_t *samples, size_t num_frames)
{
    if (!samples || num_frames == 0) return;

    consume_pending_updates_core1();

    bool mixer_identity = (!s_mixer_rt.crossfeed_en &&
                           s_mixer_rt.m_ll == 1.0f && s_mixer_rt.m_rl == 0.0f &&
                           s_mixer_rt.m_lr == 0.0f && s_mixer_rt.m_rr == 1.0f);
    bool harm_bypassed = (!s_harm_active.bass_active && !s_harm_active.exc_active && !s_harm_in_crossfade);
    if (s_active_is_flat && !s_in_crossfade && harm_bypassed && mixer_identity) {
        return;
    }

    for (size_t f = 0; f < num_frames; f++) {
        float eq_l, eq_r, hm_l, hm_r, mx_l, mx_r;
        process_peq_loudness_sample((float)samples[2 * f], (float)samples[2 * f + 1], &eq_l, &eq_r);
        process_harmonics_sample(eq_l, eq_r, 1.0f / 32768.0f, 32768.0f, &hm_l, &hm_r);
        process_mixer_sample(hm_l, hm_r, &mx_l, &mx_r);
        samples[2 * f]     = clamp_s16(mx_l);
        samples[2 * f + 1] = clamp_s16(mx_r);
    }
}

static void publish_block_metrics(float pk_l, float pk_r, float sq_sum_l, float sq_sum_r,
                                  size_t total_frames, bool lim_active, bool clipped)
{
    float rms_l = sqrtf(sq_sum_l / (float)total_frames);
    float rms_r = sqrtf(sq_sum_r / (float)total_frames);
    uint32_t sil_ms = (s_rt_sr > 0) ? (uint32_t)((s_silence_samples * 1000ULL) / (uint64_t)s_rt_sr) : 0;

    taskENTER_CRITICAL(&s_peq_lock);
    if (pk_l > s_vu_peak_lin_l) s_vu_peak_lin_l = pk_l;
    if (pk_r > s_vu_peak_lin_r) s_vu_peak_lin_r = pk_r;
    s_vu_rms_lin_l = (rms_l > s_vu_rms_lin_l)
                     ? (0.50f * rms_l + 0.50f * s_vu_rms_lin_l)
                     : (0.15f * rms_l + 0.85f * s_vu_rms_lin_l);
    s_vu_rms_lin_r = (rms_r > s_vu_rms_lin_r)
                     ? (0.50f * rms_r + 0.50f * s_vu_rms_lin_r)
                     : (0.15f * rms_r + 0.85f * s_vu_rms_lin_r);
    if (s_vu_rms_lin_l < 1.0f) s_vu_rms_lin_l = 0.0f;
    if (s_vu_rms_lin_r < 1.0f) s_vu_rms_lin_r = 0.0f;
    if (lim_active) s_vu_lim_active = true;
    if (clipped) s_vu_clip_latched = true;
    s_vu_silence_ms = sil_ms;
    taskEXIT_CRITICAL(&s_peq_lock);
}

void peq_dsp_process_interleaved_s24_to_s32(const uint8_t *src_s24_packed, int32_t *dst_s32,
                                            size_t num_frames, float volume_factor)
{
    if (!src_s24_packed || !dst_s32 || num_frames == 0) return;

    consume_pending_updates_core1();

    float blk_pk_l = 0.0f, blk_pk_r = 0.0f;
    float blk_sq_l = 0.0f, blk_sq_r = 0.0f;
    bool  blk_lim = false, blk_clip = false;

    size_t pos = 0;
    while (pos < num_frames) {
        size_t chunk = num_frames - pos;
        if (chunk > FIR_BLOCK_MAX) chunk = FIR_BLOCK_MAX;

        /* Stage 1: Unpack 24-bit + silence detector + PEQ/Loudness */
        for (size_t i = 0; i < chunk; i++) {
            const uint8_t *p = &src_s24_packed[(pos + i) * 6];
            int32_t s24_l = (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)(int8_t)p[2] << 16));
            int32_t s24_r = (int32_t)((uint32_t)p[3] | ((uint32_t)p[4] << 8) | ((uint32_t)(int8_t)p[5] << 16));

            if (s24_l >= -2 && s24_l <= 2 && s24_r >= -2 && s24_r <= 2) {
                s_silence_samples++;
            } else {
                s_silence_samples = 0;
            }

            float eq_l, eq_r;
            process_peq_loudness_sample((float)s24_l, (float)s24_r, &eq_l, &eq_r);
            s_fir_in_l[i] = eq_l;
            s_fir_in_r[i] = eq_r;
        }

        /* Stage 2: Optional ESP-DSP SIMD FIR Convolution (Core 1 PIE accelerated) */
        const float *ch_l_ptr = s_fir_in_l;
        const float *ch_r_ptr = s_fir_in_r;
        if (s_fir_rt_en && s_fir_rt_active_l) {
            dsps_fir_f32(&s_fir_rt_l, s_fir_in_l, s_fir_out_l, (int)chunk);
            ch_l_ptr = s_fir_out_l;
        }
        if (s_fir_rt_en && s_fir_rt_active_r) {
            dsps_fir_f32(&s_fir_rt_r, s_fir_in_r, s_fir_out_r, (int)chunk);
            ch_r_ptr = s_fir_out_r;
        }

        /* Stage 3: Harmonics -> Mixer / Crossfeed -> Smooth Volume -> Lookahead Limiter -> TPDF Dither -> 24-bit I2S Pack */
        if (s_vol_smooth < 0.0f) {
            s_vol_smooth = volume_factor;
        }
        for (size_t i = 0; i < chunk; i++) {
            float hm_l, hm_r, mx_l, mx_r, fin_l, fin_r;
            s_vol_smooth += s_vol_smooth_coeff * (volume_factor - s_vol_smooth);
            if (fabsf(volume_factor - s_vol_smooth) < 1e-6f) {
                s_vol_smooth = volume_factor;
            }
            float vol_cur = s_vol_smooth;
            process_harmonics_sample(ch_l_ptr[i], ch_r_ptr[i], 1.0f / FULLSCALE_S24_F, FULLSCALE_S24_F, &hm_l, &hm_r);
            process_mixer_sample(hm_l, hm_r, &mx_l, &mx_r);
            apply_limiter_and_ramp(mx_l * vol_cur, mx_r * vol_cur,
                                   &fin_l, &fin_r, &blk_lim, &blk_clip);

            /* Meter pre-volume (post-DSP/mixer) level unless muted so VU stays lively at any knob position */
            float ml = (vol_cur > 0.0f) ? mx_l : 0.0f;
            float mr = (vol_cur > 0.0f) ? mx_r : 0.0f;
            float al = fabsf(ml);
            float ar = fabsf(mr);
            if (al > blk_pk_l) blk_pk_l = al;
            if (ar > blk_pk_r) blk_pk_r = ar;
            blk_sq_l += ml * ml;
            blk_sq_r += mr * mr;

            bool dither_en = (vol_cur > 0.0f && (al > 1.0f || ar > 1.0f));
            dst_s32[(pos + i) * 2]     = clamp_s24_to_s32_dither(fin_l, dither_en);
            dst_s32[(pos + i) * 2 + 1] = clamp_s24_to_s32_dither(fin_r, dither_en);
        }

        pos += chunk;
    }

    publish_block_metrics(blk_pk_l, blk_pk_r, blk_sq_l, blk_sq_r, num_frames, blk_lim, blk_clip);
}

void peq_dsp_process_interleaved_s16_to_s32(const int16_t *src_s16, int32_t *dst_s32,
                                            size_t num_frames, float volume_factor)
{
    if (!src_s16 || !dst_s32 || num_frames == 0) return;

    consume_pending_updates_core1();

    float blk_pk_l = 0.0f, blk_pk_r = 0.0f;
    float blk_sq_l = 0.0f, blk_sq_r = 0.0f;
    bool  blk_lim = false, blk_clip = false;

    size_t pos = 0;
    while (pos < num_frames) {
        size_t chunk = num_frames - pos;
        if (chunk > FIR_BLOCK_MAX) chunk = FIR_BLOCK_MAX;

        for (size_t i = 0; i < chunk; i++) {
            int16_t s_l = src_s16[(pos + i) * 2];
            int16_t s_r = src_s16[(pos + i) * 2 + 1];
            if (s_l == 0 && s_r == 0) {
                s_silence_samples++;
            } else {
                s_silence_samples = 0;
            }

            float eq_l, eq_r;
            process_peq_loudness_sample((float)s_l * 256.0f, (float)s_r * 256.0f, &eq_l, &eq_r);
            s_fir_in_l[i] = eq_l;
            s_fir_in_r[i] = eq_r;
        }

        const float *ch_l_ptr = s_fir_in_l;
        const float *ch_r_ptr = s_fir_in_r;
        if (s_fir_rt_en && s_fir_rt_active_l) {
            dsps_fir_f32(&s_fir_rt_l, s_fir_in_l, s_fir_out_l, (int)chunk);
            ch_l_ptr = s_fir_out_l;
        }
        if (s_fir_rt_en && s_fir_rt_active_r) {
            dsps_fir_f32(&s_fir_rt_r, s_fir_in_r, s_fir_out_r, (int)chunk);
            ch_r_ptr = s_fir_out_r;
        }

        if (s_vol_smooth < 0.0f) {
            s_vol_smooth = volume_factor;
        }
        for (size_t i = 0; i < chunk; i++) {
            float hm_l, hm_r, mx_l, mx_r, fin_l, fin_r;
            s_vol_smooth += s_vol_smooth_coeff * (volume_factor - s_vol_smooth);
            if (fabsf(volume_factor - s_vol_smooth) < 1e-6f) {
                s_vol_smooth = volume_factor;
            }
            float vol_cur = s_vol_smooth;
            process_harmonics_sample(ch_l_ptr[i], ch_r_ptr[i], 1.0f / FULLSCALE_S24_F, FULLSCALE_S24_F, &hm_l, &hm_r);
            process_mixer_sample(hm_l, hm_r, &mx_l, &mx_r);
            apply_limiter_and_ramp(mx_l * vol_cur, mx_r * vol_cur,
                                   &fin_l, &fin_r, &blk_lim, &blk_clip);

            float ml = (vol_cur > 0.0f) ? mx_l : 0.0f;
            float mr = (vol_cur > 0.0f) ? mx_r : 0.0f;
            float al = fabsf(ml);
            float ar = fabsf(mr);
            if (al > blk_pk_l) blk_pk_l = al;
            if (ar > blk_pk_r) blk_pk_r = ar;
            blk_sq_l += ml * ml;
            blk_sq_r += mr * mr;

            bool dither_en = (vol_cur > 0.0f && (al > 1.0f || ar > 1.0f));
            dst_s32[(pos + i) * 2]     = clamp_s24_to_s32_dither(fin_l, dither_en);
            dst_s32[(pos + i) * 2 + 1] = clamp_s24_to_s32_dither(fin_r, dither_en);
        }

        pos += chunk;
    }

    publish_block_metrics(blk_pk_l, blk_pk_r, blk_sq_l, blk_sq_r, num_frames, blk_lim, blk_clip);
}
