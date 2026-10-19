/*
 * ESP32-S3 High-Fidelity Audio DSP/DAC System (Schematic 2: DSP+DAC+SCREEN)
 *
 * Subsystems:
 *   - YD-ESP32-S3 (N16R8, 16MB Flash, 8MB Octal PSRAM)
 *   - Texas Instruments GY-PCM5102A 32-bit DAC via Master I2S0:
 *       LCK (WS)   : GPIO 4
 *       DIN (DOUT) : GPIO 5
 *       BCK (BCLK) : GPIO 6
 *       XMST (Mute): GPIO 7 (Active-LOW soft mute)
 *   - Philips TDA1308 Class-AB Headphone Amplifier to 3.5mm Stereo Jack (J3)
 *   - SSD1306 128x64 Monochrome I2C OLED Display (0x3C):
 *       SDA        : GPIO 8
 *       SCL        : GPIO 9
 *   - EC11 Quadrature Rotary Encoder with Push Switch:
 *       Phase A    : GPIO 15 (Hardware PCNT unit with 1000ns glitch filter)
 *       Phase B    : GPIO 16 (Hardware PCNT unit with 1000ns glitch filter)
 *       Push SW    : GPIO 17 (Active-LOW, 10k hardware pull-up)
 *   - Tactile Mode Pushbutton:
 *       SW6 (Mode) : GPIO 18 (Active-LOW, 10k hardware pull-up)
 *   - Dynamic Parametric Equalizer (PEQ) DSP Engine (1 to 10 Bands, RBJ Audio Cookbook)
 *       Presets: Flat, Warm Bass Boost (+4.5dB), Harman OE2018 Target, User Custom
 *       15 ms click-free crossfade, hard float saturation clamping [-32768, 32767]
 *   - Driverless USB WebHID Live Tuner (TinyUSB OTG, 60 FPS, 0 flash wear)
 *   - 4-Phase Zero-Pop Audio Source Arbiter
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_system.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "driver/gpio.h"

#include "audio_pipeline.h"
#include "controls.h"
#include "oled_display.h"
#include "peq_dsp.h"
#include "audio_source_arbiter.h"
#include "webhid_peq.h"
#include "board_pins.h"

#define TAG "AUDIO_DAC_DSP"

static QueueHandle_t s_ctrl_queue = NULL;

/******************************************************************************
 * Core 1 Real-Time Audio Synth / Generator Task
 ******************************************************************************/

static void audio_dsp_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Audio DSP Task started on Core 1.");
    int16_t buffer[256 * 2]; // 256 stereo pairs (512 samples)
    const int buffer_pairs = 256;
    const int sample_rate = 48000;

    float phase = 0.0f;
    float current_freq = 440.0f;
    float sweep_freq = 100.0f;
    float sweep_dir = 1.0f;

    // Chord arpeggio frequencies for Tone Test (A minor arpeggio: A4, C5, E5, A5)
    static const float arpeggio_notes[] = {440.00f, 523.25f, 659.25f, 880.00f};
    int arp_idx = 0;
    int arp_counter = 0;

    while (1) {
        audio_source_t source = audio_source_arbiter_get_current(NULL);

        if (source == AUDIO_SOURCE_STANDALONE) {
            // Standalone / USB DAC Mode: Read stream from TinyUSB UAC 1.0 FIFO
            uint16_t avail = webhid_peq_audio_available();
            if (avail >= 192) {
                uint16_t to_read = avail > sizeof(buffer) ? sizeof(buffer) : avail;
                to_read = (to_read / 4) * 4; // Ensure stereo pair alignment (4 bytes per pair)
                if (to_read > 0) {
                    uint16_t n_read = webhid_peq_audio_read(buffer, to_read);
                    if (n_read > 0) {
                        int pairs = n_read / (2 * sizeof(int16_t));
                        // Process USB audio through active PEQ filters on Core 1
                        peq_dsp_process_interleaved_s16(buffer, pairs);
                        // Stream directly to Texas Instruments GY-PCM5102A DAC via I2S DMA
                        audio_pipeline_write(buffer, n_read);
                        continue;
                    }
                }
            }
            // At CONFIG_FREERTOS_HZ=100 (10 ms/tick), pdMS_TO_TICKS(2) truncates to 0 ticks,
            // which causes a tight busy-loop at Priority 10 and starves IDLE1. Use 10 ms (1 tick):
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        // Determine target frequency based on active source mode
        if (source == AUDIO_SOURCE_TONE_TEST) {
            // Cycle arpeggio every ~250ms (every ~47 buffers of 256 samples @ 48k)
            arp_counter++;
            if (arp_counter >= 47) {
                arp_counter = 0;
                arp_idx = (arp_idx + 1) % 4;
            }
            current_freq = arpeggio_notes[arp_idx];
        } else if (source == AUDIO_SOURCE_SWEEP) {
            // Smooth logarithmic/linear frequency sweep between 100 Hz and 4000 Hz
            sweep_freq += sweep_dir * 15.0f;
            if (sweep_freq >= 4000.0f) {
                sweep_freq = 4000.0f;
                sweep_dir = -1.0f;
            } else if (sweep_freq <= 100.0f) {
                sweep_freq = 100.0f;
                sweep_dir = 1.0f;
            }
            current_freq = sweep_freq;
        }

        const float phase_step = (2.0f * (float)M_PI * current_freq) / (float)sample_rate;

        // Synthesize stereo sine frame
        for (int i = 0; i < buffer_pairs; i++) {
            float amp = 10000.0f; // Safe level (~-10 dBFS)
            float val = sinf(phase) * amp;
            int16_t s16 = (int16_t)roundf(val);
            buffer[2 * i]     = s16; // Left
            buffer[2 * i + 1] = s16; // Right

            phase += phase_step;
            if (phase >= 2.0f * (float)M_PI) {
                phase -= 2.0f * (float)M_PI;
            }
        }

        // Process synthesized audio through active 5-band PEQ biquad filters on Core 1
        peq_dsp_process_interleaved_s16(buffer, buffer_pairs);

        // Feed filtered PCM data to I2S DMA with software master volume scaling
        audio_pipeline_write(buffer, buffer_pairs * 2 * sizeof(int16_t));
    }

    vTaskDelete(NULL);
}

/******************************************************************************
 * Core 0 User Controls & UI Dispatcher Task (Playback, Menu, Live EQ Adjust)
 ******************************************************************************/

static ui_mode_t s_ui_mode = UI_MODE_PLAYBACK;
static int s_menu_selection = 0;
static ui_eq_adjust_state_t s_eq_adjust_state;
static peq_profile_t s_revert_profile_snapshot;
static int s_saved_popup_ticks = 0;

static void update_playback_ui(void)
{
    char cur_pname[24] = "Flat";
    peq_dsp_get_current_preset(cur_pname);

    char cur_sname[20] = "DSP Standalone";
    audio_source_arbiter_get_current(cur_sname);

    bool is_muted = audio_pipeline_is_muted();

    ui_display_state_t uistate;
    uistate.volume = audio_pipeline_get_volume();
    uistate.is_muted = is_muted;
    uistate.is_playing = true;
    strncpy(uistate.mode_str, "USB DAC", sizeof(uistate.mode_str));
    strncpy(uistate.sample_rate_str, "48.0k 16b", sizeof(uistate.sample_rate_str));
    strncpy(uistate.title, "ESP32-S3 Audio DSP", sizeof(uistate.title));
    strncpy(uistate.subtitle, is_muted ? "[MUTED]" : cur_sname, sizeof(uistate.subtitle));
    strncpy(uistate.eq_preset, cur_pname, sizeof(uistate.eq_preset));
    oled_display_set_state(&uistate);
}

static void update_menu_ui(void)
{
    char cur_pname[24] = "Flat";
    peq_dsp_get_current_preset(cur_pname);

    char cur_sname[20] = "USB DAC";
    audio_source_arbiter_get_current(cur_sname);

    peq_profile_t prof;
    peq_dsp_get_profile(PEQ_PRESET_USER_CUSTOM, &prof);

    oled_display_update_menu(s_menu_selection, cur_pname, cur_sname, prof.preamp_gain_db);
}

static void enter_menu_mode(void)
{
    s_ui_mode = UI_MODE_MENU;
    s_menu_selection = 0;
    oled_display_set_ui_mode(UI_MODE_MENU);
    update_menu_ui();
    ESP_LOGI(TAG, "Entered Main Menu mode.");
}

static void exit_to_playback_mode(void)
{
    s_ui_mode = UI_MODE_PLAYBACK;
    oled_display_set_ui_mode(UI_MODE_PLAYBACK);
    update_playback_ui();
    ESP_LOGI(TAG, "Exited to Playback mode.");
}

static void enter_eq_adjust_mode(void)
{
    char pname[24];
    peq_preset_id_t active_id = peq_dsp_get_current_preset(pname);

    // If currently listening to a factory preset, copy it to User Custom as starting baseline
    if (active_id != PEQ_PRESET_USER_CUSTOM) {
        peq_profile_t active_prof;
        peq_dsp_get_profile(active_id, &active_prof);
        for (int i = 0; i < active_prof.num_bands; i++) {
            peq_dsp_update_user_band(i, &active_prof.bands[i]);
        }
        peq_dsp_set_band_count(active_prof.num_bands);
        peq_dsp_set_preamp(active_prof.preamp_gain_db);
        peq_dsp_select_preset(PEQ_PRESET_USER_CUSTOM);
    }

    // Save snapshot of profile for cancellation / revert
    peq_dsp_get_profile(PEQ_PRESET_USER_CUSTOM, &s_revert_profile_snapshot);

    peq_profile_t cur_prof;
    peq_dsp_get_profile(PEQ_PRESET_USER_CUSTOM, &cur_prof);

    s_eq_adjust_state.active_band = 0;
    s_eq_adjust_state.num_bands = cur_prof.num_bands;
    s_eq_adjust_state.cursor = EQ_CURSOR_FREQ_1K; // Default to 1kHz digit for instant tuning
    s_eq_adjust_state.band = cur_prof.bands[0];
    s_eq_adjust_state.saved_notification = false;
    s_saved_popup_ticks = 0;

    s_ui_mode = UI_MODE_EQ_ADJUST;
    oled_display_set_ui_mode(UI_MODE_EQ_ADJUST);
    oled_display_update_eq_adjust(&s_eq_adjust_state);
    ESP_LOGI(TAG, "Entered Live EQ Adjust mode (Bands: %d).", cur_prof.num_bands);
}

static void trigger_eq_saved_notification(void)
{
    peq_dsp_save_user_profile_nvs();
    peq_dsp_get_profile(PEQ_PRESET_USER_CUSTOM, &s_revert_profile_snapshot);
    s_eq_adjust_state.saved_notification = true;
    s_saved_popup_ticks = 15; // 1.5 seconds at 100ms poll
    oled_display_update_eq_adjust(&s_eq_adjust_state);
    ESP_LOGI(TAG, "PEQ User Custom Profile saved to NVS.");
}

static void cancel_and_revert_eq(void)
{
    ESP_LOGI(TAG, "Cancelling EQ adjustments and reverting to pre-edit snapshot...");
    for (int i = 0; i < s_revert_profile_snapshot.num_bands; i++) {
        peq_dsp_update_user_band(i, &s_revert_profile_snapshot.bands[i]);
    }
    peq_dsp_set_band_count(s_revert_profile_snapshot.num_bands);
    peq_dsp_set_preamp(s_revert_profile_snapshot.preamp_gain_db);

    exit_to_playback_mode();
}

static void eq_editor_add_band(void)
{
    peq_profile_t prof;
    peq_dsp_get_profile(PEQ_PRESET_USER_CUSTOM, &prof);
    if (prof.num_bands < PEQ_MAX_BANDS) {
        int new_idx = prof.num_bands;
        peq_band_param_t new_b = {
            .type = PEQ_FILTER_PEAKING,
            .freq_hz = 1000.0f,
            .gain_db = 0.0f,
            .q_factor = 1.0f,
            .enabled = true,
        };
        peq_dsp_update_user_band(new_idx, &new_b);
        peq_dsp_set_band_count(new_idx + 1);

        s_eq_adjust_state.active_band = new_idx;
        s_eq_adjust_state.num_bands = new_idx + 1;
        s_eq_adjust_state.band = new_b;
        s_eq_adjust_state.cursor = EQ_CURSOR_FREQ_1K;
        oled_display_update_eq_adjust(&s_eq_adjust_state);
        ESP_LOGI(TAG, "Added EQ Band %d (total: %d)", new_idx + 1, new_idx + 1);
    }
}

static void eq_editor_remove_band(void)
{
    peq_profile_t prof;
    peq_dsp_get_profile(PEQ_PRESET_USER_CUSTOM, &prof);
    if (prof.num_bands > 1) {
        int act = s_eq_adjust_state.active_band;
        for (int i = act; i < prof.num_bands - 1; i++) {
            prof.bands[i] = prof.bands[i + 1];
            peq_dsp_update_user_band(i, &prof.bands[i]);
        }
        int new_total = prof.num_bands - 1;
        peq_dsp_set_band_count(new_total);

        if (act >= new_total) {
            act = new_total - 1;
        }
        peq_dsp_get_profile(PEQ_PRESET_USER_CUSTOM, &prof);
        s_eq_adjust_state.active_band = act;
        s_eq_adjust_state.num_bands = new_total;
        s_eq_adjust_state.band = prof.bands[act];
        oled_display_update_eq_adjust(&s_eq_adjust_state);
        ESP_LOGI(TAG, "Removed EQ Band (new total: %d, active: %d)", new_total, act + 1);
    }
}

static void handle_playback_event(const control_event_t *evt)
{
    switch (evt->type) {
    case CONTROL_EVT_ROTARY_DELTA: {
        int delta_pct = evt->value * 2; // 2% volume per encoder detent
        int vol = audio_pipeline_adjust_volume(delta_pct);
        oled_display_show_volume_overlay(vol);
        break;
    }
    case CONTROL_EVT_ENC_CLICK: {
        bool is_muted = audio_pipeline_is_muted();
        audio_pipeline_set_mute(!is_muted);
        update_playback_ui();
        ESP_LOGI(TAG, "Playback: Encoder SW Click: %s", !is_muted ? "MUTED" : "UNMUTED");
        break;
    }
    case CONTROL_EVT_ENC_DOUBLE_CLICK: {
        ESP_LOGI(TAG, "Playback: Saving active user profile to NVS...");
        peq_dsp_save_user_profile_nvs();
        oled_display_show_eq_overlay("Saved to NVS");
        break;
    }
    case CONTROL_EVT_ENC_LONG_PRESS: {
        audio_source_t next_src = audio_source_arbiter_cycle();
        char sname[20];
        audio_source_arbiter_get_current(sname);
        oled_display_show_eq_overlay(sname);
        update_playback_ui();
        ESP_LOGI(TAG, "Playback: Cycled audio source to [%d]: %s", next_src, sname);
        break;
    }
    case CONTROL_EVT_BTN1_CLICK: {
        // Button 1 (GPIO 18 / SW6): Open Main Menu
        enter_menu_mode();
        break;
    }
    case CONTROL_EVT_BTN2_CLICK: {
        // Button 2 (GPIO 10): Cycle EQ preset forward
        peq_preset_id_t next = peq_dsp_cycle_preset();
        char pname[24];
        peq_dsp_get_current_preset(pname);
        oled_display_show_eq_overlay(pname);
        update_playback_ui();
        ESP_LOGI(TAG, "Playback: Cycled PEQ preset forward to [%d]: %s", next, pname);
        break;
    }
    case CONTROL_EVT_BTN1_LONG_PRESS: {
        // Button 1 Long Press: Cycle EQ preset backward
        char pname[24];
        peq_preset_id_t cur = peq_dsp_get_current_preset(pname);
        peq_preset_id_t prev = (cur + PEQ_PRESET_COUNT - 1) % PEQ_PRESET_COUNT;
        peq_dsp_select_preset(prev);
        peq_dsp_get_current_preset(pname);
        oled_display_show_eq_overlay(pname);
        update_playback_ui();
        ESP_LOGI(TAG, "Playback: Cycled PEQ preset backward to [%d]: %s", prev, pname);
        break;
    }
    default:
        break;
    }
}

static void handle_menu_event(const control_event_t *evt)
{
    switch (evt->type) {
    case CONTROL_EVT_ROTARY_DELTA: {
        s_menu_selection = (s_menu_selection + evt->value) % MENU_ITEM_COUNT;
        if (s_menu_selection < 0) s_menu_selection += MENU_ITEM_COUNT;
        update_menu_ui();
        break;
    }
    case CONTROL_EVT_BTN1_CLICK: {
        // Button 1 (◄): Exit Menu back to Playback mode
        exit_to_playback_mode();
        break;
    }
    case CONTROL_EVT_BTN2_CLICK: {
        // Button 2 (►): Navigate down menu list
        s_menu_selection = (s_menu_selection + 1) % MENU_ITEM_COUNT;
        update_menu_ui();
        break;
    }
    case CONTROL_EVT_ENC_CLICK: {
        // Rotary Push Switch: Confirm selected item
        switch (s_menu_selection) {
        case MENU_ITEM_EQ_ADJUST:
            enter_eq_adjust_mode();
            break;
        case MENU_ITEM_EQ_PRESET: {
            peq_dsp_cycle_preset();
            update_menu_ui();
            break;
        }
        case MENU_ITEM_AUDIO_SOURCE: {
            audio_source_arbiter_cycle();
            update_menu_ui();
            break;
        }
        case MENU_ITEM_PREAMP_GAIN: {
            peq_profile_t prof;
            peq_dsp_get_profile(PEQ_PRESET_USER_CUSTOM, &prof);
            float g = prof.preamp_gain_db - 2.0f;
            if (g < -6.5f) g = 0.0f;
            peq_dsp_set_preamp(g);
            update_menu_ui();
            break;
        }
        case MENU_ITEM_SAVE_NVS: {
            peq_dsp_save_user_profile_nvs();
            oled_display_show_eq_overlay("Saved to NVS");
            break;
        }
        case MENU_ITEM_EXIT:
            exit_to_playback_mode();
            break;
        default:
            break;
        }
        break;
    }
    case CONTROL_EVT_ENC_LONG_PRESS: {
        peq_dsp_save_user_profile_nvs();
        oled_display_show_eq_overlay("Saved to NVS");
        break;
    }
    default:
        break;
    }
}

static void handle_eq_adjust_event(const control_event_t *evt)
{
    switch (evt->type) {
    case CONTROL_EVT_BTN2_CLICK: {
        // Button 2 (►): Move cursor to the right
        s_eq_adjust_state.cursor = (eq_cursor_pos_t)((s_eq_adjust_state.cursor + 1) % EQ_CURSOR_COUNT);
        oled_display_update_eq_adjust(&s_eq_adjust_state);
        break;
    }
    case CONTROL_EVT_BTN1_CLICK: {
        // Button 1 (◄): Move cursor to the left
        s_eq_adjust_state.cursor = (eq_cursor_pos_t)((s_eq_adjust_state.cursor + EQ_CURSOR_COUNT - 1) % EQ_CURSOR_COUNT);
        oled_display_update_eq_adjust(&s_eq_adjust_state);
        break;
    }
    case CONTROL_EVT_BTN2_DOUBLE_CLICK: {
        // Shortcut: Add Band
        eq_editor_add_band();
        break;
    }
    case CONTROL_EVT_BTN1_DOUBLE_CLICK: {
        // Shortcut: Remove Band
        eq_editor_remove_band();
        break;
    }
    case CONTROL_EVT_ENC_LONG_PRESS: {
        // Instant Save to NVS
        trigger_eq_saved_notification();
        break;
    }
    case CONTROL_EVT_BTN1_LONG_PRESS: {
        // Instant Cancel & Revert
        cancel_and_revert_eq();
        break;
    }
    case CONTROL_EVT_ENC_CLICK: {
        // Switch / Select
        switch (s_eq_adjust_state.cursor) {
        case EQ_CURSOR_ACTION_ADD:
            eq_editor_add_band();
            break;
        case EQ_CURSOR_ACTION_DEL:
            eq_editor_remove_band();
            break;
        case EQ_CURSOR_ACTION_SAVE:
            trigger_eq_saved_notification();
            break;
        case EQ_CURSOR_ACTION_EXIT:
            exit_to_playback_mode();
            break;
        case EQ_CURSOR_BAND:
        case EQ_CURSOR_TYPE:
            s_eq_adjust_state.cursor = EQ_CURSOR_FREQ_1K;
            oled_display_update_eq_adjust(&s_eq_adjust_state);
            break;
        case EQ_CURSOR_FREQ_10K:
        case EQ_CURSOR_FREQ_1K:
        case EQ_CURSOR_FREQ_100:
        case EQ_CURSOR_FREQ_10:
        case EQ_CURSOR_FREQ_1:
            s_eq_adjust_state.cursor = EQ_CURSOR_GAIN_1;
            oled_display_update_eq_adjust(&s_eq_adjust_state);
            break;
        case EQ_CURSOR_GAIN_SIGN:
        case EQ_CURSOR_GAIN_10:
        case EQ_CURSOR_GAIN_1:
        case EQ_CURSOR_GAIN_DEC:
            s_eq_adjust_state.cursor = EQ_CURSOR_Q_DEC1;
            oled_display_update_eq_adjust(&s_eq_adjust_state);
            break;
        case EQ_CURSOR_Q_INT:
        case EQ_CURSOR_Q_DEC1:
        case EQ_CURSOR_Q_DEC2:
            s_eq_adjust_state.cursor = EQ_CURSOR_ACTION_SAVE;
            oled_display_update_eq_adjust(&s_eq_adjust_state);
            break;
        default:
            s_eq_adjust_state.cursor = EQ_CURSOR_BAND;
            oled_display_update_eq_adjust(&s_eq_adjust_state);
            break;
        }
        break;
    }
    case CONTROL_EVT_ROTARY_DELTA: {
        int delta = evt->value;
        switch (s_eq_adjust_state.cursor) {
        case EQ_CURSOR_BAND: {
            int new_band = s_eq_adjust_state.active_band + delta;
            if (new_band < 0) new_band = 0;
            if (new_band >= s_eq_adjust_state.num_bands) new_band = s_eq_adjust_state.num_bands - 1;
            if (new_band != s_eq_adjust_state.active_band) {
                // Save current band before switching
                peq_dsp_update_user_band(s_eq_adjust_state.active_band, &s_eq_adjust_state.band);
                peq_profile_t prof;
                peq_dsp_get_profile(PEQ_PRESET_USER_CUSTOM, &prof);
                s_eq_adjust_state.active_band = new_band;
                s_eq_adjust_state.band = prof.bands[new_band];
                oled_display_update_eq_adjust(&s_eq_adjust_state);
            }
            break;
        }
        case EQ_CURSOR_TYPE: {
            int t = (int)s_eq_adjust_state.band.type + delta;
            if (t < (int)PEQ_FILTER_PEAKING) t = (int)PEQ_FILTER_HIGH_SHELF;
            if (t > (int)PEQ_FILTER_HIGH_SHELF) t = (int)PEQ_FILTER_PEAKING;
            s_eq_adjust_state.band.type = (peq_filter_type_t)t;
            peq_dsp_update_user_band(s_eq_adjust_state.active_band, &s_eq_adjust_state.band);
            oled_display_update_eq_adjust(&s_eq_adjust_state);
            break;
        }
        case EQ_CURSOR_FREQ_10K:
        case EQ_CURSOR_FREQ_1K:
        case EQ_CURSOR_FREQ_100:
        case EQ_CURSOR_FREQ_10:
        case EQ_CURSOR_FREQ_1: {
            float step = 1.0f;
            if (s_eq_adjust_state.cursor == EQ_CURSOR_FREQ_10K) step = 10000.0f;
            else if (s_eq_adjust_state.cursor == EQ_CURSOR_FREQ_1K) step = 1000.0f;
            else if (s_eq_adjust_state.cursor == EQ_CURSOR_FREQ_100) step = 100.0f;
            else if (s_eq_adjust_state.cursor == EQ_CURSOR_FREQ_10) step = 10.0f;

            float f = s_eq_adjust_state.band.freq_hz + ((float)delta * step);
            if (f < 20.0f) f = 20.0f;
            if (f > 20000.0f) f = 20000.0f;
            s_eq_adjust_state.band.freq_hz = f;
            peq_dsp_update_user_band(s_eq_adjust_state.active_band, &s_eq_adjust_state.band);
            oled_display_update_eq_adjust(&s_eq_adjust_state);
            break;
        }
        case EQ_CURSOR_GAIN_SIGN: {
            s_eq_adjust_state.band.gain_db = -s_eq_adjust_state.band.gain_db;
            if (s_eq_adjust_state.band.gain_db < -24.0f) s_eq_adjust_state.band.gain_db = -24.0f;
            if (s_eq_adjust_state.band.gain_db > 12.0f) s_eq_adjust_state.band.gain_db = 12.0f;
            peq_dsp_update_user_band(s_eq_adjust_state.active_band, &s_eq_adjust_state.band);
            oled_display_update_eq_adjust(&s_eq_adjust_state);
            break;
        }
        case EQ_CURSOR_GAIN_10:
        case EQ_CURSOR_GAIN_1:
        case EQ_CURSOR_GAIN_DEC: {
            float step = 0.1f;
            if (s_eq_adjust_state.cursor == EQ_CURSOR_GAIN_10) step = 10.0f;
            else if (s_eq_adjust_state.cursor == EQ_CURSOR_GAIN_1) step = 1.0f;

            float g = s_eq_adjust_state.band.gain_db + ((float)delta * step);
            if (g < -24.0f) g = -24.0f;
            if (g > 12.0f) g = 12.0f;
            s_eq_adjust_state.band.gain_db = g;
            peq_dsp_update_user_band(s_eq_adjust_state.active_band, &s_eq_adjust_state.band);
            oled_display_update_eq_adjust(&s_eq_adjust_state);
            break;
        }
        case EQ_CURSOR_Q_INT:
        case EQ_CURSOR_Q_DEC1:
        case EQ_CURSOR_Q_DEC2: {
            float step = 0.01f;
            if (s_eq_adjust_state.cursor == EQ_CURSOR_Q_INT) step = 1.0f;
            else if (s_eq_adjust_state.cursor == EQ_CURSOR_Q_DEC1) step = 0.1f;

            float q = s_eq_adjust_state.band.q_factor + ((float)delta * step);
            if (q < 0.10f) q = 0.10f;
            if (q > 10.00f) q = 10.00f;
            s_eq_adjust_state.band.q_factor = q;
            peq_dsp_update_user_band(s_eq_adjust_state.active_band, &s_eq_adjust_state.band);
            oled_display_update_eq_adjust(&s_eq_adjust_state);
            break;
        }
        case EQ_CURSOR_ACTION_ADD:
        case EQ_CURSOR_ACTION_DEL:
        case EQ_CURSOR_ACTION_SAVE:
        case EQ_CURSOR_ACTION_EXIT: {
            int act = (int)s_eq_adjust_state.cursor + delta;
            if (act < (int)EQ_CURSOR_ACTION_ADD) act = (int)EQ_CURSOR_ACTION_EXIT;
            if (act > (int)EQ_CURSOR_ACTION_EXIT) act = (int)EQ_CURSOR_ACTION_ADD;
            s_eq_adjust_state.cursor = (eq_cursor_pos_t)act;
            oled_display_update_eq_adjust(&s_eq_adjust_state);
            break;
        }
        default:
            break;
        }
        break;
    }
    default:
        break;
    }
}

static void control_dispatcher_task(void *pvParameters)
{
    control_event_t evt;
    while (1) {
        if (xQueueReceive(s_ctrl_queue, &evt, pdMS_TO_TICKS(100)) == pdTRUE) {
            switch (s_ui_mode) {
            case UI_MODE_MENU:
                handle_menu_event(&evt);
                break;
            case UI_MODE_EQ_ADJUST:
                handle_eq_adjust_event(&evt);
                break;
            case UI_MODE_PLAYBACK:
            default:
                handle_playback_event(&evt);
                break;
            }
        }

        // Handle auto-clearing saved banner in EQ adjust mode
        if (s_ui_mode == UI_MODE_EQ_ADJUST && s_saved_popup_ticks > 0) {
            s_saved_popup_ticks--;
            if (s_saved_popup_ticks == 0 && s_eq_adjust_state.saved_notification) {
                s_eq_adjust_state.saved_notification = false;
                oled_display_update_eq_adjust(&s_eq_adjust_state);
            }
        }
    }
}

/******************************************************************************
 * Main Application Entry Point
 ******************************************************************************/

void app_main(void)
{
    ESP_LOGI(TAG, "=================================================");
    ESP_LOGI(TAG, "  ESP32-S3 Audio DSP/DAC System (Schematic 2)   ");
    ESP_LOGI(TAG, "  LCK (WS)   : GPIO %d                           ", BOARD_I2S_WS_PIN);
    ESP_LOGI(TAG, "  DIN (DOUT) : GPIO %d                           ", BOARD_I2S_DATA_OUT_PIN);
    ESP_LOGI(TAG, "  BCK        : GPIO %d                           ", BOARD_I2S_BCK_PIN);
    ESP_LOGI(TAG, "  MUTE (XMT) : GPIO %d                           ", BOARD_DAC_MUTE_PIN);
    ESP_LOGI(TAG, "  I2C SDA/SCL: GPIO %d / GPIO %d                 ", BOARD_I2C_SDA_PIN, BOARD_I2C_SCL_PIN);
    ESP_LOGI(TAG, "  ENC A / B  : GPIO %d / GPIO %d                 ", BOARD_ENC_A_PIN, BOARD_ENC_B_PIN);
    ESP_LOGI(TAG, "  ENC SW     : GPIO %d                           ", BOARD_ENC_SW_PIN);
    ESP_LOGI(TAG, "  BTN 1 (◄)  : GPIO %d (Menu / Back)             ", BOARD_BTN_1_PIN);
    ESP_LOGI(TAG, "  BTN 2 (►)  : GPIO %d (Next / Preset)           ", BOARD_BTN_2_PIN);
    ESP_LOGI(TAG, "  USB D-/D+  : GPIO %d / GPIO %d                 ", BOARD_USB_DM_PIN, BOARD_USB_DP_PIN);
    ESP_LOGI(TAG, "  WS2812B LED: GPIO %d (RMT Volume Loudness)     ", BOARD_RGB_LED_PIN);
    ESP_LOGI(TAG, "=================================================");

    // Initialize NVS (required for persistent settings and PEQ custom profiles)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize I2S peripheral and PCM5102A hardware soft mute on GPIO 7
    ESP_ERROR_CHECK(audio_pipeline_init(48000));

    // Initialize 5-band Parametric Equalizer (PEQ) DSP engine
    ESP_ERROR_CHECK(peq_dsp_init(48000));

    // Initialize Audio Source Arbiter (Default: Standalone DSP DAC mode)
    ESP_ERROR_CHECK(audio_source_arbiter_init(AUDIO_SOURCE_STANDALONE));

    // Play verification chime on startup (C5 - E5 - G5 - C6)
    audio_pipeline_play_startup_chime();

    // Initialize SSD1306 128x64 OLED Display (I2C on GPIO 8/9 @ 0x3C)
    ESP_ERROR_CHECK(oled_display_init());

    // Initialize EC11 Rotary Encoder (PCNT on GPIO 15/16) and Buttons (GPIO 17/18/10)
    ESP_ERROR_CHECK(controls_init(&s_ctrl_queue));
    xTaskCreatePinnedToCore(control_dispatcher_task, "ctrl_disp", 4096, NULL, 4, NULL, 0);

    // Initialize WebHID live PEQ tuning interface on Native USB OTG (GPIO 19/20) on Core 0
    ESP_ERROR_CHECK(webhid_peq_init());

    // Start Core 1 real-time audio generator & DSP task
    // Stack: 4096 bytes, Priority 10, Core 1
    xTaskCreatePinnedToCore(audio_dsp_task, "audio_dsp_task", 4096, NULL, 10, NULL, 1);

    ESP_LOGI(TAG, "ESP32-S3 High-Fidelity Audio DSP/DAC ready.");
}
