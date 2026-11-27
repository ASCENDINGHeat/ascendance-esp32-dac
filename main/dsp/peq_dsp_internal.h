#pragma once

#include "peq_dsp.h"

#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <math.h>

#include "esp_log.h"
#include "esp_dsp.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "nvs.h"

#define TAG "PEQ_DSP"
#define NVS_NAMESPACE       "audio_peq"
#define NVS_KEY_USER_V1     "user_profile"
#define NVS_KEY_USER_V2     "user_cfg_v2"
#define NVS_KEY_META_V3     "nvs_meta_v3"
#define NVS_KEY_FIR_L       "fir_taps_l"
#define NVS_KEY_FIR_R       "fir_taps_r"
#define NVS_KEY_HARM_V1     "harm_cfg_v1"

#define PEQ_NVS_MAGIC_V2    0x50455132UL // "PEQ2"
#define PEQ_NVS_VERSION_V2  2U
#define PEQ_NVS_MAGIC_V3    0x50455133UL // "PEQ3"
#define PEQ_NVS_VERSION_V3  3U
#define PEQ_META_MAGIC_V3   0x4D455433UL // "MET3"
#define PEQ_META_VERSION_V3 3U
#define PEQ_FIR_MAGIC       0x46495231UL // "FIR1"
#define PEQ_HARM_MAGIC_V1   0x48415231UL // "HAR1"
#define PEQ_HARM_VERSION_V1 1U

#define CROSSFEED_MAX_DELAY 32
#define FIR_BLOCK_MAX       128
#define LA_MAX_FRAMES       144          // 1.5 ms @ 96 kHz
#define FULLSCALE_S24_F     8388608.0f
#define PEAK_CEILING_S24    7918595.0f   // -0.5 dBFS
#define SOFT_KNEE_S24       8196846.0f   // -0.2 dBFS

#define HARM_LUT_SIZE       4096U
#define HARM_LUT_MAX_IN     2.0f
#define HARM_HB_SECTIONS    3
#define HARM_BASS_MAX_MIX   0.65f
#define HARM_EXC_MAX_MIX    0.55f

typedef struct {
    float b0, b1, b2, a1, a2;
    float w1_l, w2_l;
    float w1_r, w2_r;
} biquad_state_t;

typedef struct {
    float m_ll, m_rl;
    float m_lr, m_rr;
    bool  crossfeed_en;
    float crossfeed_gain;
    float cf_alpha;
    int   cf_delay_samples;
} mixer_rt_t;

typedef struct {
    float x1_l, y1_l;
    float x1_r, y1_r;
    float r;
} dc_block_state_t;

typedef struct {
    float up_s0[HARM_HB_SECTIONS];
    float up_s1[HARM_HB_SECTIONS];
    float dn_s0[HARM_HB_SECTIONS];
    float dn_s1[HARM_HB_SECTIONS];
} hb_os2x_state_t;

typedef struct {
    bool               bass_active;
    bool               exc_active;
    peq_exciter_mode_t exc_mode;
    float              bass_mix;
    float              exc_mix;
    bool               use_oversampling;
    biquad_state_t     bass_pre_lpf;
    biquad_state_t     bass_post_lpf;
    biquad_state_t     exc_pre_hpf;
    biquad_state_t     exc_post_hpf;
    dc_block_state_t   bass_dc;
    dc_block_state_t   exc_dc;
    hb_os2x_state_t    bass_os_l;
    hb_os2x_state_t    bass_os_r;
    hb_os2x_state_t    exc_os_l;
    hb_os2x_state_t    exc_os_r;
} harm_rt_t;

typedef struct {
    bool          linked_stereo;
    peq_profile_t left;
    peq_profile_t right;
} peq_slot_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint8_t  linked_stereo;
    uint8_t  reserved;
    peq_profile_t left;
    peq_profile_t right;
    peq_mixer_config_t mixer;
    uint32_t crc32;
} peq_nvs_v2_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint8_t  linked_stereo;
    uint8_t  reserved;
    peq_profile_t left;
    peq_profile_t right;
    uint32_t crc32;
} peq_nvs_slot_v3_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint8_t  active_slot;
    uint8_t  limiter_enabled;
    uint8_t  loudness_mode;
    uint8_t  oled_view;
    uint8_t  fir_enabled;
    uint8_t  reserved;
    uint16_t auto_mute_sec;
    uint16_t oled_dim_sec;
    peq_mixer_config_t mixer;
    uint32_t crc32;
} peq_nvs_meta_v3_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t num_taps;
    uint16_t reserved;
    float    taps[PEQ_FIR_MAX_TAPS_48K];
    uint32_t crc32;
} peq_fir_nvs_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint8_t  bass_enabled;
    uint8_t  bass_amount_pct;
    uint16_t bass_freq_hz;
    uint8_t  exciter_enabled;
    uint8_t  exciter_mode;
    uint8_t  exciter_amount_pct;
    uint8_t  reserved;
    uint16_t exciter_freq_hz;
    uint32_t crc32;
} peq_harm_nvs_v1_t;


/* Shared Core 0/1 synchronization lock & state (defined in main/dsp/peq_dsp.c) */
extern portMUX_TYPE s_peq_lock;

extern peq_slot_t s_slots[PEQ_USER_SLOT_COUNT];
extern bool s_ab_bypass;
extern peq_preset_id_t s_active_preset;
extern peq_mixer_config_t s_mixer;
extern int s_sample_rate;
extern uint32_t s_state_gen;

extern bool                s_limiter_enabled;
extern peq_loudness_mode_t s_loudness_mode;
extern int                 s_volume_pct;
extern uint8_t             s_oled_view;
extern uint16_t            s_auto_mute_sec;
extern uint16_t            s_oled_dim_sec;
extern bool                s_fir_enabled;
extern bool                s_auto_preamp_enabled;
extern float               s_auto_preamp_db;

extern peq_harmonics_cfg_t s_harm_cfg;
extern float *s_harm_lut_bass;
extern float *s_harm_lut_exciter[PEQ_EXCITER_MODE_COUNT];

extern float    s_fir_upload_l[PEQ_FIR_MAX_TAPS_48K];
extern float    s_fir_upload_r[PEQ_FIR_MAX_TAPS_48K];
extern uint16_t s_fir_taps_l;
extern uint16_t s_fir_taps_r;
extern float    s_fir_stage_buf_l[PEQ_FIR_MAX_TAPS_48K];
extern float    s_fir_stage_buf_r[PEQ_FIR_MAX_TAPS_48K];
extern uint16_t s_fir_stage_total_l;
extern uint16_t s_fir_stage_total_r;
extern bool     s_pending_fir_update;

extern __attribute__((aligned(16))) float s_fir_coeffs_l[PEQ_FIR_MAX_TAPS_48K];
extern __attribute__((aligned(16))) float s_fir_coeffs_r[PEQ_FIR_MAX_TAPS_48K];
extern __attribute__((aligned(16))) float s_fir_delay_l[PEQ_FIR_MAX_TAPS_48K + 16];
extern __attribute__((aligned(16))) float s_fir_delay_r[PEQ_FIR_MAX_TAPS_48K + 16];
extern fir_f32_t s_fir_rt_l;
extern fir_f32_t s_fir_rt_r;
extern bool      s_fir_rt_active_l;
extern bool      s_fir_rt_active_r;
extern bool      s_fir_rt_en;

static inline void set_passthrough_coeffs(biquad_state_t *out_bq)
{
    out_bq->b0 = 1.0f;
    out_bq->b1 = 0.0f;
    out_bq->b2 = 0.0f;
    out_bq->a1 = 0.0f;
    out_bq->a2 = 0.0f;
}

/* Internal cross-module helpers */
void calculate_biquad(const peq_band_param_t *band, float sample_rate, biquad_state_t *out_bq);
bool biquad_is_passthrough(const biquad_state_t *bq);
void compute_loudness_biquads(peq_loudness_mode_t mode, int vol_pct, float sample_rate, biquad_state_t out_bq[2]);
void apply_profile_to_biquads(const peq_profile_t *prof, float sample_rate, bool ab_bypass,
                              biquad_state_t *out_bq, int *out_num_bands, bool *out_is_flat);
void calc_butterworth_lpf(double fc, double sr, biquad_state_t *out);
void calc_butterworth_hpf(double fc, double sr, biquad_state_t *out);
float compute_auto_preamp_db(const biquad_state_t *bq_l, int nb_l,
                             const biquad_state_t *bq_r, int nb_r,
                             bool loud_en, const biquad_state_t loud_bq[2],
                             float sample_rate, const peq_harmonics_cfg_t *harm);

void compute_mixer_rt(const peq_mixer_config_t *cfg, float sample_rate, mixer_rt_t *out);
void setup_default_mixer(peq_mixer_config_t *cfg);
void rebuild_fir_rt_locked(int sr, bool en, uint16_t taps_l, uint16_t taps_r);

void clamp_harmonics_cfg(peq_harmonics_cfg_t *cfg);
void setup_default_harmonics(peq_harmonics_cfg_t *cfg);
void compute_harm_rt(const peq_harmonics_cfg_t *cfg, int sample_rate,
                     bool ab_bypass, harm_rt_t *out);
void init_harmonics_luts_once(void);

uint32_t calc_crc32(const uint8_t *data, size_t len);
void init_flat_profile(peq_profile_t *prof, const char *slot_name);
void setup_default_slots(void);
void sanitize_profile(peq_profile_t *prof);
esp_err_t save_slot_nvs_internal(int slot_id);
esp_err_t save_meta_nvs_internal(void);
void save_fir_nvs_internal(peq_channel_t ch);
esp_err_t save_harm_nvs_internal(void);
void load_user_profile_nvs(void);

void stage_pending_from_active_ex(bool reset_states, bool stage_mixer, bool stage_fir, bool reinit_harm);
void stage_pending_from_active(bool reset_states, bool stage_mixer, bool stage_fir);
void stage_pending_harmonics(bool reset_states);
