#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CONTROL_EVT_NONE = 0,
    CONTROL_EVT_ROTARY_DELTA,       // Param: int delta (+/- steps)
    CONTROL_EVT_ENC_CLICK,          // Short press on rotary push switch (GPIO 17)
    CONTROL_EVT_ENC_DOUBLE_CLICK,   // Double click on rotary push switch (GPIO 17)
    CONTROL_EVT_ENC_LONG_PRESS,     // Long press (>800ms) on rotary push switch (GPIO 17)
    CONTROL_EVT_MODE_CLICK,         // Short press on mode button / Button 1 (GPIO 18)
    CONTROL_EVT_MODE_DOUBLE_CLICK,  // Double click on mode button / Button 1 (GPIO 18)
    CONTROL_EVT_MODE_LONG_PRESS,    // Long press (>800ms) on mode button / Button 1 (GPIO 18)
    CONTROL_EVT_BTN1_CLICK = CONTROL_EVT_MODE_CLICK,
    CONTROL_EVT_BTN1_DOUBLE_CLICK = CONTROL_EVT_MODE_DOUBLE_CLICK,
    CONTROL_EVT_BTN1_LONG_PRESS = CONTROL_EVT_MODE_LONG_PRESS,
    CONTROL_EVT_BTN2_CLICK,         // Short press on Button 2 (GPIO 10)
    CONTROL_EVT_BTN2_DOUBLE_CLICK,  // Double click on Button 2 (GPIO 10)
    CONTROL_EVT_BTN2_LONG_PRESS,    // Long press (>800ms) on Button 2 (GPIO 10)
} control_event_type_t;

typedef struct {
    control_event_type_t type;
    int32_t value;                  // e.g. delta for ROTARY_DELTA
} control_event_t;

/**
 * @brief Initialize the rotary encoder (PCNT on GPIO 15/16) and button inputs (GPIO 17/18).
 *
 * Configures hardware Pulse Counter (PCNT) with 1000ns glitch filtering on
 * GPIO 15 (Phase A) and GPIO 16 (Phase B), and sets up debounced GPIO inputs
 * with pull-ups on GPIO 17 (SW) and GPIO 18 (Mode button).
 * Spawns the controls polling task on Core 0.
 *
 * @param out_event_queue Pointer to receive the FreeRTOS QueueHandle for control events.
 * @return esp_err_t ESP_OK on success.
 */
esp_err_t controls_init(QueueHandle_t *out_event_queue);

/**
 * @brief Directly read the instantaneous delta from the PCNT hardware encoder.
 *
 * @return int Positive for clockwise, negative for counter-clockwise, 0 for no movement.
 */
int controls_read_encoder_delta(void);

/**
 * @brief Deinitialize controls and stop the task.
 */
void controls_deinit(void);

#ifdef __cplusplus
}
#endif
