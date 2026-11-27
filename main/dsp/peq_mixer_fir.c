#include "peq_dsp_internal.h"

void compute_mixer_rt(const peq_mixer_config_t *cfg, float sample_rate, mixer_rt_t *out)
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

void setup_default_mixer(peq_mixer_config_t *cfg)
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


void rebuild_fir_rt_locked(int sr, bool en, uint16_t taps_l, uint16_t taps_r)
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

