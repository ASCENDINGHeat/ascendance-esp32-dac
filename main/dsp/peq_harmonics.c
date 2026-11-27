#include "peq_dsp_internal.h"

void clamp_harmonics_cfg(peq_harmonics_cfg_t *cfg)
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

void setup_default_harmonics(peq_harmonics_cfg_t *cfg)
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


void compute_harm_rt(const peq_harmonics_cfg_t *cfg, int sample_rate,
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

void init_harmonics_luts_once(void)
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


void peq_dsp_set_harmonics_cfg(const peq_harmonics_cfg_t *cfg)
{
    if (!cfg) return;
    peq_harmonics_cfg_t clamped = *cfg;
    clamp_harmonics_cfg(&clamped);

    bool need_auto_preamp = false;
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
    need_auto_preamp = s_auto_preamp_enabled;
    s_state_gen++;
    taskEXIT_CRITICAL(&s_peq_lock);

    if (need_auto_preamp) {
        stage_pending_from_active_ex(false, false, false, true);
    } else {
        stage_pending_harmonics(false);
    }
}

void peq_dsp_get_harmonics_cfg(peq_harmonics_cfg_t *out_cfg)
{
    if (!out_cfg) return;
    taskENTER_CRITICAL(&s_peq_lock);
    *out_cfg = s_harm_cfg;
    taskEXIT_CRITICAL(&s_peq_lock);
}

