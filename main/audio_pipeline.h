#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the primary I2S0 audio output channel to PCM5102A DAC.
 *
 * Configures I2S standard Philips mode on:
 *   - LCK / WS : GPIO 4 (BOARD_I2S_WS_PIN)
 *   - DIN / DO : GPIO 5 (BOARD_I2S_DATA_OUT_PIN)
 *   - BCK      : GPIO 6 (BOARD_I2S_BCK_PIN)
 * Also configures hardware soft mute on GPIO 7 (BOARD_DAC_MUTE_PIN).
 *
 * @param sample_rate Audio sample rate in Hz (e.g. 44100, 48000).
 * @return esp_err_t ESP_OK on success, or error code.
 */
esp_err_t audio_pipeline_init(int sample_rate);

/**
 * @brief Set hardware soft mute state on PCM5102A DAC (GPIO 7: XMT).
 *
 * Active-LOW:
 *   - mute = true  -> GPIO 7 driven LOW  (analog outputs smoothly decay to 0V)
 *   - mute = false -> GPIO 7 driven HIGH (normal audio playback)
 *
 * @param mute true to assert mute, false to unmute.
 */
void audio_pipeline_set_mute(bool mute);

/**
 * @brief Check if hardware mute is currently asserted.
 *
 * @return true if muted, false otherwise.
 */
bool audio_pipeline_is_muted(void);

/**
 * @brief Dynamically reconfigure the I2S clock for a new sample rate.
 *
 * Strictly adheres to Invariant 6:
 * Disables channel -> reconfigures clock dividers -> re-enables channel.
 *
 * @param new_rate Target sample rate in Hz.
 */
void audio_pipeline_set_sample_rate(int new_rate);

/**
 * @brief Get the currently configured sample rate.
 *
 * @return int Sample rate in Hz.
 */
int audio_pipeline_get_sample_rate(void);

/**
 * @brief Read lock-free I2S TX DMA completion telemetry for UAC1 async clock feedback.
 *
 * @param out_frames_sent Optional pointer receiving cumulative stereo frames shifted out by I2S DMA.
 * @param out_last_us     Optional pointer receiving esp_timer_get_time() at last DMA EOF ISR.
 * @param out_underruns   Optional pointer receiving cumulative I2S TX queue underrun count.
 */
void audio_pipeline_get_dma_sync(uint32_t *out_frames_sent, int64_t *out_last_us, uint32_t *out_underruns);

/**
 * @brief Set software master volume with saturation protection.
 *
 * @param volume_percent Volume level from 0 to 100.
 */
void audio_pipeline_set_volume(int volume_percent);

/**
 * @brief Get the current software master volume.
 *
 * @return int Volume level from 0 to 100.
 */
int audio_pipeline_get_volume(void);

/**
 * @brief Adjust volume by a relative delta (+/-).
 *
 * Clamped between 0 and 100.
 *
 * @param delta Positive or negative integer step.
 * @return int New volume level from 0 to 100.
 */
int audio_pipeline_adjust_volume(int delta);

/**
 * @brief Get the current perceptual linear volume multiplier (0.0f to 1.0f).
 *
 * @return float Volume scaling factor.
 */
float audio_pipeline_get_volume_factor(void);

/**
 * @brief Push stereo 16-bit PCM audio samples to 32-bit I2S DMA.
 *
 * Applies current software volume scaling and up-shifts to 32-bit MSB-aligned slots.
 *
 * @param data Interleaved stereo 16-bit PCM buffer (L, R, L, R...).
 * @param size_bytes Size of data in bytes.
 */
void audio_pipeline_write(const void *data, size_t size_bytes);

/**
 * @brief Push pre-processed 32-bit MSB-aligned stereo PCM frames directly to I2S DMA.
 *
 * @param data Interleaved stereo int32_t buffer (L, R, L, R...).
 * @param size_bytes Size of data in bytes.
 */
void audio_pipeline_write_s32(const int32_t *data, size_t size_bytes);

/**
 * @brief Output a synthesized pure sine wave test tone.
 *
 * Used for hardware bench testing, frequency response sweeps, and pop checks.
 *
 * @param freq_hz Tone frequency in Hz.
 * @param duration_ms Duration in milliseconds.
 * @param sample_rate Sample rate in Hz.
 */
void audio_pipeline_play_tone(float freq_hz, int duration_ms, int sample_rate);

/**
 * @brief Play startup verification chime arpeggio (C5 - E5 - G5 - C6).
 *
 * Validates DAC, amplifier, and 3.5mm TRS headphone jack integrity on boot.
 */
void audio_pipeline_play_startup_chime(void);

/**
 * @brief Gracefully deinitialize the I2S pipeline and release DMA descriptors.
 *
 * @return esp_err_t ESP_OK on success.
 */
esp_err_t audio_pipeline_deinit(void);

#ifdef __cplusplus
}
#endif
