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
    MENU_ITEM_EQ_ADJUST = 0,
    MENU_ITEM_EQ_PRESET,
    MENU_ITEM_AUDIO_SOURCE,
    MENU_ITEM_PREAMP_GAIN,
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
    int selected_idx;
    char current_preset[24];
    char current_source[20];
    float preamp_gain_db;
} ui_menu_state_t;

typedef struct {
    uint8_t active_band;    // 0 .. num_bands - 1
    uint8_t num_bands;      // 1 .. PEQ_MAX_BANDS
    eq_cursor_pos_t cursor; // Active cursor position
    peq_band_param_t band;  // Current band parameters
    bool saved_notification;// Temporary "Saved!" banner
} ui_eq_adjust_state_t;

typedef struct {
    char mode_str[12];          // e.g. "DSP DAC", "Synth"
    char sample_rate_str[12];   // e.g. "44.1k 16b"
    char title[32];             // e.g. "ESP32-S3 Audio DSP"
    char subtitle[32];          // e.g. "Active / Ready"
    char eq_preset[16];         // e.g. "Flat", "Bass Boost"
    int  volume;                // 0 - 100
    bool is_muted;
    bool is_playing;
} ui_display_state_t;

/**
 * @brief Initialize the SSD1306 128x64 OLED display on I2C Master (GPIO 8 SDA, GPIO 9 SCL).
 *
 * Configures the I2C bus at 400 kHz, transmits the display power-up sequence,
 * initializes the 1024-byte framebuffer, and launches the UI render task on Core 0.
 *
 * @return esp_err_t ESP_OK on success.
 */
esp_err_t oled_display_init(void);

/**
 * @brief Thread-safe update of the display state.
 *
 * Copies the new state into the UI rendering buffer to be refreshed in the next frame.
 *
 * @param state Pointer to new UI state.
 */
void oled_display_set_state(const ui_display_state_t *state);

/**
 * @brief Signal a temporary volume change overlay popup.
 *
 * @param volume Current volume 0-100.
 */
void oled_display_show_volume_overlay(int volume);

/**
 * @brief Signal a temporary EQ preset change overlay popup.
 *
 * @param preset_name Name of the newly engaged EQ preset.
 */
void oled_display_show_eq_overlay(const char *preset_name);

/**
 * @brief Set the active UI screen mode (Playback, Menu, or EQ Adjust).
 *
 * @param mode Active UI mode.
 */
void oled_display_set_ui_mode(ui_mode_t mode);

/**
 * @brief Get the currently active UI screen mode.
 *
 * @return ui_mode_t Active UI mode.
 */
ui_mode_t oled_display_get_ui_mode(void);

/**
 * @brief Update the state rendered in Main Menu mode.
 *
 * @param selected_index Index of the highlighted item (0 to MENU_ITEM_COUNT - 1).
 * @param preset_name Active EQ preset name string.
 * @param source_name Active audio source string.
 * @param preamp_gain_db Current preamp headroom gain in dB.
 */
void oled_display_update_menu(int selected_index, const char *preset_name, const char *source_name, float preamp_gain_db);

/**
 * @brief Update the state rendered in Live EQ Adjust mode.
 *
 * @param eq_state Pointer to the active EQ editor state.
 */
void oled_display_update_eq_adjust(const ui_eq_adjust_state_t *eq_state);

#ifdef __cplusplus
}
#endif
