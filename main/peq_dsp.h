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
    PEQ_FILTER_HIGH_PASS,
    PEQ_FILTER_LOW_PASS,
    PEQ_FILTER_NOTCH,
    PEQ_FILTER_BAND_PASS,
    PEQ_FILTER_TYPE_COUNT
} peq_filter_type_t;

typedef enum {
    PEQ_CH_LINKED = 0,
    PEQ_CH_LEFT   = 1,
    PEQ_CH_RIGHT  = 2,
} peq_channel_t;

typedef enum {
    MIXER_MODE_STEREO = 0,
    MIXER_MODE_MONO,
    MIXER_MODE_SWAP,
    MIXER_MODE_WIDTH,
    MIXER_MODE_CROSSFEED,
    MIXER_MODE_CUSTOM_MATRIX,
    MIXER_MODE_COUNT
} mixer_mode_t;

typedef struct {
    mixer_mode_t mode;
    int8_t       balance;            // -100 (Full Left) to +100 (Full Right), 0 = Center
    uint8_t      width_pct;          // 0 (Mono) to 200 (Ultra-Wide), 100 = 1.0
    uint8_t      crossfeed_level;    // 0 = Off, 1 = Low (-4.5 dB), 2 = High (-6.0 dB)
    float        custom_matrix[2][2];// [mLL, mRL], [mLR, mRR]
} peq_mixer_config_t;

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
 * @brief Initialize the Parametric Equalizer DSP engine.
 *
 * Configures biquad coefficient tables for all factory presets (Flat, Bass Boost, Harman)
 * and loads User Custom configuration (Dual-Mono L/R PEQ + Matrix/Crossfeed) from NVS.
 *
 * @param sample_rate Current sample rate in Hz (e.g. 44100, 48000, 96000).
 * @return esp_err_t ESP_OK on success.
 */
esp_err_t peq_dsp_init(int sample_rate);

/**
 * @brief Update sample rate and recalculate all active biquad coefficients and crossfeed delay.
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
 * @brief Process packed 24-bit stereo PCM (3 bytes/sample) to 32-bit MSB-aligned I2S frames.
 *
 * Applies Dual-Mono L/R PEQ, 15 ms crossfading, A/B bypass, Channel Matrix / Crossfeed,
 * software master volume scaling, and 24-bit saturation clamping [-8388608, 8388607].
 *
 * @param src_s24_packed Pointer to 24-bit little-endian stereo PCM (6 bytes per frame).
 * @param dst_s32 Output buffer of interleaved int32_t stereo I2S samples (8 bytes per frame).
 * @param num_frames Number of stereo frames.
 * @param volume_factor Linear amplitude volume multiplier (0.0f to 1.0f).
 */
void peq_dsp_process_interleaved_s24_to_s32(const uint8_t *src_s24_packed, int32_t *dst_s32,
                                            size_t num_frames, float volume_factor);

/**
 * @brief Process interleaved stereo 16-bit PCM into 32-bit MSB-aligned I2S frames.
 *
 * @param src_s16 Input 16-bit interleaved stereo PCM.
 * @param dst_s32 Output 32-bit interleaved stereo I2S PCM.
 * @param num_frames Number of stereo frames.
 * @param volume_factor Linear amplitude volume multiplier (0.0f to 1.0f).
 */
void peq_dsp_process_interleaved_s16_to_s32(const int16_t *src_s16, int32_t *dst_s32,
                                            size_t num_frames, float volume_factor);

/**
 * @brief Update a specific band in the User Custom profile (Linked Stereo) in volatile SRAM.
 *
 * @param band_idx Band index (0 to PEQ_NUM_BANDS - 1).
 * @param param Pointer to new band parameters.
 * @return esp_err_t ESP_OK on success.
 */
esp_err_t peq_dsp_update_user_band(int band_idx, const peq_band_param_t *param);

/**
 * @brief Update a specific band for Linked Stereo, Left, or Right channel in volatile SRAM.
 *
 * @param ch Channel target (PEQ_CH_LINKED, PEQ_CH_LEFT, PEQ_CH_RIGHT).
 * @param band_idx Band index (0 to PEQ_MAX_BANDS - 1).
 * @param param Pointer to new band parameters.
 * @return esp_err_t ESP_OK on success.
 */
esp_err_t peq_dsp_update_user_band_ch(peq_channel_t ch, int band_idx, const peq_band_param_t *param);

/**
 * @brief Explicitly persist User Custom Dual-Mono L/R & Matrix config to NVS with delta check.
 *
 * @return esp_err_t ESP_OK on success.
 */
esp_err_t peq_dsp_save_user_profile_nvs(void);

/**
 * @brief Get full configuration of a specific preset profile (Left/Primary bank).
 *
 * @param preset_id Target preset ID.
 * @param out_profile Pointer to struct to receive copy of profile.
 * @return esp_err_t ESP_OK on success.
 */
esp_err_t peq_dsp_get_profile(peq_preset_id_t preset_id, peq_profile_t *out_profile);

/**
 * @brief Get profile for a specific channel (Left or Right) of a preset.
 */
esp_err_t peq_dsp_get_profile_ch(peq_preset_id_t preset_id, peq_channel_t ch, peq_profile_t *out_profile);

/**
 * @brief Get Dual-Mono link state and both Left and Right User Custom profiles.
 */
esp_err_t peq_dsp_get_user_dual_mono(bool *out_linked, peq_profile_t *out_left, peq_profile_t *out_right);

/**
 * @brief Enable or disable Linked Stereo mode for User Custom PEQ.
 *
 * When enabling linked mode, Left channel profile is mirrored onto Right.
 */
esp_err_t peq_dsp_set_linked_stereo(bool linked);

/**
 * @brief Query whether User Custom PEQ is currently in Linked Stereo mode.
 */
bool peq_dsp_is_linked_stereo(void);

/**
 * @brief Update preamp headroom gain for the User Custom profile in volatile SRAM.
 *
 * @param gain_db Headroom attenuation (e.g. -3.0 dB).
 * @return esp_err_t ESP_OK on success.
 */
esp_err_t peq_dsp_set_preamp(float gain_db);

/**
 * @brief Get the currently active dynamic band count (primary/left bank).
 *
 * @return int Active band count (1 to PEQ_MAX_BANDS).
 */
int peq_dsp_get_band_count(void);

/**
 * @brief Get the active band count for a specific channel of the active preset.
 */
int peq_dsp_get_band_count_ch(peq_channel_t ch);

/**
 * @brief Dynamically set the active band count (Linked Stereo).
 *
 * @param num_bands Target band count (1 to PEQ_MAX_BANDS).
 * @return esp_err_t ESP_OK on success.
 */
esp_err_t peq_dsp_set_band_count(int num_bands);

/**
 * @brief Dynamically set the active band count for Linked, Left, or Right channel.
 */
esp_err_t peq_dsp_set_band_count_ch(peq_channel_t ch, int num_bands);

/**
 * @brief Enable or disable transient A/B flat bypass (preamp-normalized, 15 ms crossfade).
 */
void peq_dsp_set_ab_bypass(bool bypass);

/**
 * @brief Query whether transient A/B bypass is currently active.
 */
bool peq_dsp_get_ab_bypass(void);

/**
 * @brief Update the 2x2 Channel Matrix & Chu Moy Crossfeed configuration.
 */
esp_err_t peq_dsp_set_mixer(const peq_mixer_config_t *cfg);

/**
 * @brief Read the current 2x2 Channel Matrix & Chu Moy Crossfeed configuration.
 */
esp_err_t peq_dsp_get_mixer(peq_mixer_config_t *out_cfg);

#ifdef __cplusplus
}
#endif

