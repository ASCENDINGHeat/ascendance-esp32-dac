#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AUDIO_SOURCE_STANDALONE = 0,    // High-Fidelity Standalone DSP/DAC Mode
    AUDIO_SOURCE_TONE_TEST,         // Harmonic Bench Tone Generator (audition PEQ curves)
    AUDIO_SOURCE_SWEEP,             // Frequency Sweep Diagnostic Mode (20 Hz - 20 kHz)
    AUDIO_SOURCE_COUNT
} audio_source_t;

/**
 * @brief Initialize the Audio Source Arbiter.
 *
 * Configures the source coordinator mutex and sets the default boot audio source.
 *
 * @param initial_source Starting source (default AUDIO_SOURCE_STANDALONE).
 * @return esp_err_t ESP_OK on success.
 */
esp_err_t audio_source_arbiter_init(audio_source_t initial_source);

/**
 * @brief Get the currently active audio source and human-readable name.
 *
 * @param out_name Optional buffer for string name (at least 20 bytes).
 * @return audio_source_t Currently active audio source.
 */
audio_source_t audio_source_arbiter_get_current(char *out_name);

/**
 * @brief Switch active audio source executing the 4-Phase Zero-Pop Muting Protocol.
 *
 * Protocol phases:
 *   1. Hardware Soft-Mute: Pull GPIO 7 LOW (15 ms)
 *   2. Pipeline Teardown: Pause old source generator
 *   3. DMA Flush & Reclock: Flush I2S DMA with zeros (10 ms)
 *   4. Unmute & Handover: Activate new source and pull GPIO 7 HIGH (15 ms)
 *
 * @param new_source Target audio source.
 * @return esp_err_t ESP_OK on success.
 */
esp_err_t audio_source_arbiter_switch_to(audio_source_t new_source);

/**
 * @brief Cycle to the next audio source.
 *
 * @return audio_source_t Newly engaged audio source.
 */
audio_source_t audio_source_arbiter_cycle(void);

#ifdef __cplusplus
}
#endif
