#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PEQ_MAX_BANDS         10
#define PEQ_DEFAULT_NUM_BANDS 5
#define PEQ_NUM_BANDS         PEQ_MAX_BANDS

typedef enum {
    PEQ_FILTER_BYPASS = 0,
    PEQ_FILTER_PEAKING,
    PEQ_FILTER_LOW_SHELF,
    PEQ_FILTER_HIGH_SHELF,
} peq_filter_type_t;

typedef enum {
    PEQ_PRESET_FLAT = 0,
    PEQ_PRESET_BASS_BOOST,
    PEQ_PRESET_HARMAN,
    PEQ_PRESET_USER_CUSTOM,
    PEQ_PRESET_COUNT
} peq_preset_id_t;

typedef struct {
    peq_filter_type_t type;
    float freq_hz;      // 20.0 Hz to 20000.0 Hz
    float gain_db;      // -24.0 dB to +12.0 dB
    float q_factor;     // 0.1 to 10.0
    bool  enabled;
} peq_band_param_t;

typedef struct {
    uint8_t num_bands;  // Dynamic active band count (1 to PEQ_MAX_BANDS)
    peq_band_param_t bands[PEQ_MAX_BANDS];
    float preamp_gain_db; // Pre-attenuation headroom (e.g. -3.0 dB)
    char name[24];
} peq_profile_t;


/**
 * @brief Initialize the 5-band Parametric Equalizer DSP engine.
 *
 * Configures biquad coefficient tables for all factory presets (Flat, Bass Boost, Harman)
 * and loads User Custom preset from NVS if available.
 *
 * @param sample_rate Current sample rate in Hz (e.g. 44100).
 * @return esp_err_t ESP_OK on success.
 */
esp_err_t peq_dsp_init(int sample_rate);

/**
 * @brief Update sample rate and recalculate all active biquad coefficients.
 *
 * Called whenever audio stream sample rate changes (e.g. 44.1k -> 48k).
 *
 * @param sample_rate New sampling rate in Hz.
 */
void peq_dsp_set_sample_rate(int sample_rate);

/**
 * @brief Switch active EQ preset with 15 ms click-free double-buffered crossfade.
 *
 * @param preset_id Target preset (PEQ_PRESET_FLAT, PEQ_PRESET_BASS_BOOST, PEQ_PRESET_HARMAN, PEQ_PRESET_USER_CUSTOM).
 */
void peq_dsp_select_preset(peq_preset_id_t preset_id);

/**
 * @brief Cycle to next preset in sequence (Flat -> Bass Boost -> Harman -> User Custom -> Flat).
 *
 * @return peq_preset_id_t Newly active preset ID.
 */
peq_preset_id_t peq_dsp_cycle_preset(void);

/**
 * @brief Get currently selected preset ID and its human-readable name.
 *
 * @param out_name Buffer to receive string name (at least 24 bytes).
 * @return peq_preset_id_t Active preset ID.
 */
peq_preset_id_t peq_dsp_get_current_preset(char *out_name);

/**
 * @brief Process interleaved stereo 16-bit PCM audio in-place through active PEQ biquads.
 *
 * Strictly adheres to Invariant 8 (hard float saturation clamping to [-32768, 32767]).
 * Runs on Core 1 inside the audio processing pipeline.
 *
 * @param samples Pointer to interleaved stereo 16-bit PCM buffer (L, R, L, R...).
 * @param num_frames Number of stereo frames (i.e. num_samples / 2).
 */
void peq_dsp_process_interleaved_s16(int16_t *samples, size_t num_frames);

/**
 * @brief Update a specific band in the User Custom profile in volatile SRAM.
 *
 * @param band_idx Band index (0 to PEQ_NUM_BANDS - 1).
 * @param param Pointer to new band parameters.
 * @return esp_err_t ESP_OK on success.
 */
esp_err_t peq_dsp_update_user_band(int band_idx, const peq_band_param_t *param);

/**
 * @brief Explicitly persist User Custom profile to NVS flash with delta check.
 *
 * @return esp_err_t ESP_OK on success.
 */
esp_err_t peq_dsp_save_user_profile_nvs(void);

/**
 * @brief Get full configuration of a specific preset profile.
 *
 * @param preset_id Target preset ID.
 * @param out_profile Pointer to struct to receive copy of profile.
 * @return esp_err_t ESP_OK on success.
 */
esp_err_t peq_dsp_get_profile(peq_preset_id_t preset_id, peq_profile_t *out_profile);

/**
 * @brief Update preamp headroom gain for the User Custom profile in volatile SRAM.
 *
 * @param gain_db Headroom attenuation (e.g. -3.0 dB).
 * @return esp_err_t ESP_OK on success.
 */
esp_err_t peq_dsp_set_preamp(float gain_db);

/**
 * @brief Get the currently active dynamic band count.
 *
 * @return int Active band count (1 to PEQ_MAX_BANDS).
 */
int peq_dsp_get_band_count(void);

/**
 * @brief Dynamically set the active band count.
 *
 * @param num_bands Target band count (1 to PEQ_MAX_BANDS).
 * @return esp_err_t ESP_OK on success.
 */
esp_err_t peq_dsp_set_band_count(int num_bands);

#ifdef __cplusplus
}
#endif


