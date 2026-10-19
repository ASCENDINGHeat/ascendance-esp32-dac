#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the onboard WS2812B aRGB LED on GPIO 48 using the ESP-IDF RMT driver.
 *
 * Configures the RMT TX channel and bytes encoder for the WS2812B single-wire
 * 800 kHz NRZ timing protocol on Core 0. Sets the initial LED state.
 *
 * @return esp_err_t ESP_OK on success, or error code.
 */
esp_err_t led_indicator_init(void);

/**
 * @brief Update WS2812B LED brightness and color to indicate volume loudness and mute state.
 *
 * Optical Safety & Perceptual Mapping:
 *   - Muted (is_muted == true): Soft Red warning indicator (dim red alert).
 *   - Volume 0%: LED is completely OFF (silent / quiescent state).
 *   - Volume 1% .. 100%:
 *       - Brightness scales proportionally with loudness, strictly capped at ~25% duty
 *         cycle (max value 60 out of 255) in compliance with hardware optical safety rules.
 *       - Color spectrum tracks audio intensity:
 *           * 1% - 35%  : Deep Blue / Teal (subtle, relaxing background level)
 *           * 36% - 75% : Vibrant Pure Cyan (balanced nominal listening zone)
 *           * 76% - 100%: Electric Cyan-Mint (high-energy dynamic peak level)
 *
 * @param volume_percent Volume level from 0 to 100.
 * @param is_muted True if the pipeline is currently muted.
 */
void led_indicator_set_volume(int volume_percent, bool is_muted);

/**
 * @brief Set raw RGB color values directly on the WS2812B LED.
 *
 * Used for chime startup effects, diagnostics, or system state transitions.
 *
 * @param red   Red channel (0 - 255).
 * @param green Green channel (0 - 255).
 * @param blue  Blue channel (0 - 255).
 */
void led_indicator_set_rgb(uint8_t red, uint8_t green, uint8_t blue);

/**
 * @brief Turn off the WS2812B LED completely.
 */
void led_indicator_clear(void);

#ifdef __cplusplus
}
#endif
