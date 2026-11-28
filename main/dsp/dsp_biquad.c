#include "dsp_internal.h"


static bool filter_uses_gain(peq_filter_type_t type)
{
    return (type == PEQ_FILTER_PEAKING ||
            type == PEQ_FILTER_LOW_SHELF ||
            type == PEQ_FILTER_HIGH_SHELF);
}

void calculate_biquad(const peq_band_param_t *band, float sample_rate, biquad_state_t *out_bq)
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

bool biquad_is_passthrough(const biquad_state_t *bq)
{
    return (bq->b0 == 1.0f && bq->b1 == 0.0f && bq->b2 == 0.0f &&
            bq->a1 == 0.0f && bq->a2 == 0.0f);
}

void compute_loudness_biquads(peq_loudness_mode_t mode, int vol_pct, float sample_rate,
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

void apply_profile_to_biquads(const peq_profile_t *prof, float sample_rate, bool ab_bypass,
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


void calc_butterworth_lpf(double fc, double sr, biquad_state_t *out)
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

void calc_butterworth_hpf(double fc, double sr, biquad_state_t *out)
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


float compute_auto_preamp_db(const biquad_state_t *bq_l, int nb_l,
                                    const biquad_state_t *bq_r, int nb_r,
                                    const biquad_state_t *loud_bq, bool loud_en,
                                    const peq_harmonics_cfg_t *harm, bool ab_bypass,
                                    float sample_rate)
{
    if (ab_bypass || sample_rate <= 0.0f) {
        return 0.0f;
    }

    const int num_pts = 48;
    float log_min = logf(20.0f);
    float max_f = sample_rate * 0.48f;
    if (max_f > 20000.0f) max_f = 20000.0f;
    float log_max = logf(max_f);
    double max_mag_sq = 1.0;

    for (int i = 0; i < num_pts; i++) {
        float t = (float)i / (float)(num_pts - 1);
        float f = expf(log_min + t * (log_max - log_min));
        float w = 2.0f * (float)M_PI * f / sample_rate;
        double cw = (double)cosf(w), sw = (double)sinf(w);
        double c2w = (double)cosf(2.0f * w), s2w = (double)sinf(2.0f * w);

        double mag_l = 1.0;
        for (int b = 0; b < nb_l; b++) {
            if (biquad_is_passthrough(&bq_l[b])) continue;
            double nr = (double)bq_l[b].b0 + (double)bq_l[b].b1 * cw + (double)bq_l[b].b2 * c2w;
            double ni = -((double)bq_l[b].b1 * sw + (double)bq_l[b].b2 * s2w);
            double dr = 1.0 + (double)bq_l[b].a1 * cw + (double)bq_l[b].a2 * c2w;
            double di = -((double)bq_l[b].a1 * sw + (double)bq_l[b].a2 * s2w);
            double denom = dr * dr + di * di;
            if (denom > 1e-18) mag_l *= (nr * nr + ni * ni) / denom;
        }

        double mag_r = 1.0;
        for (int b = 0; b < nb_r; b++) {
            if (biquad_is_passthrough(&bq_r[b])) continue;
            double nr = (double)bq_r[b].b0 + (double)bq_r[b].b1 * cw + (double)bq_r[b].b2 * c2w;
            double ni = -((double)bq_r[b].b1 * sw + (double)bq_r[b].b2 * s2w);
            double dr = 1.0 + (double)bq_r[b].a1 * cw + (double)bq_r[b].a2 * c2w;
            double di = -((double)bq_r[b].a1 * sw + (double)bq_r[b].a2 * s2w);
            double denom = dr * dr + di * di;
            if (denom > 1e-18) mag_r *= (nr * nr + ni * ni) / denom;
        }

        double mag_loud = 1.0;
        if (loud_en && loud_bq) {
            for (int l = 0; l < 2; l++) {
                if (biquad_is_passthrough(&loud_bq[l])) continue;
                double nr = (double)loud_bq[l].b0 + (double)loud_bq[l].b1 * cw + (double)loud_bq[l].b2 * c2w;
                double ni = -((double)loud_bq[l].b1 * sw + (double)loud_bq[l].b2 * s2w);
                double dr = 1.0 + (double)loud_bq[l].a1 * cw + (double)loud_bq[l].a2 * c2w;
                double di = -((double)loud_bq[l].a1 * sw + (double)loud_bq[l].a2 * s2w);
                double denom = dr * dr + di * di;
                if (denom > 1e-18) mag_loud *= (nr * nr + ni * ni) / denom;
            }
        }

        double peak_lr = (mag_l > mag_r) ? mag_l : mag_r;
        double total = peak_lr * mag_loud;
        if (total > max_mag_sq) {
            max_mag_sq = total;
        }
    }

    float peak_boost_db = (max_mag_sq > 1.0) ? (10.0f * log10f((float)max_mag_sq)) : 0.0f;

    /* Account for additive harmonic energy when Bass Enhancer / Exciter are active */
    if (harm) {
        if (harm->bass_enabled && harm->bass_amount_pct > 0U) {
            peak_boost_db += 2.5f * ((float)harm->bass_amount_pct / 100.0f);
        }
        if (harm->exciter_enabled && harm->exciter_amount_pct > 0U) {
            peak_boost_db += 1.5f * ((float)harm->exciter_amount_pct / 100.0f);
        }
    }

    if (peak_boost_db <= 0.05f) {
        return 0.0f;
    }
    float auto_db = -peak_boost_db;
    if (auto_db < -12.0f) auto_db = -12.0f;
    return auto_db;
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

