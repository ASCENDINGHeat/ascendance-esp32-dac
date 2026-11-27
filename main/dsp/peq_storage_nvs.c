#include "peq_dsp_internal.h"

uint32_t calc_crc32(const uint8_t *data, size_t len)
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


void init_flat_profile(peq_profile_t *prof, const char *slot_name)
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

void setup_default_slots(void)
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

void sanitize_profile(peq_profile_t *prof)
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

esp_err_t save_slot_nvs_internal(int slot_id)
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

esp_err_t save_meta_nvs_internal(void)
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
    meta.reserved = s_auto_preamp_enabled ? 0x01 : 0x00;
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

void save_fir_nvs_internal(peq_channel_t ch)
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

esp_err_t save_harm_nvs_internal(void)
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

void setup_default_dsp_pro(peq_dsp_pro_cfg_t *cfg)
{
    if (!cfg) return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->tpdf_dither_enabled = true;
    cfg->subsonic_filter_enabled = false;
    cfg->vol_taper_log_enabled = false;
    cfg->dynamic_bass_enabled = false;
    cfg->dynamic_bass_amount_pct = 40;
}

esp_err_t save_dsp_pro_nvs_internal(void)
{
    peq_dsp_pro_nvs_v1_t blob;
    memset(&blob, 0, sizeof(blob));
    blob.magic = PEQ_DSP_PRO_MAGIC_V1;
    blob.version = PEQ_DSP_PRO_VERSION_V1;

    taskENTER_CRITICAL(&s_peq_lock);
    peq_dsp_pro_cfg_t snap = s_dsp_pro_cfg;
    taskEXIT_CRITICAL(&s_peq_lock);

    if (snap.dynamic_bass_amount_pct > 100) snap.dynamic_bass_amount_pct = 100;
    blob.flags = (snap.tpdf_dither_enabled ? 0x01U : 0U) |
                 (snap.subsonic_filter_enabled ? 0x02U : 0U) |
                 (snap.vol_taper_log_enabled ? 0x04U : 0U) |
                 (snap.dynamic_bass_enabled ? 0x08U : 0U);
    blob.dynamic_bass_amount_pct = snap.dynamic_bass_amount_pct;
    blob.crc32 = calc_crc32((const uint8_t *)&blob, offsetof(peq_dsp_pro_nvs_v1_t, crc32));

    nvs_handle_t nvs_h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_h);
    if (err != ESP_OK) return err;

    size_t sz = sizeof(peq_dsp_pro_nvs_v1_t);
    peq_dsp_pro_nvs_v1_t existing;
    bool needs_write = true;
    if (nvs_get_blob(nvs_h, NVS_KEY_DSP_PRO_V1, &existing, &sz) == ESP_OK &&
        sz == sizeof(peq_dsp_pro_nvs_v1_t) &&
        memcmp(&existing, &blob, sizeof(peq_dsp_pro_nvs_v1_t)) == 0) {
        needs_write = false;
    }

    if (needs_write) {
        err = nvs_set_blob(nvs_h, NVS_KEY_DSP_PRO_V1, &blob, sizeof(peq_dsp_pro_nvs_v1_t));
        if (err == ESP_OK) {
            err = nvs_commit(nvs_h);
            ESP_LOGI(TAG, "Committed NVS %s (flags=0x%02X, dyn_bass=%u%%)",
                     NVS_KEY_DSP_PRO_V1, (unsigned)blob.flags, (unsigned)blob.dynamic_bass_amount_pct);
        }
    }
    nvs_close(nvs_h);
    return err;
}

void load_user_profile_nvs(void)
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
        s_oled_view = (meta.oled_view < 5) ? meta.oled_view : 0;
        s_fir_enabled = (meta.fir_enabled != 0);
        s_auto_preamp_enabled = ((meta.reserved & 0x01) != 0);
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

    /* 3c. Load v0.5.0 DSP Pro configuration (dsp_pro_v1) */
    size_t req_pro = sizeof(peq_dsp_pro_nvs_v1_t);
    peq_dsp_pro_nvs_v1_t pro_blob;
    if (nvs_get_blob(nvs_h, NVS_KEY_DSP_PRO_V1, &pro_blob, &req_pro) == ESP_OK &&
        req_pro == sizeof(peq_dsp_pro_nvs_v1_t) &&
        pro_blob.magic == PEQ_DSP_PRO_MAGIC_V1 &&
        pro_blob.version == PEQ_DSP_PRO_VERSION_V1 &&
        calc_crc32((const uint8_t *)&pro_blob, offsetof(peq_dsp_pro_nvs_v1_t, crc32)) == pro_blob.crc32) {
        s_dsp_pro_cfg.tpdf_dither_enabled = (pro_blob.flags & 0x01U) != 0U;
        s_dsp_pro_cfg.subsonic_filter_enabled = (pro_blob.flags & 0x02U) != 0U;
        s_dsp_pro_cfg.vol_taper_log_enabled = (pro_blob.flags & 0x04U) != 0U;
        s_dsp_pro_cfg.dynamic_bass_enabled = (pro_blob.flags & 0x08U) != 0U;
        s_dsp_pro_cfg.dynamic_bass_amount_pct = (pro_blob.dynamic_bass_amount_pct <= 100U)
                                                ? pro_blob.dynamic_bass_amount_pct : 100U;
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

