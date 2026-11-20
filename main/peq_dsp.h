#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PEQ_MAX_BANDS          10
#define PEQ_DEFAULT_NUM_BANDS  5
#define PEQ_NUM_BANDS          PEQ_MAX_BANDS
#define PEQ_USER_SLOT_COUNT    8
#define PEQ_SLOT_NAME_MAX      24
#define PEQ_FIR_MAX_TAPS_48K   256
#define PEQ_FIR_MAX_TAPS_96K   128

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
    PEQ_SLOT_0 = 0,
    PEQ_SLOT_1 = 1,
    PEQ_SLOT_2 = 2,
    PEQ_SLOT_3 = 3,
    PEQ_SLOT_4 = 4,
    PEQ_SLOT_5 = 5,
    PEQ_SLOT_6 = 6,
    PEQ_SLOT_7 = 7,
    PEQ_PRESET_COUNT       = PEQ_USER_SLOT_COUNT,
    PEQ_PRESET_FLAT        = PEQ_SLOT_0,
    PEQ_PRESET_USER_CUSTOM = PEQ_SLOT_0
} peq_preset_id_t;

typedef enum {
    PEQ_LOUDNESS_OFF = 0,
    PEQ_LOUDNESS_MILD,
    PEQ_LOUDNESS_FULL,
    PEQ_LOUDNESS_COUNT
} peq_loudness_mode_t;

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
    char name[PEQ_SLOT_NAME_MAX];
} peq_profile_t;

typedef struct {
    float    peak_db_l;
    float    peak_db_r;
    float    rms_db_l;
    float    rms_db_r;
    bool     limiter_active;
    bool     clip_latched;
    uint32_t silence_ms;
} peq_vu_metrics_t;

typedef struct {
    bool                limiter_enabled;
    peq_loudness_mode_t loudness_mode;
    uint8_t             oled_view;        // 0=HUD, 1=CURVE, 2=VU, 3=SPLIT
    uint16_t            auto_mute_sec;    // 0=Disabled, default 10 s
    uint16_t            oled_dim_sec;     // 0=Disabled, default 60 s
    bool                fir_enabled;
    uint16_t            fir_taps_l;
    uint16_t            fir_taps_r;
} peq_dynamics_cfg_t;

typedef enum {
    PEQ_EXCITER_TUBE = 0,
    PEQ_EXCITER_TAPE = 1,
    PEQ_EXCITER_AIR = 2,
    PEQ_EXCITER_MODE_COUNT
} peq_exciter_mode_t;

typedef struct {
    bool               bass_enabled;
    uint8_t            bass_amount_pct;     // 0..100 %
    uint16_t           bass_freq_hz;        // 50..150 Hz
    bool               exciter_enabled;
    peq_exciter_mode_t exciter_mode;        // PEQ_EXCITER_TUBE..PEQ_EXCITER_AIR
    uint8_t            exciter_amount_pct;  // 0..100 %
    uint16_t           exciter_freq_hz;     // 1500..8000 Hz
} peq_harmonics_cfg_t;

/**
 * @brief Initialize the Parametric Equalizer DSP engine.
 *
 * Initializes all 8 user-editable NVS v3 slots (default Reference Flat),
 * migrates any legacy v0.1.0 NVS v1/v2 user profile into Slot 0,
 * and initializes the FIR convolution engine, loudness contour, and lookahead limiter.
 *
 * @param sample_rate Current sample rate in Hz (e.g. 44100, 48000, 96000).
 * @return esp_err_t ESP_OK on success.
 */
esp_err_t peq_dsp_init(int sample_rate);

/**
 * @brief Update sample rate and recalculate all active biquad coefficients,
 *        loudness contour, lookahead limiter window, FIR tap cap, and crossfeed delay.
 */
void peq_dsp_set_sample_rate(int sample_rate);

/**
 * @brief Switch active user slot (0..7) with 15 ms click-free double-buffered crossfade.
 */
void peq_dsp_select_preset(peq_preset_id_t preset_id);

/**
 * @brief Cycle to next user profile slot (Slot 0 -> ... -> Slot 7 -> Slot 0).
 */
peq_preset_id_t peq_dsp_cycle_preset(void);

/**
 * @brief Get currently selected user slot ID and its human-readable name.
 */
peq_preset_id_t peq_dsp_get_current_preset(char *out_name);

void peq_dsp_process_interleaved_s16(int16_t *samples, size_t num_frames);

void peq_dsp_process_interleaved_s24_to_s32(const uint8_t *src_s24_packed, int32_t *dst_s32,
                                            size_t num_frames, float volume_factor);

void peq_dsp_process_interleaved_s16_to_s32(const int16_t *src_s16, int32_t *dst_s32,
                                            size_t num_frames, float volume_factor);

esp_err_t peq_dsp_update_user_band(int band_idx, const peq_band_param_t *param);
esp_err_t peq_dsp_update_user_band_ch(peq_channel_t ch, int band_idx, const peq_band_param_t *param);

esp_err_t peq_dsp_save_user_profile_nvs(void);
esp_err_t peq_dsp_rename_slot(int slot_id, const char *new_name);
esp_err_t peq_dsp_reset_slot_to_flat(int slot_id);

esp_err_t peq_dsp_get_profile(peq_preset_id_t preset_id, peq_profile_t *out_profile);
esp_err_t peq_dsp_get_profile_ch(peq_preset_id_t preset_id, peq_channel_t ch, peq_profile_t *out_profile);
esp_err_t peq_dsp_get_user_dual_mono(bool *out_linked, peq_profile_t *out_left, peq_profile_t *out_right);

esp_err_t peq_dsp_set_linked_stereo(bool linked);
bool peq_dsp_is_linked_stereo(void);

esp_err_t peq_dsp_set_preamp(float gain_db);

int peq_dsp_get_band_count(void);
int peq_dsp_get_band_count_ch(peq_channel_t ch);
esp_err_t peq_dsp_set_band_count(int num_bands);
esp_err_t peq_dsp_set_band_count_ch(peq_channel_t ch, int num_bands);

void peq_dsp_set_ab_bypass(bool bypass);
bool peq_dsp_get_ab_bypass(void);

esp_err_t peq_dsp_set_mixer(const peq_mixer_config_t *cfg);
esp_err_t peq_dsp_get_mixer(peq_mixer_config_t *out_cfg);

/* ---- v0.2.0 Studio Dynamics, FIR, Loudness, Telemetry & Auto-Mute ---- */

void peq_dsp_set_limiter_enabled(bool enabled);
bool peq_dsp_is_limiter_enabled(void);

void peq_dsp_set_loudness_mode(peq_loudness_mode_t mode);
peq_loudness_mode_t peq_dsp_get_loudness_mode(void);
void peq_dsp_notify_volume_pct(int volume_pct);

void peq_dsp_set_oled_view(uint8_t view);
uint8_t peq_dsp_get_oled_view(void);

void peq_dsp_set_auto_mute_sec(uint16_t sec);
uint16_t peq_dsp_get_auto_mute_sec(void);

void peq_dsp_set_oled_dim_sec(uint16_t sec);
uint16_t peq_dsp_get_oled_dim_sec(void);

void peq_dsp_get_dynamics_cfg(peq_dynamics_cfg_t *out_cfg);

/* ---- Psychoacoustic Bass Enhancer & Tube/Tape/Air Harmonic Exciter ---- */

void peq_dsp_set_harmonics_cfg(const peq_harmonics_cfg_t *cfg);
void peq_dsp_get_harmonics_cfg(peq_harmonics_cfg_t *out_cfg);

esp_err_t peq_dsp_set_fir_enabled(bool enabled);
bool peq_dsp_is_fir_enabled(void);
esp_err_t peq_dsp_upload_fir_chunk(peq_channel_t ch, uint16_t total_taps, uint16_t offset,
                                   uint8_t count, const float *taps, bool commit_nvs);
uint16_t peq_dsp_get_fir_tap_count(peq_channel_t ch);
uint16_t peq_dsp_get_fir_max_taps_for_sr(int sample_rate);

void peq_dsp_eval_curve_db(float *out_db, int num_points, float min_hz, float max_hz);
void peq_dsp_get_vu_metrics(peq_vu_metrics_t *out_vu, bool clear_clip);
void peq_dsp_clear_vu_metrics(void);

void peq_dsp_trigger_wake_ramp(void);
bool peq_dsp_is_signal_silent(void);

#ifdef __cplusplus
}
#endif

