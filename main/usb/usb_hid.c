/*
 * ESP32-S3 WebHID Live Parametric EQ Protocol Router (64-byte 0x4551 framing)
 *
 * Handles GET_REPORT / SET_REPORT callbacks and outbound telemetry reports.
 * USB UAC 1.0 descriptors, IRAM audio ISR, and SPSC ring buffer live in
 * main/usb_uac_audio.c. Pinned strictly to Core 0 (Golden Rule 1).
 */

#include "usb_hid.h"

#include <stdio.h>
#include <string.h>
#include <math.h>

#include "esp_log.h"
#include "esp_err.h"
#include "tusb.h"
#include "class/hid/hid_device.h"

#include "dsp_peq.h"
#include "ui_oled.h"

#define TAG "WEBHID_PEQ"

static bool webhid_build_band_packet(uint8_t band_idx, peq_channel_t ch, webhid_peq_packet_t *pkt)
{
    if (!pkt) return false;

    peq_mixer_config_t mixer;
    if (peq_dsp_get_mixer(&mixer) != ESP_OK) {
        memset(&mixer, 0, sizeof(mixer));
    }

    if (band_idx == WEBHID_REPORT_SUB_MATRIX) {
        memset(pkt, 0, sizeof(*pkt));
        pkt->magic = WEBHID_MAGIC_16;
        pkt->cmd = WEBHID_CMD_SET_MIXER;
        pkt->band_idx = (uint8_t)mixer.mode;
        pkt->filter_type = (uint8_t)mixer.balance;
        pkt->enabled = mixer.width_pct;
        pkt->freq_hz = (uint16_t)mixer.crossfeed_level;
        memcpy(&pkt->reserved[0], &mixer.custom_matrix[0][0], sizeof(float) * 4);
        return true;
    }

    if (band_idx == WEBHID_REPORT_SUB_EXT_STATE) {
        peq_dynamics_cfg_t dyn;
        peq_dsp_get_dynamics_cfg(&dyn);
        char slot_name[PEQ_SLOT_NAME_MAX] = {0};
        peq_preset_id_t active = peq_dsp_get_current_preset(slot_name);
        uint32_t sr = webhid_peq_get_sample_rate();
        uint16_t fir_max = peq_dsp_get_fir_max_taps_for_sr((int)sr);

        uint8_t flags = (dyn.limiter_enabled ? 0x01 : 0x00) |
                        (((uint8_t)dyn.loudness_mode & 0x03) << 1) |
                        (dyn.fir_enabled ? 0x08 : 0x00) |
                        (((uint8_t)dyn.oled_view & 0x03) << 4) |
                        (dyn.auto_preamp_enabled ? 0x40 : 0x00);

        memset(pkt, 0, sizeof(*pkt));
        pkt->magic = WEBHID_MAGIC_16;
        pkt->cmd = WEBHID_CMD_SET_DYNAMICS;
        pkt->band_idx = WEBHID_REPORT_SUB_EXT_STATE;
        pkt->filter_type = (uint8_t)active;
        pkt->enabled = flags;
        pkt->freq_hz = dyn.auto_mute_sec;
        pkt->gain_db_x10 = (int16_t)dyn.oled_dim_sec;
        pkt->q_x100 = dyn.fir_taps_l;
        memcpy(&pkt->reserved[0], slot_name, 16);
        memcpy(&pkt->reserved[16], &dyn.fir_taps_r, sizeof(uint16_t));
        memcpy(&pkt->reserved[18], &fir_max, sizeof(uint16_t));

        peq_harmonics_cfg_t harm;
        peq_dsp_get_harmonics_cfg(&harm);
        uint8_t harm_flags = (harm.bass_enabled ? 0x01 : 0x00) |
                             (harm.exciter_enabled ? 0x02 : 0x00) |
                             (((uint8_t)harm.exciter_mode & 0x03) << 2);
        uint16_t bass_hz = harm.bass_freq_hz;
        uint16_t exc_hz = harm.exciter_freq_hz;
        pkt->reserved[20] = harm_flags;
        pkt->reserved[21] = harm.bass_amount_pct;
        pkt->reserved[22] = harm.exciter_amount_pct;
        memcpy(&pkt->reserved[23], &bass_hz, sizeof(uint16_t));
        memcpy(&pkt->reserved[25], &exc_hz, sizeof(uint16_t));
        int auto_att_x10 = (int)roundf(fabsf(dyn.auto_preamp_db) * 10.0f);
        if (auto_att_x10 < 0) auto_att_x10 = 0;
        if (auto_att_x10 > 120) auto_att_x10 = 120;
        pkt->reserved[27] = (uint8_t)auto_att_x10;

        /* v0.5.0 DSP Pro telemetry mirrored in reserved[28..31] (packet bytes 40..43) */
        peq_dsp_pro_cfg_t pro;
        peq_dsp_get_pro_cfg(&pro);
        uint8_t pro_flags = (pro.tpdf_dither_enabled ? 0x01U : 0U) |
                            (pro.subsonic_filter_enabled ? 0x02U : 0U) |
                            (pro.vol_taper_log_enabled ? 0x04U : 0U) |
                            (pro.dynamic_bass_enabled ? 0x08U : 0U) |
                            (dyn.auto_preamp_enabled ? 0x10U : 0U);
        pkt->reserved[28] = 1U; // DSP Pro telemetry schema version 1
        pkt->reserved[29] = pro_flags;
        pkt->reserved[30] = pro.dynamic_bass_amount_pct;
        pkt->reserved[31] = dyn.oled_view;
        return true;
    }

    if (band_idx >= PEQ_MAX_BANDS) {
        return false;
    }

    peq_profile_t profile;
    peq_preset_id_t active = peq_dsp_get_current_preset(NULL);
    if (peq_dsp_get_profile_ch(active, ch, &profile) != ESP_OK) {
        return false;
    }

    uint32_t sr = webhid_peq_get_sample_rate();
    int16_t preamp_x10 = (int16_t)roundf(profile.preamp_gain_db * 10.0f);
    uint8_t flags = (peq_dsp_is_linked_stereo() ? 0x01 : 0x00) |
                    (peq_dsp_get_ab_bypass() ? 0x02 : 0x00);

    memset(pkt, 0, sizeof(*pkt));
    pkt->magic = WEBHID_MAGIC_16;
    pkt->cmd = WEBHID_CMD_GET_ACTIVE_PROFILE;
    pkt->band_idx = band_idx;
    pkt->filter_type = (uint8_t)profile.bands[band_idx].type;
    pkt->enabled = profile.bands[band_idx].enabled ? 1 : 0;
    pkt->freq_hz = (uint16_t)roundf(profile.bands[band_idx].freq_hz);
    pkt->gain_db_x10 = (int16_t)roundf(profile.bands[band_idx].gain_db * 10.0f);
    pkt->q_x100 = (uint16_t)roundf(profile.bands[band_idx].q_factor * 100.0f);
    pkt->reserved[0] = profile.num_bands;
    pkt->reserved[1] = (uint8_t)ch;
    pkt->reserved[2] = flags;
    pkt->reserved[3] = (uint8_t)active;
    memcpy(&pkt->reserved[4], &preamp_x10, sizeof(preamp_x10));
    pkt->reserved[6] = (uint8_t)mixer.mode;
    pkt->reserved[7] = (uint8_t)mixer.balance;
    pkt->reserved[8] = mixer.width_pct;
    pkt->reserved[9] = mixer.crossfeed_level;
    memcpy(&pkt->reserved[10], &sr, sizeof(sr));
    return true;
}


// Invoked when host requests a report via GET_REPORT
uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t* buffer, uint16_t reqlen)
{
    (void)instance; (void)report_id; (void)report_type;
    if (reqlen < sizeof(webhid_peq_packet_t)) {
        return 0;
    }

    webhid_peq_packet_t pkt;
    if (!webhid_build_band_packet(0, PEQ_CH_LEFT, &pkt)) {
        return 0;
    }
    memcpy(buffer, &pkt, sizeof(pkt));
    return sizeof(pkt);
}

// Invoked when host sends a report via SET_REPORT (WebHID live tuning packets)
void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t const* buffer, uint16_t bufsize)
{
    (void)instance; (void)report_id; (void)report_type;

    if (!buffer || bufsize < 16) {
        return;
    }

    // Check Schema A: uint16 magic 0x4551 ("EQ")
    uint16_t magic_u16 = 0;
    memcpy(&magic_u16, &buffer[0], sizeof(uint16_t));

    if (magic_u16 == WEBHID_MAGIC_16 && bufsize >= sizeof(webhid_peq_packet_t)) {
        const webhid_peq_packet_t *pkt = (const webhid_peq_packet_t *)buffer;

        switch (pkt->cmd) {
        case WEBHID_CMD_GET_ACTIVE_PROFILE: {
            peq_channel_t ch = (pkt->reserved[1] <= PEQ_CH_RIGHT) ? (peq_channel_t)pkt->reserved[1] : PEQ_CH_LINKED;
            if (pkt->band_idx == WEBHID_REPORT_SUB_MATRIX || pkt->band_idx == WEBHID_REPORT_SUB_EXT_STATE) {
                webhid_peq_send_band_report_ch(pkt->band_idx, ch);
            } else {
                uint8_t b_idx = (pkt->band_idx < PEQ_MAX_BANDS) ? pkt->band_idx : 0;
                webhid_peq_send_band_report_ch(b_idx, ch);
            }
            break;
        }

        case WEBHID_CMD_SET_BAND_LIVE: {
            if (pkt->band_idx < PEQ_MAX_BANDS && pkt->filter_type < PEQ_FILTER_TYPE_COUNT) {
                peq_channel_t ch = (pkt->reserved[1] <= PEQ_CH_RIGHT) ? (peq_channel_t)pkt->reserved[1] : PEQ_CH_LINKED;
                peq_band_param_t band = {
                    .type = (peq_filter_type_t)pkt->filter_type,
                    .freq_hz = (float)pkt->freq_hz,
                    .gain_db = (float)pkt->gain_db_x10 / 10.0f,
                    .q_factor = (float)pkt->q_x100 / 100.0f,
                    .enabled = (pkt->enabled != 0)
                };

                peq_dsp_update_user_band_ch(ch, pkt->band_idx, &band);
                oled_display_invalidate_curve();
                oled_display_notify_activity();
            }
            break;
        }

        case WEBHID_CMD_SET_BAND_COUNT: {
            uint8_t count = pkt->band_idx;
            peq_channel_t ch = (pkt->reserved[1] <= PEQ_CH_RIGHT) ? (peq_channel_t)pkt->reserved[1] : PEQ_CH_LINKED;
            if (count >= 1 && count <= PEQ_MAX_BANDS) {
                peq_dsp_set_band_count_ch(ch, count);
                oled_display_invalidate_curve();
                char msg[24];
                snprintf(msg, sizeof(msg), "%d Bands Active", count);
                oled_display_show_eq_overlay(msg);
            }
            break;
        }

        case WEBHID_CMD_SET_PREAMP_LIVE: {
            float preamp_db = (float)pkt->gain_db_x10 / 10.0f;
            peq_dsp_set_preamp(preamp_db);
            oled_display_invalidate_curve();
            oled_display_notify_activity();
            break;
        }

        case WEBHID_CMD_SELECT_PROFILE: {
            peq_preset_id_t slot = (peq_preset_id_t)pkt->band_idx;
            if (slot < PEQ_USER_SLOT_COUNT) {
                peq_dsp_select_preset(slot);
                char name[24] = {0};
                peq_dsp_get_current_preset(name);
                oled_display_set_eq_preset(name);
                oled_display_show_eq_overlay(name);
                ESP_LOGI(TAG, "WebHID switched active slot to [%d]: %s", (int)slot, name);
            }
            break;
        }

        case WEBHID_CMD_SAVE_TO_PRESET: {
            peq_dsp_save_user_profile_nvs();
            oled_display_show_eq_overlay("Saved to NVS");
            ESP_LOGI(TAG, "WebHID explicit commit: Active slot & dynamics saved to NVS.");
            break;
        }

        case WEBHID_CMD_RESET_TO_PRESET: {
            int cur_slot = (int)peq_dsp_get_current_preset(NULL);
            peq_dsp_reset_slot_to_flat(cur_slot);
            char name[24] = {0};
            peq_dsp_get_current_preset(name);
            oled_display_set_eq_preset(name);
            oled_display_show_eq_overlay("Reset Slot Flat");
            break;
        }

        case WEBHID_CMD_SET_AB_BYPASS: {
            bool bypass = (pkt->band_idx != 0);
            peq_dsp_set_ab_bypass(bypass);
            oled_display_invalidate_curve();
            oled_display_show_eq_overlay(bypass ? "A/B: EQ Bypass" : "A/B: EQ Active");
            break;
        }

        case WEBHID_CMD_SET_DUAL_MONO: {
            bool linked = (pkt->band_idx != 0);
            peq_dsp_set_linked_stereo(linked);
            oled_display_invalidate_curve();
            oled_display_show_eq_overlay(linked ? "Stereo Linked" : "Dual-Mono L/R");
            break;
        }

        case WEBHID_CMD_RENAME_SLOT: {
            uint8_t slot_id = pkt->band_idx;
            if (slot_id < PEQ_USER_SLOT_COUNT) {
                char new_name[17];
                memcpy(new_name, &pkt->reserved[0], 16);
                new_name[16] = '\0';
                peq_dsp_rename_slot(slot_id, new_name);
                if ((int)slot_id == (int)peq_dsp_get_current_preset(NULL)) {
                    char cur_name[24] = {0};
                    peq_dsp_get_current_preset(cur_name);
                    oled_display_set_eq_preset(cur_name);
                    oled_display_show_eq_overlay(cur_name);
                }
            }
            break;
        }

        case WEBHID_CMD_SET_DYNAMICS: {
            bool lim_en = (pkt->enabled & 0x01) != 0;
            bool fir_en = (pkt->enabled & 0x02) != 0;
            bool auto_preamp_en = (pkt->enabled & 0x04) != 0;
            uint8_t loud = (pkt->filter_type < PEQ_LOUDNESS_COUNT) ? pkt->filter_type : 0;
            uint8_t view = (pkt->band_idx < OLED_VIEW_COUNT) ? pkt->band_idx : 0;

            peq_dsp_set_limiter_enabled(lim_en);
            peq_dsp_set_auto_preamp_enabled(auto_preamp_en);
            peq_dsp_set_loudness_mode((peq_loudness_mode_t)loud);
            peq_dsp_set_fir_enabled(fir_en);
            peq_dsp_set_oled_view(view);
            oled_display_set_playback_view((oled_playback_view_t)view);
            oled_display_invalidate_curve();
            oled_display_show_eq_overlay("Dynamics Updated");
            webhid_peq_send_band_report(WEBHID_REPORT_SUB_EXT_STATE);
            break;
        }

        case WEBHID_CMD_SET_POWER_CFG: {
            uint16_t auto_mute = pkt->freq_hz;
            uint16_t oled_dim  = pkt->q_x100;
            peq_dsp_set_auto_mute_sec(auto_mute);
            peq_dsp_set_oled_dim_sec(oled_dim);
            oled_display_set_dim_sec(oled_dim);
            oled_display_show_eq_overlay("Power Cfg Set");
            break;
        }

        case WEBHID_CMD_FACTORY_RESET_SLOT: {
            uint8_t slot_id = pkt->band_idx;
            if (slot_id < PEQ_USER_SLOT_COUNT) {
                peq_dsp_reset_slot_to_flat(slot_id);
                if ((int)slot_id == (int)peq_dsp_get_current_preset(NULL)) {
                    char cur_name[24] = {0};
                    peq_dsp_get_current_preset(cur_name);
                    oled_display_set_eq_preset(cur_name);
                    oled_display_show_eq_overlay("Slot Reset");
                }
            }
            break;
        }

        case WEBHID_CMD_SET_HARMONICS: {
            uint8_t flags = pkt->band_idx;
            uint8_t bass_amt = (pkt->filter_type <= 100) ? pkt->filter_type : 100;
            uint8_t exc_amt = (pkt->enabled <= 100) ? pkt->enabled : 100;
            uint16_t bass_freq = pkt->freq_hz;
            uint16_t exc_freq = pkt->q_x100;

            if (bass_freq < 50) bass_freq = 50;
            if (bass_freq > 150) bass_freq = 150;
            if (exc_freq < 1500) exc_freq = 1500;
            if (exc_freq > 8000) exc_freq = 8000;

            uint8_t raw_mode = (flags >> 2) & 0x03;
            if (raw_mode >= (uint8_t)PEQ_EXCITER_MODE_COUNT) {
                raw_mode = (uint8_t)(PEQ_EXCITER_MODE_COUNT - 1);
            }

            peq_harmonics_cfg_t cfg = {
                .bass_enabled = (flags & 0x01) != 0,
                .bass_amount_pct = bass_amt,
                .bass_freq_hz = bass_freq,
                .exciter_enabled = (flags & 0x02) != 0,
                .exciter_mode = (peq_exciter_mode_t)raw_mode,
                .exciter_amount_pct = exc_amt,
                .exciter_freq_hz = exc_freq,
            };
            peq_dsp_set_harmonics_cfg(&cfg);
            oled_display_notify_activity();
            webhid_peq_send_band_report(WEBHID_REPORT_SUB_EXT_STATE);
            break;
        }

        case WEBHID_CMD_SET_MIXER: {
            peq_mixer_config_t cfg;
            memset(&cfg, 0, sizeof(cfg));
            cfg.mode = (pkt->band_idx < MIXER_MODE_COUNT) ? (mixer_mode_t)pkt->band_idx : MIXER_MODE_STEREO;
            cfg.balance = (int8_t)pkt->filter_type;
            cfg.width_pct = (pkt->enabled <= 200) ? pkt->enabled : 200;
            cfg.crossfeed_level = (uint8_t)(pkt->freq_hz <= 3 ? pkt->freq_hz : 3);
            memcpy(&cfg.custom_matrix[0][0], &pkt->reserved[0], sizeof(float) * 4);
            peq_dsp_set_mixer(&cfg);
            oled_display_show_eq_overlay("Mixer Updated");
            break;
        }

        case WEBHID_CMD_UPLOAD_FIR_CHUNK: {
            peq_channel_t ch = (pkt->band_idx <= PEQ_CH_RIGHT) ? (peq_channel_t)pkt->band_idx : PEQ_CH_LINKED;
            uint8_t count = (pkt->filter_type <= 12) ? pkt->filter_type : 0;
            bool commit = (pkt->enabled & 0x01) != 0;
            uint16_t total_taps = pkt->freq_hz;
            uint16_t offset = pkt->q_x100;
            float taps_buf[12] = {0};
            if (count > 0) {
                memcpy(taps_buf, &pkt->reserved[0], sizeof(float) * count);
            }
            esp_err_t ferr = peq_dsp_upload_fir_chunk(ch, total_taps, offset, count, taps_buf, commit);
            if (ferr == ESP_OK && (total_taps == 0 || (uint32_t)offset + count >= total_taps)) {
                oled_display_show_eq_overlay(total_taps == 0 ? "FIR Cleared" : "FIR Loaded");
            }
            break;
        }

        case WEBHID_CMD_SET_DSP_PRO: {
            uint8_t flags = pkt->band_idx;
            peq_dsp_pro_cfg_t pro;
            peq_dsp_get_pro_cfg(&pro);
            pro.tpdf_dither_enabled = (flags & 0x01U) != 0U;
            pro.subsonic_filter_enabled = (flags & 0x02U) != 0U;
            pro.vol_taper_log_enabled = (flags & 0x04U) != 0U;
            pro.dynamic_bass_enabled = (flags & 0x08U) != 0U;
            pro.dynamic_bass_amount_pct = (pkt->filter_type <= 100U) ? pkt->filter_type : 100U;
            peq_dsp_set_pro_cfg(&pro);
            peq_dsp_set_auto_preamp_enabled((flags & 0x10U) != 0U);
            if (pkt->enabled != 0xFFU && pkt->enabled < OLED_VIEW_COUNT) {
                peq_dsp_set_oled_view(pkt->enabled);
                oled_display_set_playback_view((oled_playback_view_t)pkt->enabled);
            }
            oled_display_invalidate_curve();
            webhid_peq_notify_state_changed();
            break;
        }

        default:
            ESP_LOGW(TAG, "Unknown WebHID Schema A command: 0x%02X", pkt->cmd);
            break;
        }
        return;
    }


    // Check Schema B: byte 0 magic 'E' (0x45) with float parameters (04_usb_audio_webhid.md)
    if (buffer[0] == WEBHID_MAGIC_BYTE) {
        uint8_t cmd = buffer[1];
        uint8_t band_idx = buffer[2];
        uint8_t filter_type = buffer[3];

        float freq = 1000.0f, gain = 0.0f, q = 0.707f;
        memcpy(&freq, &buffer[4], sizeof(float));
        memcpy(&gain, &buffer[8], sizeof(float));
        memcpy(&q,    &buffer[12], sizeof(float));

        if (cmd == 0x01 && band_idx < PEQ_MAX_BANDS) {
            peq_band_param_t band = {
                .type = (peq_filter_type_t)filter_type,
                .freq_hz = freq,
                .gain_db = gain,
                .q_factor = q,
                .enabled = true
            };
            peq_dsp_update_user_band(band_idx, &band);
            oled_display_invalidate_curve();
        } else if (cmd == 0x02) {
            peq_dsp_save_user_profile_nvs();
            oled_display_show_eq_overlay("Saved to NVS");
        }
    }
}

// =============================================================================
// Public API
// =============================================================================


esp_err_t webhid_peq_send_band_report_ch(uint8_t band_idx, peq_channel_t ch)
{
    if (band_idx != WEBHID_REPORT_SUB_MATRIX &&
        band_idx != WEBHID_REPORT_SUB_EXT_STATE &&
        band_idx >= PEQ_MAX_BANDS) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!tud_hid_n_ready(0)) return ESP_ERR_NOT_FOUND;

    webhid_peq_packet_t pkt;
    if (!webhid_build_band_packet(band_idx, ch, &pkt)) {
        return ESP_FAIL;
    }

    bool res = tud_hid_n_report(0, 0, &pkt, sizeof(pkt));
    return res ? ESP_OK : ESP_FAIL;
}

esp_err_t webhid_peq_send_band_report(uint8_t band_idx)
{
    return webhid_peq_send_band_report_ch(band_idx, PEQ_CH_LEFT);
}

esp_err_t webhid_peq_notify_state_changed(void)
{
    return webhid_peq_send_band_report(WEBHID_REPORT_SUB_EXT_STATE);
}

