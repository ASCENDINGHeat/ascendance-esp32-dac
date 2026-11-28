#include "oled_display.h"
#include "ssd1306_drv.h"
#include "board_pins.h"
#include "peq_dsp.h"
#include "webhid_peq.h"

#include <stdio.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_dsp.h"

#define TAG "OLED"
#define OLED_BLANK_SEC 180U
#define OLED_SPEC_BANDS 32

/* Static BSS buffers for Core 0 1024-pt FFT 32-band RTA (off task stack) */
static float s_fft_in[PEQ_FFT_SIZE];
static __attribute__((aligned(16))) float s_fft_cf[PEQ_FFT_SIZE * 2];
static float s_fft_win[PEQ_FFT_SIZE];
static float s_spec_db[OLED_SPEC_BANDS];
static float s_spec_peak_db[OLED_SPEC_BANDS];
static uint8_t s_spec_peak_hold[OLED_SPEC_BANDS];
static bool s_fft_inited = false;

static SemaphoreHandle_t s_ui_mutex = NULL;
static ui_display_state_t s_current_state;

static ui_mode_t s_current_ui_mode = UI_MODE_PLAYBACK;
static ui_menu_state_t s_current_menu_state;
static ui_eq_adjust_state_t s_current_eq_state;
static oled_playback_view_t s_playback_view = OLED_VIEW_HUD;

/* Temporary overlay counters (in 100ms UI ticks) */
static int s_overlay_vol_timeout = 0;
static int s_overlay_vol_val = 0;
static int s_overlay_eq_timeout = 0;
static char s_overlay_eq_name[24] = {0};

/* Cached PEQ curve response in dB across 128 screen pixels */
static float s_curve_db[OLED_WIDTH];
static bool  s_curve_dirty = true;

/* VU Peak-Hold state (in dBFS) */
static float s_vu_hold_db_l = -60.0f;
static float s_vu_hold_db_r = -60.0f;
static int   s_vu_hold_ticks_l = 0;
static int   s_vu_hold_ticks_r = 0;

/* Burn-in / Activity / Power state */
static TickType_t s_last_activity_tick = 0;
static uint16_t   s_dim_sec = 60;
static bool       s_is_dimmed = false;
static bool       s_is_blanked = false;
static bool       s_usb_suspended = false;
static int        s_probe_tick_cnt = 0;
static int        s_frame_tick_cnt = 0;

static void refresh_curve_cache_if_dirty(void)
{
    if (s_curve_dirty) {
        peq_dsp_eval_curve_db(s_curve_db, OLED_WIDTH, 20.0f, 20000.0f);
        s_curve_dirty = false;
    }
}

static int db_to_curve_y(float db)
{
    // Body area y=12..48 (0 dB centerline at y=30, +/-12 dB -> 1.4 px/dB)
    if (db > 12.5f) db = 12.5f;
    if (db < -12.5f) db = -12.5f;
    int y = 30 - (int)lroundf(db * 1.4f);
    if (y < 12) y = 12;
    if (y > 48) y = 48;
    return y;
}

static void draw_curve_plot(int x0, int width)
{
    refresh_curve_cache_if_dirty();

    // Dotted 0 dB centerline
    for (int x = 0; x < width; x++) {
        if ((x & 3) == 0) {
            oled_draw_pixel(x0 + x, 30, true);
        }
    }

    int prev_y = -1;
    for (int x = 0; x < width; x++) {
        int idx = (width > 1) ? ((x * (OLED_WIDTH - 1)) / (width - 1)) : 0;
        int cy = db_to_curve_y(s_curve_db[idx]);
        if (prev_y < 0) prev_y = cy;
        int y_lo = (prev_y < cy) ? prev_y : cy;
        int y_hi = (prev_y > cy) ? prev_y : cy;
        for (int yy = y_lo; yy <= y_hi; yy++) {
            oled_draw_pixel(x0 + x, yy, true);
        }
        prev_y = cy;
    }
}

static void update_vu_hold(const peq_vu_metrics_t *vu)
{
    if (vu->peak_db_l >= s_vu_hold_db_l) {
        s_vu_hold_db_l = vu->peak_db_l;
        s_vu_hold_ticks_l = 3; // 300 ms hold at 10 FPS
    } else if (s_vu_hold_ticks_l > 0) {
        s_vu_hold_ticks_l--;
    } else {
        s_vu_hold_db_l -= 2.2f;
        if (s_vu_hold_db_l < -60.0f) s_vu_hold_db_l = -60.0f;
    }

    if (vu->peak_db_r >= s_vu_hold_db_r) {
        s_vu_hold_db_r = vu->peak_db_r;
        s_vu_hold_ticks_r = 3;
    } else if (s_vu_hold_ticks_r > 0) {
        s_vu_hold_ticks_r--;
    } else {
        s_vu_hold_db_r -= 2.2f;
        if (s_vu_hold_db_r < -60.0f) s_vu_hold_db_r = -60.0f;
    }
}

static int db_to_bar_px(float db, int max_px)
{
    if (db <= -60.0f) return 0;
    if (db >= 0.0f) return max_px;
    int px = (int)lroundf(((db + 60.0f) / 60.0f) * (float)max_px);
    if (px < 0) px = 0;
    if (px > max_px) px = max_px;
    return px;
}

static void render_vu_body(void)
{
    peq_vu_metrics_t vu;
    peq_dsp_get_vu_metrics(&vu, false);
    update_vu_hold(&vu);

    const int bar_x = 12;
    const int bar_w = 88; // x=12..99

    oled_draw_string(2, 15, "L", false);
    oled_draw_rect(bar_x, 15, bar_w, 8, false, true);
    int rms_w_l = db_to_bar_px(vu.rms_db_l, bar_w - 4);
    int pk_w_l  = db_to_bar_px(vu.peak_db_l, bar_w - 4);
    int hd_x_l  = db_to_bar_px(s_vu_hold_db_l, bar_w - 5);
    if (rms_w_l > 0) oled_draw_rect(bar_x + 2, 17, rms_w_l, 4, true, true);
    if (pk_w_l > rms_w_l) oled_draw_hline(bar_x + 2 + rms_w_l, 19, pk_w_l - rms_w_l, true);
    if (hd_x_l > 0) oled_draw_vline(bar_x + 2 + hd_x_l, 16, 6, true);

    oled_draw_string(2, 26, "R", false);
    oled_draw_rect(bar_x, 26, bar_w, 8, false, true);
    int rms_w_r = db_to_bar_px(vu.rms_db_r, bar_w - 4);
    int pk_w_r  = db_to_bar_px(vu.peak_db_r, bar_w - 4);
    int hd_x_r  = db_to_bar_px(s_vu_hold_db_r, bar_w - 5);
    if (rms_w_r > 0) oled_draw_rect(bar_x + 2, 28, rms_w_r, 4, true, true);
    if (pk_w_r > rms_w_r) oled_draw_hline(bar_x + 2 + rms_w_r, 30, pk_w_r - rms_w_r, true);
    if (hd_x_r > 0) oled_draw_vline(bar_x + 2 + hd_x_r, 27, 6, true);

    if (vu.clip_latched) {
        oled_draw_string(103, 15, "CLIP", true);
    } else if (vu.limiter_active) {
        oled_draw_string(106, 15, "LIM", true);
    } else {
        oled_draw_string(106, 15, "0dB", false);
    }
    oled_draw_string(104, 26, peq_dsp_is_limiter_enabled() ? "L:ON" : "L:--", false);

    // Scale legend & Peak readout
    char db_str[28];
    if (peq_dsp_is_signal_silent()) {
        snprintf(db_str, sizeof(db_str), "SILENCE AUTO-MUTE");
    } else {
        float max_pk = (vu.peak_db_l > vu.peak_db_r) ? vu.peak_db_l : vu.peak_db_r;
        if (max_pk < -90.0f) {
            snprintf(db_str, sizeof(db_str), "-60  -36  -18  -6  0");
        } else {
            snprintf(db_str, sizeof(db_str), "PK:%5.1fdB  RMS:%5.1f", max_pk,
                     0.5f * (vu.rms_db_l + vu.rms_db_r));
        }
    }
    oled_draw_string(2, 39, db_str, false);
}

static void render_split_body(void)
{
    peq_vu_metrics_t vu;
    peq_dsp_get_vu_metrics(&vu, false);
    update_vu_hold(&vu);

    // Left 94px: Frequency Response Curve
    draw_curve_plot(0, 94);
    oled_draw_vline(95, 12, 37, true);

    // Right 30px (x=98..127): Vertical L & R VU Bars
    const int vh = 26;
    oled_draw_rect(99, 13, 11, vh + 2, false, true);
    oled_draw_rect(114, 13, 11, vh + 2, false, true);

    int rms_hl = db_to_bar_px(vu.rms_db_l, vh - 2);
    int pk_hl  = db_to_bar_px(s_vu_hold_db_l, vh - 2);
    if (rms_hl > 0) {
        oled_draw_rect(101, 13 + vh - rms_hl, 7, rms_hl, true, true);
    }
    if (pk_hl > 0) {
        oled_draw_hline(101, 13 + vh - pk_hl, 7, true);
    }

    int rms_hr = db_to_bar_px(vu.rms_db_r, vh - 2);
    int pk_hr  = db_to_bar_px(s_vu_hold_db_r, vh - 2);
    if (rms_hr > 0) {
        oled_draw_rect(116, 13 + vh - rms_hr, 7, rms_hr, true, true);
    }
    if (pk_hr > 0) {
        oled_draw_hline(116, 13 + vh - pk_hr, 7, true);
    }

    oled_draw_string(102, 42, "L", vu.clip_latched);
    oled_draw_string(117, 42, "R", vu.limiter_active);
}

static void render_spectrum_body(void)
{
    if (!s_fft_inited) {
        dsps_fft2r_init_fc32(NULL, PEQ_FFT_SIZE);
        dsps_wind_hann_f32(s_fft_win, PEQ_FFT_SIZE);
        for (int b = 0; b < OLED_SPEC_BANDS; b++) {
            s_spec_db[b] = -60.0f;
            s_spec_peak_db[b] = -60.0f;
            s_spec_peak_hold[b] = 0;
        }
        s_fft_inited = true;
    }

    int sr = 48000;
    if (peq_dsp_read_fft_snapshot(s_fft_in, &sr)) {
        if (sr <= 0) sr = 48000;
        for (int i = 0; i < PEQ_FFT_SIZE; i++) {
            s_fft_cf[2 * i]     = s_fft_in[i] * s_fft_win[i];
            s_fft_cf[2 * i + 1] = 0.0f;
        }
        dsps_fft2r_fc32(s_fft_cf, PEQ_FFT_SIZE);
        dsps_bit_rev_fc32(s_fft_cf, PEQ_FFT_SIZE);

        /* Group bins 1..511 into 32 log-spaced bands (47 Hz .. 18 kHz) */
        const float f_min = 47.0f;
        const float f_max = fminf(18000.0f, 0.45f * (float)sr);
        const float ratio = powf(f_max / f_min, 1.0f / (float)OLED_SPEC_BANDS);
        const float norm_scale = 4.0f / (float)PEQ_FFT_SIZE; // Hann coherent gain = 0.5, single-sided = 2x
        int prev_hi = 1;

        for (int b = 0; b < OLED_SPEC_BANDS; b++) {
            float f_lo = f_min * powf(ratio, (float)b);
            float f_hi = f_lo * ratio;
            int b0 = (int)lroundf(f_lo * (float)PEQ_FFT_SIZE / (float)sr);
            int b1 = (int)lroundf(f_hi * (float)PEQ_FFT_SIZE / (float)sr);
            if (b0 < prev_hi) b0 = prev_hi;
            if (b0 < 1) b0 = 1;
            if (b1 <= b0) b1 = b0 + 1;
            if (b1 > (PEQ_FFT_SIZE / 2)) b1 = PEQ_FFT_SIZE / 2;
            prev_hi = b1;

            float max_mag2 = 0.0f;
            for (int k = b0; k < b1; k++) {
                float re = s_fft_cf[2 * k];
                float im = s_fft_cf[2 * k + 1];
                float m2 = re * re + im * im;
                if (m2 > max_mag2) max_mag2 = m2;
            }

            float mag = sqrtf(max_mag2) * norm_scale;
            float db = (mag > 1e-4f) ? (20.0f * log10f(mag)) : -60.0f;
            if (db < -60.0f) db = -60.0f;
            if (db > 0.0f) db = 0.0f;

            /* Fast attack, smooth decay */
            if (db > s_spec_db[b]) {
                s_spec_db[b] = 0.70f * db + 0.30f * s_spec_db[b];
            } else {
                s_spec_db[b] = 0.25f * db + 0.75f * s_spec_db[b];
            }

            /* Peak-hold dot with 6-tick (~600 ms) hold */
            if (s_spec_db[b] >= s_spec_peak_db[b]) {
                s_spec_peak_db[b] = s_spec_db[b];
                s_spec_peak_hold[b] = 6;
            } else if (s_spec_peak_hold[b] > 0) {
                s_spec_peak_hold[b]--;
            } else {
                s_spec_peak_db[b] -= 2.5f;
                if (s_spec_peak_db[b] < -60.0f) s_spec_peak_db[b] = -60.0f;
            }
        }
    } else {
        /* Gentle idle decay when no new audio frames arrive */
        for (int b = 0; b < OLED_SPEC_BANDS; b++) {
            s_spec_db[b] -= 3.0f;
            if (s_spec_db[b] < -60.0f) s_spec_db[b] = -60.0f;
            s_spec_peak_db[b] -= 2.0f;
            if (s_spec_peak_db[b] < -60.0f) s_spec_peak_db[b] = -60.0f;
        }
    }

    /* Draw 32 bars across 128 px (3 px bar + 1 px gap), y = 12..48 (36 px height) */
    const int base_y = 48;
    const int max_h = 36;
    for (int b = 0; b < OLED_SPEC_BANDS; b++) {
        int x = b * 4;
        int h = db_to_bar_px(s_spec_db[b], max_h);
        if (h > 0) {
            oled_draw_rect(x, base_y - h, 3, h, true, true);
        }
        int ph = db_to_bar_px(s_spec_peak_db[b], max_h);
        if (ph > 0) {
            oled_draw_hline(x, base_y - ph, 3, true);
        }
    }
}

static void render_playback_screen(void)
{
    xSemaphoreTake(s_ui_mutex, portMAX_DELAY);
    ui_display_state_t state = s_current_state;
    oled_playback_view_t view = s_playback_view;
    int vol_timeout = s_overlay_vol_timeout;
    int vol_val = s_overlay_vol_val;
    int eq_timeout = s_overlay_eq_timeout;
    char eq_name[24];
    snprintf(eq_name, sizeof(eq_name), "%.23s", s_overlay_eq_name);
    if (s_overlay_vol_timeout > 0) s_overlay_vol_timeout--;
    if (s_overlay_eq_timeout > 0) s_overlay_eq_timeout--;
    xSemaphoreGive(s_ui_mutex);

    oled_clear();

    // 1. Header (y: 0..9)
    char header_left[32];
    snprintf(header_left, sizeof(header_left), "[%s] %s",
             state.mode_str[0] ? state.mode_str : "USB DAC",
             state.sample_rate_str[0] ? state.sample_rate_str : "48.0k");
    oled_draw_string(0, 0, header_left, false);

    char header_right[16];
    if (state.is_muted) {
        snprintf(header_right, sizeof(header_right), "MUTED");
    } else {
        snprintf(header_right, sizeof(header_right), "V:%d%%", state.volume);
    }
    int right_x = OLED_WIDTH - (int)strlen(header_right) * 6;
    oled_draw_string(right_x, 0, header_right, state.is_muted);

    oled_draw_hline(0, 10, OLED_WIDTH, true);

    // 2. Main Content Body (y: 12..48)
    if (vol_timeout > 0) {
        oled_draw_rect(10, 16, 108, 30, true, false);
        oled_draw_rect(10, 16, 108, 30, false, true);
        char v_str[16];
        snprintf(v_str, sizeof(v_str), "VOLUME: %d%%", vol_val);
        oled_draw_string(24, 20, v_str, false);
        oled_draw_progress_bar(16, 32, 96, 9, vol_val);
    } else if (eq_timeout > 0) {
        oled_draw_rect(10, 16, 108, 30, true, false);
        oled_draw_rect(10, 16, 108, 30, false, true);
        oled_draw_string(19, 20, "<< EQ SLOT >>", false);
        int name_len = (int)strlen(eq_name);
        if (name_len > 17) {
            eq_name[17] = '\0';
            name_len = 17;
        }
        int eq_x = 10 + (108 - name_len * 6) / 2;
        oled_draw_string(eq_x, 32, eq_name, false);
    } else {
        switch (view) {
        case OLED_VIEW_CURVE:
            draw_curve_plot(0, OLED_WIDTH);
            break;
        case OLED_VIEW_VU:
            render_vu_body();
            break;
        case OLED_VIEW_SPLIT:
            render_split_body();
            break;
        case OLED_VIEW_SPECTRUM:
            render_spectrum_body();
            break;
        case OLED_VIEW_HUD:
        default:
            oled_draw_string(2, 16, state.title[0] ? state.title : "ASCENDANCE Hi-Fi", false);
            oled_draw_string(2, 28, state.subtitle[0] ? state.subtitle : "Active", false);
            oled_draw_progress_bar(2, 40, 124, 6, state.volume);
            break;
        }
    }

    // 3. Footer (y: 52..63)
    oled_draw_hline(0, 50, OLED_WIDTH, true);
    char footer_eq[24];
    snprintf(footer_eq, sizeof(footer_eq), "%.14s", state.eq_preset[0] ? state.eq_preset : "Slot 1");
    oled_draw_string(0, 54, footer_eq, false);

    const char *status_str = state.is_muted ? "MUTE" : (state.is_playing ? "PLAY" : "IDLE");
    int status_x = OLED_WIDTH - (int)strlen(status_str) * 6;
    oled_draw_string(status_x, 54, status_str, state.is_muted);

    oled_flush();
}

static void format_menu_item_label(int item_idx, const ui_menu_state_t *m, char *buf, size_t sz)
{
    static const char *const k_xf_names[4]   = { "Off", "CM", "JM", "JM+" };
    static const char *const k_loud_names[3] = { "Off", "Mild", "Full" };
    static const char *const k_view_names[5] = { "Classic", "Curve", "VU", "Split", "Spectrum" };
    static const char *const k_exc_names[PEQ_EXCITER_MODE_COUNT] = { "Tube", "Tape", "Air" };

    int num = item_idx + 1;
    switch (item_idx) {
    case MENU_ITEM_EQ_ADJUST:
        snprintf(buf, sz, "%d. Live EQ Adjust", num);
        break;
    case MENU_ITEM_EQ_PRESET:
        snprintf(buf, sz, "%d. Slot:%.12s", num,
                 m->current_preset[0] ? m->current_preset : "Slot 1");
        break;
    case MENU_ITEM_AB_BYPASS:
        snprintf(buf, sz, "%d. A/B:%s", num, m->ab_bypass ? "Bypass" : "Active");
        break;
    case MENU_ITEM_CROSSFEED:
        snprintf(buf, sz, "%d. Crossfeed:%s", num,
                 k_xf_names[(m->crossfeed_level < 4) ? m->crossfeed_level : 0]);
        break;
    case MENU_ITEM_LOUDNESS:
        snprintf(buf, sz, "%d. Loudness:%s", num,
                 k_loud_names[(m->loudness_mode < 3) ? m->loudness_mode : 0]);
        break;
    case MENU_ITEM_LIMITER:
        snprintf(buf, sz, "%d. Limiter:%s", num, m->limiter_enabled ? "On" : "Off");
        break;
    case MENU_ITEM_OLED_VIEW:
        snprintf(buf, sz, "%d. View:%s", num,
                 k_view_names[(m->oled_view < 5) ? m->oled_view : 0]);
        break;
    case MENU_ITEM_BASS_ENH:
        snprintf(buf, sz, "%d. Bass Enh:%s", num, m->bass_enabled ? "On" : "Off");
        break;
    case MENU_ITEM_EXCITER:
        snprintf(buf, sz, "%d. Exciter:%s", num,
                 m->exciter_enabled ? k_exc_names[(m->exciter_mode < PEQ_EXCITER_MODE_COUNT) ? m->exciter_mode : 0] : "Off");
        break;
    case MENU_ITEM_PREAMP_GAIN:
        snprintf(buf, sz, "%d. Preamp:%.1fdB", num, m->preamp_gain_db);
        break;
    case MENU_ITEM_AUTO_PREAMP:
        snprintf(buf, sz, "%d. AutoPreamp:%s", num, m->auto_preamp_enabled ? "On" : "Off");
        break;
    case MENU_ITEM_VOL_TAPER:
        snprintf(buf, sz, "%d. VolTaper:%s", num, m->vol_taper_log ? "Log" : "Std");
        break;
    case MENU_ITEM_SUBSONIC:
        snprintf(buf, sz, "%d. Subsonic:%s", num, m->subsonic_enabled ? "On" : "Off");
        break;
    case MENU_ITEM_AUDIO_SOURCE:
        snprintf(buf, sz, "%d. Input:%.10s", num,
                 m->current_source[0] ? m->current_source : "USB DAC");
        break;
    case MENU_ITEM_SAVE_NVS:
        snprintf(buf, sz, "%d. Save to DAC", num);
        break;
    case MENU_ITEM_EXIT:
        snprintf(buf, sz, "%d. Exit Menu", num);
        break;
    default:
        snprintf(buf, sz, " ");
        break;
    }
}

static void render_menu_screen(void)
{
    xSemaphoreTake(s_ui_mutex, portMAX_DELAY);
    ui_menu_state_t menu = s_current_menu_state;
    xSemaphoreGive(s_ui_mutex);

    oled_clear();

    // 1. Header (y: 0..9)
    oled_draw_string(18, 0, "== MAIN MENU ==", false);
    oled_draw_hline(0, 10, OLED_WIDTH, true);

    // 2. Menu items (Visible window of 4 items)
    int top_idx = 0;
    if (menu.selected_idx >= 3) {
        top_idx = menu.selected_idx - 2;
        if (top_idx > MENU_ITEM_COUNT - 4) {
            top_idx = MENU_ITEM_COUNT - 4;
        }
    }

    char line_buf[32];
    for (int i = 0; i < 4; i++) {
        int item_idx = top_idx + i;
        if (item_idx >= MENU_ITEM_COUNT) break;

        format_menu_item_label(item_idx, &menu, line_buf, sizeof(line_buf));

        bool is_selected = (item_idx == menu.selected_idx);
        int y_pos = 13 + (i * 9);
        if (is_selected) {
            oled_draw_rect(0, y_pos - 1, OLED_WIDTH, 9, true, true);
            oled_draw_string(2, y_pos, line_buf, true);
        } else {
            oled_draw_string(2, y_pos, line_buf, false);
        }
    }

    // 3. Footer (y: 52..63)
    oled_draw_hline(0, 50, OLED_WIDTH, true);
    oled_draw_string(2, 54, "Turn:Move  Click:OK", false);

    oled_flush();
}

static void render_eq_adjust_screen(void)
{
    xSemaphoreTake(s_ui_mutex, portMAX_DELAY);
    ui_eq_adjust_state_t eq = s_current_eq_state;
    xSemaphoreGive(s_ui_mutex);

    oled_clear();

    // 1. Header (y: 0..8): "BND:[3]/5  [PEAK]"
    char band_str[8];
    snprintf(band_str, sizeof(band_str), "[%d]", eq.active_band + 1);
    char total_str[8];
    snprintf(total_str, sizeof(total_str), "/%d", eq.num_bands);

    oled_draw_string(2, 0, "BND:", false);
    bool bnd_sel = (eq.cursor == EQ_CURSOR_BAND);
    oled_draw_string(26, 0, band_str, bnd_sel);
    oled_draw_string(26 + (int)strlen(band_str) * 6, 0, total_str, false);

    const char *type_str = "PEAK";
    if (eq.band.type == PEQ_FILTER_LOW_SHELF) type_str = "L-SH";
    else if (eq.band.type == PEQ_FILTER_HIGH_SHELF) type_str = "H-SH";
    else if (eq.band.type == PEQ_FILTER_HIGH_PASS) type_str = "HPF ";
    else if (eq.band.type == PEQ_FILTER_LOW_PASS) type_str = "LPF ";
    else if (eq.band.type == PEQ_FILTER_NOTCH) type_str = "NTCH";
    else if (eq.band.type == PEQ_FILTER_BAND_PASS) type_str = "BPF ";
    else if (eq.band.type == PEQ_FILTER_BYPASS) type_str = "BYP ";

    char type_box[12];
    snprintf(type_box, sizeof(type_box), "[%s]", type_str);
    bool type_sel = (eq.cursor == EQ_CURSOR_TYPE);
    oled_draw_string(80, 0, type_box, type_sel);

    oled_draw_hline(0, 9, OLED_WIDTH, true);

    // 2. Frequency (y: 11..19): "FREQ:  0 1 0 0 0  Hz"
    int f_int = (int)roundf(eq.band.freq_hz);
    if (f_int < 20) f_int = 20;
    if (f_int > 20000) f_int = 20000;
    int f_10k = (f_int / 10000) % 10;
    int f_1k  = (f_int / 1000) % 10;
    int f_100 = (f_int / 100) % 10;
    int f_10  = (f_int / 10) % 10;
    int f_1   = f_int % 10;

    oled_draw_string(2, 11, "FREQ:", false);
    oled_draw_char(38, 11, '0' + f_10k, eq.cursor == EQ_CURSOR_FREQ_10K);
    oled_draw_char(46, 11, '0' + f_1k,  eq.cursor == EQ_CURSOR_FREQ_1K);
    oled_draw_char(54, 11, '0' + f_100, eq.cursor == EQ_CURSOR_FREQ_100);
    oled_draw_char(62, 11, '0' + f_10,  eq.cursor == EQ_CURSOR_FREQ_10);
    oled_draw_char(70, 11, '0' + f_1,   eq.cursor == EQ_CURSOR_FREQ_1);
    oled_draw_string(82, 11, "Hz", false);

    // 3. Gain (y: 21..29): "GAIN:  + 0 4 . 5  dB"
    float g = eq.band.gain_db;
    if (g < -24.0f) g = -24.0f;
    if (g > 12.0f) g = 12.0f;
    int g_x10 = (int)roundf(fabsf(g) * 10.0f);
    char g_sign = (g_x10 == 0 || g >= 0.0f) ? '+' : '-';
    int g_tens = (g_x10 / 100) % 10;
    int g_ones = (g_x10 / 10) % 10;
    int g_dec  = g_x10 % 10;

    oled_draw_string(2, 21, "GAIN:", false);
    oled_draw_char(38, 21, g_sign,       eq.cursor == EQ_CURSOR_GAIN_SIGN);
    oled_draw_char(46, 21, '0' + g_tens, eq.cursor == EQ_CURSOR_GAIN_10);
    oled_draw_char(54, 21, '0' + g_ones, eq.cursor == EQ_CURSOR_GAIN_1);
    oled_draw_char(60, 21, '.',          false);
    oled_draw_char(66, 21, '0' + g_dec,  eq.cursor == EQ_CURSOR_GAIN_DEC);
    oled_draw_string(82, 21, "dB", false);

    // 4. Q Factor (y: 31..39): "Q   :  1 . 4 1" or "1 0 . 0 0"
    float q = eq.band.q_factor;
    if (q < 0.10f) q = 0.10f;
    if (q > 10.00f) q = 10.00f;
    int q_x100 = (int)roundf(q * 100.0f);
    int q_tens = q_x100 / 1000;
    int q_ones = (q_x100 / 100) % 10;
    int q_dec1 = (q_x100 / 10) % 10;
    int q_dec2 = q_x100 % 10;

    oled_draw_string(2, 31, "Q   :", false);
    oled_draw_char(40, 31, q_tens ? ('0' + q_tens) : ' ', eq.cursor == EQ_CURSOR_Q_INT && q_tens > 0);
    oled_draw_char(46, 31, '0' + q_ones, eq.cursor == EQ_CURSOR_Q_INT);
    oled_draw_char(54, 31, '.',          false);
    oled_draw_char(60, 31, '0' + q_dec1, eq.cursor == EQ_CURSOR_Q_DEC1);
    oled_draw_char(68, 31, '0' + q_dec2, eq.cursor == EQ_CURSOR_Q_DEC2);

    // 5. Hint text (y: 41..48)
    oled_draw_string(2, 42, "< >:Move  Turn:0-9", false);

    // 6. Action Toolbar (y: 52..63)
    oled_draw_hline(0, 50, OLED_WIDTH, true);
    oled_draw_string(2,   53, "[+B]",   eq.cursor == EQ_CURSOR_ACTION_ADD);
    oled_draw_string(32,  53, "[-B]",   eq.cursor == EQ_CURSOR_ACTION_DEL);
    oled_draw_string(62,  53, "[SAVE]", eq.cursor == EQ_CURSOR_ACTION_SAVE);
    oled_draw_string(101, 53, "[EXIT]", eq.cursor == EQ_CURSOR_ACTION_EXIT);

    // 7. Saved notification overlay popup
    if (eq.saved_notification) {
        oled_draw_rect(14, 18, 100, 26, true, false); // clear black box
        oled_draw_rect(14, 18, 100, 26, false, true); // white border
        oled_draw_string(25, 27, "SAVED TO DAC!", false);
    }

    oled_flush();
}

static void render_screen(void)
{
    ui_mode_t mode;
    xSemaphoreTake(s_ui_mutex, portMAX_DELAY);
    mode = s_current_ui_mode;
    xSemaphoreGive(s_ui_mutex);

    switch (mode) {
    case UI_MODE_MENU:
        render_menu_screen();
        break;
    case UI_MODE_EQ_ADJUST:
        render_eq_adjust_screen();
        break;
    case UI_MODE_PLAYBACK:
    default:
        render_playback_screen();
        break;
    }
}

static void oled_ui_task(void *pvParameters)
{
    ESP_LOGI(TAG, "OLED UI rendering task started on Core 0.");
    while (1) {
        s_frame_tick_cnt++;

        if (!ssd1306_is_online()) {
            s_probe_tick_cnt++;
            if (s_probe_tick_cnt >= 20) { // Non-blocking 2 s hot-plug probe
                s_probe_tick_cnt = 0;
                if (ssd1306_probe_and_init() == ESP_OK) {
                    s_is_dimmed = false;
                    s_is_blanked = false;
                }
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        if (s_usb_suspended) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        xSemaphoreTake(s_ui_mutex, portMAX_DELAY);
        TickType_t last_act = s_last_activity_tick;
        uint16_t dim_sec = s_dim_sec;
        bool blanked = s_is_blanked;
        bool dimmed = s_is_dimmed;
        xSemaphoreGive(s_ui_mutex);

        if (dim_sec > 0 && !webhid_peq_is_streaming()) {
            uint32_t idle_sec = (uint32_t)(pdTICKS_TO_MS(xTaskGetTickCount() - last_act) / 1000U);
            if (!blanked && idle_sec >= OLED_BLANK_SEC) {
                s_is_blanked = true;
                ssd1306_send_cmd1(0xAE);
            } else if (!blanked && !dimmed && idle_sec >= dim_sec) {
                s_is_dimmed = true;
                ssd1306_send_cmd2(0x81, 0x1F);
            }
        }

        if (s_is_dimmed && (s_frame_tick_cnt % 100) == 0) {
            static const int8_t shifts[4] = { 0, 1, 0, -1 };
            int phase = (s_frame_tick_cnt / 100) & 3;
            ssd1306_set_pixel_shift(shifts[phase], shifts[(phase + 1) & 3]);
        }

        if (!s_is_blanked) {
            render_screen();
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

esp_err_t oled_display_init(void)
{
    s_ui_mutex = xSemaphoreCreateMutex();
    if (!s_ui_mutex) return ESP_ERR_NO_MEM;

    s_last_activity_tick = xTaskGetTickCount();
    s_playback_view = (oled_playback_view_t)(peq_dsp_get_oled_view() % OLED_VIEW_COUNT);
    s_dim_sec = peq_dsp_get_oled_dim_sec();

    memset(&s_current_state, 0, sizeof(s_current_state));
    snprintf(s_current_state.mode_str, sizeof(s_current_state.mode_str), "USB DAC");
    snprintf(s_current_state.sample_rate_str, sizeof(s_current_state.sample_rate_str), "48.0k 24b");
    snprintf(s_current_state.title, sizeof(s_current_state.title), "ASCENDANCE Hi-Fi");
    snprintf(s_current_state.subtitle, sizeof(s_current_state.subtitle), "Ready");
    snprintf(s_current_state.eq_preset, sizeof(s_current_state.eq_preset), "Slot 1");
    s_current_state.volume = 50;
    s_current_state.is_playing = true;

    esp_err_t ret = ssd1306_bus_init();
    if (ret != ESP_OK) {
        return ret;
    }

    if (ssd1306_probe_and_init() != ESP_OK) {
        ESP_LOGW(TAG, "No SSD1306 found on 0x3C/0x3D at boot; background hot-plug probe active.");
    }

    memset(&s_current_menu_state, 0, sizeof(s_current_menu_state));
    snprintf(s_current_menu_state.current_preset, sizeof(s_current_menu_state.current_preset), "Slot 1");
    snprintf(s_current_menu_state.current_source, sizeof(s_current_menu_state.current_source), "USB DAC");
    s_current_menu_state.limiter_enabled = peq_dsp_is_limiter_enabled();
    s_current_menu_state.loudness_mode = (uint8_t)peq_dsp_get_loudness_mode();
    s_current_menu_state.oled_view = (uint8_t)s_playback_view;
    peq_harmonics_cfg_t harm;
    peq_dsp_get_harmonics_cfg(&harm);
    s_current_menu_state.bass_enabled = harm.bass_enabled;
    s_current_menu_state.exciter_enabled = harm.exciter_enabled;
    s_current_menu_state.exciter_mode = (uint8_t)harm.exciter_mode;

    s_current_eq_state.active_band = 0;
    s_current_eq_state.num_bands = 5;
    s_current_eq_state.cursor = EQ_CURSOR_FREQ_1K;
    s_current_eq_state.band.type = PEQ_FILTER_PEAKING;
    s_current_eq_state.band.freq_hz = 1000.0f;
    s_current_eq_state.band.gain_db = 0.0f;
    s_current_eq_state.band.q_factor = 1.0f;
    s_current_eq_state.band.enabled = true;
    s_current_eq_state.saved_notification = false;

    xTaskCreatePinnedToCore(oled_ui_task, "oled_ui_task", 4096, NULL, 2, NULL, 0);
    return ESP_OK;
}

void oled_display_notify_activity(void)
{
    if (!s_ui_mutex) return;
    bool need_wake = false;
    bool need_bright = false;

    xSemaphoreTake(s_ui_mutex, portMAX_DELAY);
    s_last_activity_tick = xTaskGetTickCount();
    if (s_is_blanked) {
        s_is_blanked = false;
        s_is_dimmed = false;
        need_wake = true;
        need_bright = true;
    } else if (s_is_dimmed) {
        s_is_dimmed = false;
        need_bright = true;
    }
    xSemaphoreGive(s_ui_mutex);

    if (need_wake && !s_usb_suspended) ssd1306_send_cmd1(0xAF);
    if (need_bright) ssd1306_set_pixel_shift(0, 0);
    if (need_bright && !s_usb_suspended) ssd1306_send_cmd2(0x81, 0xCF);
}

void oled_display_set_dim_sec(uint16_t dim_sec)
{
    if (!s_ui_mutex) return;
    xSemaphoreTake(s_ui_mutex, portMAX_DELAY);
    s_dim_sec = dim_sec;
    xSemaphoreGive(s_ui_mutex);
    oled_display_notify_activity();
}

void oled_display_sleep(void)
{
    s_usb_suspended = true;
    ssd1306_send_cmd1(0xAE);
}

void oled_display_wake(void)
{
    s_usb_suspended = false;
    s_is_blanked = false;
    s_is_dimmed = false;
    ssd1306_set_pixel_shift(0, 0);
    if (s_ui_mutex) {
        xSemaphoreTake(s_ui_mutex, portMAX_DELAY);
        s_last_activity_tick = xTaskGetTickCount();
        xSemaphoreGive(s_ui_mutex);
    }
    if (ssd1306_is_online()) {
        ssd1306_send_cmd2(0x81, 0xCF);
        ssd1306_send_cmd1(0xAF);
    }
}

void oled_display_set_state(const ui_display_state_t *state)
{
    if (!state || !s_ui_mutex) return;
    xSemaphoreTake(s_ui_mutex, portMAX_DELAY);
    s_current_state = *state;
    s_current_state.mode_str[sizeof(s_current_state.mode_str) - 1] = '\0';
    s_current_state.sample_rate_str[sizeof(s_current_state.sample_rate_str) - 1] = '\0';
    s_current_state.title[sizeof(s_current_state.title) - 1] = '\0';
    s_current_state.subtitle[sizeof(s_current_state.subtitle) - 1] = '\0';
    s_current_state.eq_preset[sizeof(s_current_state.eq_preset) - 1] = '\0';
    xSemaphoreGive(s_ui_mutex);
    oled_display_notify_activity();
}

void oled_display_show_volume_overlay(int volume)
{
    if (!s_ui_mutex) return;
    xSemaphoreTake(s_ui_mutex, portMAX_DELAY);
    s_overlay_vol_timeout = 15;
    s_overlay_vol_val = volume;
    s_current_state.volume = volume;
    xSemaphoreGive(s_ui_mutex);
    oled_display_notify_activity();
}

void oled_display_show_eq_overlay(const char *preset_name)
{
    if (!s_ui_mutex || !preset_name) return;
    xSemaphoreTake(s_ui_mutex, portMAX_DELAY);
    s_overlay_eq_timeout = 20;
    snprintf(s_overlay_eq_name, sizeof(s_overlay_eq_name), "%.23s", preset_name);
    s_curve_dirty = true;
    xSemaphoreGive(s_ui_mutex);
    oled_display_notify_activity();
}

void oled_display_set_eq_preset(const char *preset_name)
{
    if (!s_ui_mutex || !preset_name) return;
    xSemaphoreTake(s_ui_mutex, portMAX_DELAY);
    snprintf(s_current_state.eq_preset, sizeof(s_current_state.eq_preset), "%.15s", preset_name);
    s_curve_dirty = true;
    xSemaphoreGive(s_ui_mutex);
    oled_display_notify_activity();
}

void oled_display_set_sample_rate(int sample_rate)
{
    if (!s_ui_mutex || sample_rate <= 0) return;
    xSemaphoreTake(s_ui_mutex, portMAX_DELAY);
    snprintf(s_current_state.sample_rate_str, sizeof(s_current_state.sample_rate_str),
             "%.1fk 24b", (float)sample_rate / 1000.0f);
    s_curve_dirty = true;
    xSemaphoreGive(s_ui_mutex);
    oled_display_notify_activity();
}

void oled_display_set_ui_mode(ui_mode_t mode)
{
    if (!s_ui_mutex) return;
    xSemaphoreTake(s_ui_mutex, portMAX_DELAY);
    s_current_ui_mode = mode;
    xSemaphoreGive(s_ui_mutex);
    oled_display_notify_activity();
}

ui_mode_t oled_display_get_ui_mode(void)
{
    if (!s_ui_mutex) return UI_MODE_PLAYBACK;
    xSemaphoreTake(s_ui_mutex, portMAX_DELAY);
    ui_mode_t mode = s_current_ui_mode;
    xSemaphoreGive(s_ui_mutex);
    return mode;
}

void oled_display_update_menu_ex(const ui_menu_state_t *menu_state)
{
    if (!s_ui_mutex || !menu_state) return;
    xSemaphoreTake(s_ui_mutex, portMAX_DELAY);
    s_current_menu_state = *menu_state;
    s_current_menu_state.current_preset[sizeof(s_current_menu_state.current_preset) - 1] = '\0';
    s_current_menu_state.current_source[sizeof(s_current_menu_state.current_source) - 1] = '\0';
    xSemaphoreGive(s_ui_mutex);
    oled_display_notify_activity();
}

void oled_display_update_menu(int selected_index, const char *preset_name, const char *source_name, float preamp_gain_db)
{
    if (!s_ui_mutex) return;
    xSemaphoreTake(s_ui_mutex, portMAX_DELAY);
    s_current_menu_state.selected_idx = selected_index;
    if (preset_name) {
        snprintf(s_current_menu_state.current_preset, sizeof(s_current_menu_state.current_preset), "%.23s", preset_name);
    }
    if (source_name) {
        snprintf(s_current_menu_state.current_source, sizeof(s_current_menu_state.current_source), "%.19s", source_name);
    }
    s_current_menu_state.preamp_gain_db = preamp_gain_db;
    xSemaphoreGive(s_ui_mutex);
    oled_display_notify_activity();
}

void oled_display_update_eq_adjust(const ui_eq_adjust_state_t *eq_state)
{
    if (!s_ui_mutex || !eq_state) return;
    xSemaphoreTake(s_ui_mutex, portMAX_DELAY);
    s_current_eq_state = *eq_state;
    s_curve_dirty = true;
    xSemaphoreGive(s_ui_mutex);
    oled_display_notify_activity();
}

void oled_display_set_playback_view(oled_playback_view_t view)
{
    if ((unsigned int)view >= OLED_VIEW_COUNT) view = OLED_VIEW_HUD;
    if (!s_ui_mutex) return;
    xSemaphoreTake(s_ui_mutex, portMAX_DELAY);
    s_playback_view = view;
    if (view == OLED_VIEW_CURVE || view == OLED_VIEW_SPLIT) {
        s_curve_dirty = true;
    }
    xSemaphoreGive(s_ui_mutex);
    oled_display_notify_activity();
}

oled_playback_view_t oled_display_get_playback_view(void)
{
    if (!s_ui_mutex) return OLED_VIEW_HUD;
    xSemaphoreTake(s_ui_mutex, portMAX_DELAY);
    oled_playback_view_t v = s_playback_view;
    xSemaphoreGive(s_ui_mutex);
    return v;
}

void oled_display_invalidate_curve(void)
{
    if (!s_ui_mutex) return;
    xSemaphoreTake(s_ui_mutex, portMAX_DELAY);
    s_curve_dirty = true;
    xSemaphoreGive(s_ui_mutex);
}

