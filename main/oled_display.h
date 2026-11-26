#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#include "peq_dsp.h"

typedef enum {
    UI_MODE_PLAYBACK = 0,
    UI_MODE_MENU,
    UI_MODE_EQ_ADJUST,
} ui_mode_t;

typedef enum {
    OLED_VIEW_HUD = 0,
    OLED_VIEW_CURVE,
    OLED_VIEW_VU,
    OLED_VIEW_SPLIT,
    OLED_VIEW_COUNT
} oled_playback_view_t;

typedef enum {
    MENU_ITEM_EQ_ADJUST = 0,
    MENU_ITEM_EQ_PRESET,
    MENU_ITEM_AB_BYPASS,
    MENU_ITEM_CROSSFEED,
    MENU_ITEM_LOUDNESS,
    MENU_ITEM_LIMITER,
    MENU_ITEM_OLED_VIEW,
    MENU_ITEM_BASS_ENH,
    MENU_ITEM_EXCITER,
    MENU_ITEM_PREAMP_GAIN,
    MENU_ITEM_AUTO_PREAMP,
    MENU_ITEM_AUDIO_SOURCE,
    MENU_ITEM_SAVE_NVS,
    MENU_ITEM_EXIT,
    MENU_ITEM_COUNT
} menu_item_id_t;

typedef enum {
    EQ_CURSOR_BAND = 0,
    EQ_CURSOR_TYPE,
    EQ_CURSOR_FREQ_10K,
    EQ_CURSOR_FREQ_1K,
    EQ_CURSOR_FREQ_100,
    EQ_CURSOR_FREQ_10,
    EQ_CURSOR_FREQ_1,
    EQ_CURSOR_GAIN_SIGN,
    EQ_CURSOR_GAIN_10,
    EQ_CURSOR_GAIN_1,
    EQ_CURSOR_GAIN_DEC,
    EQ_CURSOR_Q_INT,
    EQ_CURSOR_Q_DEC1,
    EQ_CURSOR_Q_DEC2,
    EQ_CURSOR_ACTION_ADD,
    EQ_CURSOR_ACTION_DEL,
    EQ_CURSOR_ACTION_SAVE,
    EQ_CURSOR_ACTION_EXIT,
    EQ_CURSOR_COUNT
} eq_cursor_pos_t;

typedef struct {
    int   selected_idx;
    char  current_preset[24];
    char  current_source[20];
    float preamp_gain_db;
    bool  ab_bypass;
    uint8_t crossfeed_level;
    uint8_t loudness_mode;
    bool  limiter_enabled;
    uint8_t oled_view;
    bool  bass_enabled;
    bool  exciter_enabled;
    uint8_t exciter_mode;
    bool  auto_preamp_enabled;
} ui_menu_state_t;

typedef struct {
    uint8_t active_band;    // 0 .. num_bands - 1
    uint8_t num_bands;      // 1 .. PEQ_MAX_BANDS
    eq_cursor_pos_t cursor; // Active cursor position
    peq_band_param_t band;  // Current band parameters
    bool saved_notification;// Temporary "Saved!" banner
} ui_eq_adjust_state_t;

typedef struct {
    char mode_str[12];          // e.g. "USB DAC", "Synth"
    char sample_rate_str[12];   // e.g. "48.0k 24b"
    char title[32];             // e.g. "ASCENDANCE Hi-Fi"
    char subtitle[32];          // e.g. "Active / Ready"
    char eq_preset[16];         // e.g. "Slot 1"
    int  volume;                // 0 - 100
    bool is_muted;
    bool is_playing;
} ui_display_state_t;

/**
 * @brief Initialize the SSD1306 128x64 OLED display on I2C Master (GPIO 8 SDA, GPIO 9 SCL).
 *        Auto-probes 0x3C and 0x3D and supports non-blocking hot-plug recovery.
 */
esp_err_t oled_display_init(void);

void oled_display_set_state(const ui_display_state_t *state);
void oled_display_show_volume_overlay(int volume);
void oled_display_show_eq_overlay(const char *preset_name);
void oled_display_set_eq_preset(const char *preset_name);
void oled_display_set_sample_rate(int sample_rate);
void oled_display_set_ui_mode(ui_mode_t mode);
ui_mode_t oled_display_get_ui_mode(void);

void oled_display_update_menu_ex(const ui_menu_state_t *menu_state);
void oled_display_update_menu(int selected_index, const char *preset_name, const char *source_name, float preamp_gain_db);
void oled_display_update_eq_adjust(const ui_eq_adjust_state_t *eq_state);

/* ---- v0.2.0 Visualizer Views, Curve Cache & Power/Burn-in Controls ---- */
void oled_display_set_playback_view(oled_playback_view_t view);
oled_playback_view_t oled_display_get_playback_view(void);
void oled_display_invalidate_curve(void);
void oled_display_notify_activity(void);
void oled_display_set_dim_sec(uint16_t dim_sec);
void oled_display_sleep(void);
void oled_display_wake(void);

#ifdef __cplusplus
}
#endif
