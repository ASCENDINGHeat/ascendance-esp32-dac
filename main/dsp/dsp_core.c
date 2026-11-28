#include "dsp_internal.h"
#include "audio_i2s.h"
#include <stdatomic.h>

portMUX_TYPE s_peq_lock = portMUX_INITIALIZER_UNLOCKED;

peq_slot_t s_slots[PEQ_USER_SLOT_COUNT];
bool s_ab_bypass = false;
peq_preset_id_t s_active_preset = PEQ_SLOT_0;
peq_mixer_config_t s_mixer;
int s_sample_rate = 48000;
uint32_t s_state_gen = 0;

/* v0.2.0 Studio Dynamics & Power Configuration (protected by s_peq_lock) */
bool                s_limiter_enabled = true;
peq_loudness_mode_t s_loudness_mode   = PEQ_LOUDNESS_OFF;
int                 s_volume_pct      = 50;
uint8_t             s_oled_view       = 0;
uint16_t            s_auto_mute_sec   = 10;
uint16_t            s_oled_dim_sec    = 60;
bool                s_fir_enabled         = false;
bool                s_auto_preamp_enabled = false;
float               s_auto_preamp_db      = 0.0f;
peq_dsp_pro_cfg_t   s_dsp_pro_cfg;
static _Atomic bool s_tpdf_dither_rt_en   = true;
static _Atomic bool s_subsonic_rt_en      = false;
static dc_block_state_t s_subsonic_dc     = {0};
static _Atomic bool s_dynbass_rt_en       = false;
static _Atomic uint8_t s_dynbass_rt_amt   = 40U;
static biquad_state_t s_dynbass_lpf       = {0};
static float s_dynbass_env                = 0.0f;
static float s_dynbass_gain               = 0.0f;
static float s_dynbass_att_c              = 0.004f;
static float s_dynbass_rel_c              = 0.00017f;
static float s_dynbass_smooth_c           = 0.001f;

/* Psychoacoustic Bass Enhancer & Harmonic Exciter Configuration & PSRAM LUTs */
peq_harmonics_cfg_t s_harm_cfg;
float *s_harm_lut_bass = NULL;
float *s_harm_lut_exciter[PEQ_EXCITER_MODE_COUNT] = { NULL, NULL, NULL };

/* 13th-order elliptic 2x half-band polyphase allpass coefficients (>94 dB stopband) */
static const float s_hb_a0[HARM_HB_SECTIONS] = { 0.04536216f, 0.33714969f, 0.70806414f };
static const float s_hb_a1[HARM_HB_SECTIONS] = { 0.16808748f, 0.52237785f, 0.89744559f };

/* Uploaded FIR impulse responses in DRAM */
float    s_fir_upload_l[PEQ_FIR_MAX_TAPS_48K];
float    s_fir_upload_r[PEQ_FIR_MAX_TAPS_48K];
uint16_t s_fir_taps_l = 0;
uint16_t s_fir_taps_r = 0;
float    s_fir_stage_buf_l[PEQ_FIR_MAX_TAPS_48K];
float    s_fir_stage_buf_r[PEQ_FIR_MAX_TAPS_48K];
uint16_t s_fir_stage_total_l = 0;
uint16_t s_fir_stage_total_r = 0;
bool     s_pending_fir_update = false;

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
__attribute__((aligned(16))) float s_fir_coeffs_l[PEQ_FIR_MAX_TAPS_48K];
__attribute__((aligned(16))) float s_fir_coeffs_r[PEQ_FIR_MAX_TAPS_48K];
__attribute__((aligned(16))) float s_fir_delay_l[PEQ_FIR_MAX_TAPS_48K + 16];
__attribute__((aligned(16))) float s_fir_delay_r[PEQ_FIR_MAX_TAPS_48K + 16];
static __attribute__((aligned(16))) float s_fir_in_l[FIR_BLOCK_MAX];
static __attribute__((aligned(16))) float s_fir_in_r[FIR_BLOCK_MAX];
static __attribute__((aligned(16))) float s_fir_out_l[FIR_BLOCK_MAX];
static __attribute__((aligned(16))) float s_fir_out_r[FIR_BLOCK_MAX];
fir_f32_t s_fir_rt_l;
fir_f32_t s_fir_rt_r;
bool      s_fir_rt_active_l = false;
bool      s_fir_rt_active_r = false;
bool      s_fir_rt_en       = false;

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
/* Lock-free VU & Auto-Mute telemetry (written on Core 1 without spinlocks, read on Core 0) */
static _Atomic uint32_t s_vu_peak_u32_l   = 0U;
static _Atomic uint32_t s_vu_peak_u32_r   = 0U;
static _Atomic uint32_t s_vu_rms_u32_l    = 0U;
static _Atomic uint32_t s_vu_rms_u32_r    = 0U;
static _Atomic bool     s_vu_lim_active   = false;
static _Atomic bool     s_vu_clip_latched = false;
static _Atomic uint32_t s_vu_silence_ms   = 0U;
static _Atomic uint32_t s_vu_clear_gen    = 0U;
static uint32_t         s_vu_clear_seen   = 0U;
static float            s_vu_rms_rt_l     = 0.0f;
static float            s_vu_rms_rt_r     = 0.0f;

/* Lock-free 1024-sample mono snapshot for Core 0 32-band FFT Spectrum OLED view */
static float            s_fft_snap[PEQ_FFT_SIZE];
static uint32_t         s_fft_wr          = 0U;
static _Atomic bool     s_fft_snap_req    = false;
static _Atomic bool     s_fft_snap_ready  = false;
static _Atomic int      s_fft_snap_sr     = 48000;


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


void stage_pending_from_active_ex(bool reset_states, bool stage_mixer, bool stage_fir, bool stage_harm)
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
        bool auto_preamp_en = s_auto_preamp_enabled;
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

        float computed_auto_db = 0.0f;
        if (auto_preamp_en) {
            computed_auto_db = compute_auto_preamp_db(local_bq_l, local_nb_l, local_bq_r, local_nb_r,
                                                      local_loud, local_loud_en, &harm_snap, ab_bypass, (float)sr);
            float auto_lin = powf(10.0f, computed_auto_db / 20.0f);
            if (isfinite(auto_lin) && auto_lin < local_preamp_l) {
                local_preamp_l = auto_lin;
                if (computed_auto_db < -1e-4f) {
                    local_flat_l = false;
                }
            }
        }

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
            s_auto_preamp_db = computed_auto_db;
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

void stage_pending_from_active(bool reset_states, bool stage_mixer, bool stage_fir)
{
    stage_pending_from_active_ex(reset_states, stage_mixer, stage_fir, false);
}

void stage_pending_harmonics(bool reset_states)
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

    /* 1st-order 18 Hz Subsonic / DC Blocker pole: r = exp(-2*pi*18 / fs) */
    s_subsonic_dc.r = expf(-2.0f * (float)M_PI * 18.0f / (float)sample_rate);
    s_subsonic_dc.x1_l = 0.0f; s_subsonic_dc.y1_l = 0.0f;
    s_subsonic_dc.x1_r = 0.0f; s_subsonic_dc.y1_r = 0.0f;

    /* Dynamic Bass 2nd-order 90 Hz Butterworth LPF + 5ms/120ms envelope follower */
    calc_butterworth_lpf(90.0, (double)sample_rate, &s_dynbass_lpf);
    s_dynbass_lpf.w1_l = 0.0f; s_dynbass_lpf.w2_l = 0.0f;
    s_dynbass_lpf.w1_r = 0.0f; s_dynbass_lpf.w2_r = 0.0f;
    s_dynbass_env = 0.0f;
    s_dynbass_gain = 0.0f;
    s_dynbass_att_c = 1.0f - expf(-1.0f / (0.005f * (float)sample_rate));
    s_dynbass_rel_c = 1.0f - expf(-1.0f / (0.120f * (float)sample_rate));
    s_dynbass_smooth_c = 1.0f - expf(-1.0f / (0.020f * (float)sample_rate));
}


esp_err_t peq_dsp_init(int sample_rate)
{
    if (sample_rate <= 0) sample_rate = 48000;

    setup_default_slots();
    setup_default_dsp_pro(&s_dsp_pro_cfg);
    load_user_profile_nvs();
    atomic_store_explicit(&s_tpdf_dither_rt_en, s_dsp_pro_cfg.tpdf_dither_enabled, memory_order_release);
    atomic_store_explicit(&s_subsonic_rt_en, s_dsp_pro_cfg.subsonic_filter_enabled, memory_order_release);
    atomic_store_explicit(&s_dynbass_rt_en, s_dsp_pro_cfg.dynamic_bass_enabled, memory_order_release);
    atomic_store_explicit(&s_dynbass_rt_amt, s_dsp_pro_cfg.dynamic_bass_amount_pct, memory_order_release);
    audio_pipeline_set_log_volume_taper(s_dsp_pro_cfg.vol_taper_log_enabled);
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
    err = save_harm_nvs_internal();
    if (err != ESP_OK) return err;
    return save_dsp_pro_nvs_internal();
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

void peq_dsp_set_auto_preamp_enabled(bool enabled)
{
    taskENTER_CRITICAL(&s_peq_lock);
    if (s_auto_preamp_enabled == enabled) {
        taskEXIT_CRITICAL(&s_peq_lock);
        return;
    }
    s_auto_preamp_enabled = enabled;
    s_state_gen++;
    taskEXIT_CRITICAL(&s_peq_lock);
    stage_pending_from_active(false, false, false);
}

bool peq_dsp_is_auto_preamp_enabled(void)
{
    taskENTER_CRITICAL(&s_peq_lock);
    bool en = s_auto_preamp_enabled;
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
    if (view >= 5) view = 0;
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
    out_cfg->auto_preamp_enabled = s_auto_preamp_enabled;
    out_cfg->auto_preamp_db = s_auto_preamp_db;
    taskEXIT_CRITICAL(&s_peq_lock);
}

void peq_dsp_set_pro_cfg(const peq_dsp_pro_cfg_t *cfg)
{
    if (!cfg) return;
    peq_dsp_pro_cfg_t clamped = *cfg;
    if (clamped.dynamic_bass_amount_pct > 100U) {
        clamped.dynamic_bass_amount_pct = 100U;
    }
    taskENTER_CRITICAL(&s_peq_lock);
    s_dsp_pro_cfg = clamped;
    taskEXIT_CRITICAL(&s_peq_lock);
    atomic_store_explicit(&s_tpdf_dither_rt_en, clamped.tpdf_dither_enabled, memory_order_release);
    atomic_store_explicit(&s_subsonic_rt_en, clamped.subsonic_filter_enabled, memory_order_release);
    atomic_store_explicit(&s_dynbass_rt_en, clamped.dynamic_bass_enabled, memory_order_release);
    atomic_store_explicit(&s_dynbass_rt_amt, clamped.dynamic_bass_amount_pct, memory_order_release);
    audio_pipeline_set_log_volume_taper(clamped.vol_taper_log_enabled);
}

void peq_dsp_get_pro_cfg(peq_dsp_pro_cfg_t *out_cfg)
{
    if (!out_cfg) return;
    taskENTER_CRITICAL(&s_peq_lock);
    *out_cfg = s_dsp_pro_cfg;
    taskEXIT_CRITICAL(&s_peq_lock);
}


void peq_dsp_get_vu_metrics(peq_vu_metrics_t *out_vu, bool clear_clip)
{
    if (!out_vu) return;
    /* Atomic read-and-reset of inter-frame peak/limiter accumulators without blocking Core 1 */
    float pk_l = (float)atomic_exchange_explicit(&s_vu_peak_u32_l, 0U, memory_order_acq_rel) / FULLSCALE_S24_F;
    float pk_r = (float)atomic_exchange_explicit(&s_vu_peak_u32_r, 0U, memory_order_acq_rel) / FULLSCALE_S24_F;
    float rm_l = (float)atomic_load_explicit(&s_vu_rms_u32_l, memory_order_acquire) / FULLSCALE_S24_F;
    float rm_r = (float)atomic_load_explicit(&s_vu_rms_u32_r, memory_order_acquire) / FULLSCALE_S24_F;
    out_vu->limiter_active = atomic_exchange_explicit(&s_vu_lim_active, false, memory_order_acq_rel);
    out_vu->clip_latched   = clear_clip
                             ? atomic_exchange_explicit(&s_vu_clip_latched, false, memory_order_acq_rel)
                             : atomic_load_explicit(&s_vu_clip_latched, memory_order_acquire);
    out_vu->silence_ms     = atomic_load_explicit(&s_vu_silence_ms, memory_order_acquire);

    out_vu->peak_db_l = (pk_l > 1e-5f) ? (20.0f * log10f(pk_l)) : -96.0f;
    out_vu->peak_db_r = (pk_r > 1e-5f) ? (20.0f * log10f(pk_r)) : -96.0f;
    out_vu->rms_db_l  = (rm_l > 1e-5f) ? (20.0f * log10f(rm_l)) : -96.0f;
    out_vu->rms_db_r  = (rm_r > 1e-5f) ? (20.0f * log10f(rm_r)) : -96.0f;
}

void peq_dsp_clear_vu_metrics(void)
{
    atomic_store_explicit(&s_vu_peak_u32_l, 0U, memory_order_relaxed);
    atomic_store_explicit(&s_vu_peak_u32_r, 0U, memory_order_relaxed);
    atomic_store_explicit(&s_vu_rms_u32_l, 0U, memory_order_relaxed);
    atomic_store_explicit(&s_vu_rms_u32_r, 0U, memory_order_relaxed);
    atomic_store_explicit(&s_vu_lim_active, false, memory_order_relaxed);
    atomic_fetch_add_explicit(&s_vu_clear_gen, 1U, memory_order_release);
}

bool peq_dsp_read_fft_snapshot(float out_samples[PEQ_FFT_SIZE], int *out_sample_rate)
{
    if (!out_samples) return false;
    if (atomic_exchange_explicit(&s_fft_snap_ready, false, memory_order_acq_rel)) {
        memcpy(out_samples, s_fft_snap, sizeof(float) * PEQ_FFT_SIZE);
        if (out_sample_rate) {
            *out_sample_rate = atomic_load_explicit(&s_fft_snap_sr, memory_order_acquire);
        }
        atomic_store_explicit(&s_fft_snap_req, true, memory_order_release);
        return true;
    }
    if (!atomic_load_explicit(&s_fft_snap_req, memory_order_relaxed)) {
        atomic_store_explicit(&s_fft_snap_req, true, memory_order_release);
    }
    return false;
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
    taskEXIT_CRITICAL(&s_peq_lock);
    uint32_t sil_ms = atomic_load_explicit(&s_vu_silence_ms, memory_order_acquire);
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
    if (s_mixer_rt.identity) {
        *out_l_ptr = in_l;
        *out_r_ptr = in_r;
        return;
    }

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

static inline void atomic_max_u32(_Atomic uint32_t *dst, uint32_t val)
{
    uint32_t cur = atomic_load_explicit(dst, memory_order_relaxed);
    while (val > cur && !atomic_compare_exchange_weak_explicit(
               dst, &cur, val, memory_order_release, memory_order_relaxed)) {
    }
}

static void publish_block_metrics(float pk_l, float pk_r, float sq_sum_l, float sq_sum_r,
                                  size_t total_frames, bool lim_active, bool clipped)
{
    float rms_l = sqrtf(sq_sum_l / (float)total_frames);
    float rms_r = sqrtf(sq_sum_r / (float)total_frames);
    uint32_t sil_ms = (s_rt_sr > 0) ? (uint32_t)((s_silence_samples * 1000ULL) / (uint64_t)s_rt_sr) : 0U;

    uint32_t clr_gen = atomic_load_explicit(&s_vu_clear_gen, memory_order_acquire);
    if (clr_gen != s_vu_clear_seen) {
        s_vu_rms_rt_l = 0.0f;
        s_vu_rms_rt_r = 0.0f;
        s_vu_clear_seen = clr_gen;
    }

    s_vu_rms_rt_l = (rms_l > s_vu_rms_rt_l)
                    ? (0.50f * rms_l + 0.50f * s_vu_rms_rt_l)
                    : (0.15f * rms_l + 0.85f * s_vu_rms_rt_l);
    s_vu_rms_rt_r = (rms_r > s_vu_rms_rt_r)
                    ? (0.50f * rms_r + 0.50f * s_vu_rms_rt_r)
                    : (0.15f * rms_r + 0.85f * s_vu_rms_rt_r);
    if (s_vu_rms_rt_l < 1.0f) s_vu_rms_rt_l = 0.0f;
    if (s_vu_rms_rt_r < 1.0f) s_vu_rms_rt_r = 0.0f;

    uint32_t pk_l_u = (pk_l > 0.0f) ? (uint32_t)pk_l : 0U;
    uint32_t pk_r_u = (pk_r > 0.0f) ? (uint32_t)pk_r : 0U;
    atomic_max_u32(&s_vu_peak_u32_l, pk_l_u);
    atomic_max_u32(&s_vu_peak_u32_r, pk_r_u);
    atomic_store_explicit(&s_vu_rms_u32_l, (uint32_t)s_vu_rms_rt_l, memory_order_release);
    atomic_store_explicit(&s_vu_rms_u32_r, (uint32_t)s_vu_rms_rt_r, memory_order_release);
    if (lim_active) atomic_store_explicit(&s_vu_lim_active, true, memory_order_release);
    if (clipped)    atomic_store_explicit(&s_vu_clip_latched, true, memory_order_release);
    atomic_store_explicit(&s_vu_silence_ms, sil_ms, memory_order_release);
}

static inline void process_dsp_chunk_f32_to_s32(size_t chunk, int32_t *dst_s32, float volume_factor,
                                                float *blk_pk_l, float *blk_pk_r,
                                                float *blk_sq_l, float *blk_sq_r,
                                                bool *blk_lim, bool *blk_clip)
{
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
    float pk_l = *blk_pk_l;
    float pk_r = *blk_pk_r;
    float sq_l = *blk_sq_l;
    float sq_r = *blk_sq_r;
    bool  tpdf_en = atomic_load_explicit(&s_tpdf_dither_rt_en, memory_order_relaxed);
    bool  subsonic_en = atomic_load_explicit(&s_subsonic_rt_en, memory_order_relaxed);
    bool  dynbass_en = atomic_load_explicit(&s_dynbass_rt_en, memory_order_relaxed);
    float dynbass_max = (float)atomic_load_explicit(&s_dynbass_rt_amt, memory_order_relaxed) * 0.01f;
    bool  dynbass_run = dynbass_en || (s_dynbass_gain >= 1e-4f);
    bool  fft_cap = atomic_load_explicit(&s_fft_snap_req, memory_order_acquire);
    if (!subsonic_en) {
        s_subsonic_dc.x1_l = 0.0f; s_subsonic_dc.y1_l = 0.0f;
        s_subsonic_dc.x1_r = 0.0f; s_subsonic_dc.y1_r = 0.0f;
    }
    if (!dynbass_run) {
        s_dynbass_lpf.w1_l = 0.0f; s_dynbass_lpf.w2_l = 0.0f;
        s_dynbass_lpf.w1_r = 0.0f; s_dynbass_lpf.w2_r = 0.0f;
        s_dynbass_env = 0.0f;
        s_dynbass_gain = 0.0f;
    }
    for (size_t i = 0; i < chunk; i++) {
        float hm_l, hm_r, mx_l, mx_r, fin_l, fin_r;
        s_vol_smooth += s_vol_smooth_coeff * (volume_factor - s_vol_smooth);
        if (fabsf(volume_factor - s_vol_smooth) < 1e-6f) {
            s_vol_smooth = volume_factor;
        }
        float vol_cur = s_vol_smooth;
        process_harmonics_sample(ch_l_ptr[i], ch_r_ptr[i], 1.0f / FULLSCALE_S24_F, FULLSCALE_S24_F, &hm_l, &hm_r);
        if (dynbass_run) {
            float sub_l = biquad_filter_sample(&s_dynbass_lpf, hm_l, &s_dynbass_lpf.w1_l, &s_dynbass_lpf.w2_l);
            float sub_r = biquad_filter_sample(&s_dynbass_lpf, hm_r, &s_dynbass_lpf.w1_r, &s_dynbass_lpf.w2_r);
            float sub_pk = fmaxf(fabsf(sub_l), fabsf(sub_r)) * (1.0f / FULLSCALE_S24_F);
            s_dynbass_env += ((sub_pk > s_dynbass_env) ? s_dynbass_att_c : s_dynbass_rel_c) * (sub_pk - s_dynbass_env);
            float duck = (s_dynbass_env <= 0.125f) ? 1.0f
                       : ((s_dynbass_env >= 0.707f) ? 0.0f : (0.707f - s_dynbass_env) * (1.0f / (0.707f - 0.125f)));
            float tgt_g = dynbass_en ? (dynbass_max * duck) : 0.0f;
            s_dynbass_gain += s_dynbass_smooth_c * (tgt_g - s_dynbass_gain);
            hm_l += s_dynbass_gain * sub_l;
            hm_r += s_dynbass_gain * sub_r;
        }
        process_mixer_sample(hm_l, hm_r, &mx_l, &mx_r);
        if (subsonic_en) {
            mx_l = dc_block_sample(&s_subsonic_dc, mx_l, false);
            mx_r = dc_block_sample(&s_subsonic_dc, mx_r, true);
        }
        apply_limiter_and_ramp(mx_l * vol_cur, mx_r * vol_cur,
                               &fin_l, &fin_r, blk_lim, blk_clip);

        /* Meter true post-DSP output (post-volume, post-limiter, post-soft-clip, pre-dither) */
        float al = fabsf(fin_l);
        float ar = fabsf(fin_r);
        if (al > pk_l) pk_l = al;
        if (ar > pk_r) pk_r = ar;
        sq_l += fin_l * fin_l;
        sq_r += fin_r * fin_r;

        if (fft_cap) {
            s_fft_snap[s_fft_wr++] = 0.5f * (fin_l + fin_r) * (1.0f / FULLSCALE_S24_F);
            if (s_fft_wr >= PEQ_FFT_SIZE) {
                s_fft_wr = 0U;
                fft_cap = false;
                atomic_store_explicit(&s_fft_snap_sr, s_rt_sr, memory_order_relaxed);
                atomic_store_explicit(&s_fft_snap_req, false, memory_order_relaxed);
                atomic_store_explicit(&s_fft_snap_ready, true, memory_order_release);
            }
        }

        bool dither_en = tpdf_en && (al > 0.5f || ar > 0.5f);
        dst_s32[i * 2]     = clamp_s24_to_s32_dither(fin_l, dither_en);
        dst_s32[i * 2 + 1] = clamp_s24_to_s32_dither(fin_r, dither_en);
    }
    *blk_pk_l = pk_l;
    *blk_pk_r = pk_r;
    *blk_sq_l = sq_l;
    *blk_sq_r = sq_r;
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

        process_dsp_chunk_f32_to_s32(chunk, &dst_s32[pos * 2], volume_factor,
                                     &blk_pk_l, &blk_pk_r, &blk_sq_l, &blk_sq_r,
                                     &blk_lim, &blk_clip);
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

        process_dsp_chunk_f32_to_s32(chunk, &dst_s32[pos * 2], volume_factor,
                                     &blk_pk_l, &blk_pk_r, &blk_sq_l, &blk_sq_r,
                                     &blk_lim, &blk_clip);
        pos += chunk;
    }

    publish_block_metrics(blk_pk_l, blk_pk_r, blk_sq_l, blk_sq_r, num_frames, blk_lim, blk_clip);
}
