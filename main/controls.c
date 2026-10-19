#include "controls.h"
#include "board_pins.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/pulse_cnt.h"
#include "driver/gpio.h"

#define TAG "CONTROLS"

static pcnt_unit_handle_t s_pcnt_unit = NULL;
static QueueHandle_t s_ctrl_queue = NULL;
static TaskHandle_t s_controls_task_handle = NULL;
static bool s_running = false;

int controls_read_encoder_delta(void)
{
    if (!s_pcnt_unit) return 0;
    int count = 0;
    esp_err_t ret = pcnt_unit_get_count(s_pcnt_unit, &count);
    if (ret == ESP_OK && count != 0) {
        pcnt_unit_clear_count(s_pcnt_unit);
        // EC11 quadrature generates 4 edges per physical detent in x4 mode
        int delta = count / 4;
        return delta;
    }
    return 0;
}

static esp_err_t init_pcnt_encoder(gpio_num_t pin_a, gpio_num_t pin_b)
{
    ESP_LOGI(TAG, "Configuring hardware PCNT on A=%d, B=%d", pin_a, pin_b);

    pcnt_unit_config_t unit_config = {
        .high_limit = 100,
        .low_limit = -100,
        .flags.accum_count = true,
    };
    esp_err_t ret = pcnt_new_unit(&unit_config, &s_pcnt_unit);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create PCNT unit: %s", esp_err_to_name(ret));
        return ret;
    }

    // Glitch filter: Rejects electrical noise/spikes shorter than 1000 ns (1 µs)
    pcnt_glitch_filter_config_t filter_config = {
        .max_glitch_ns = 1000,
    };
    pcnt_unit_set_glitch_filter(s_pcnt_unit, &filter_config);

    // Channel A setup
    pcnt_chan_config_t chan_a_config = {
        .edge_gpio_num = pin_a,
        .level_gpio_num = pin_b,
    };
    pcnt_channel_handle_t pcnt_chan_a = NULL;
    ret = pcnt_new_channel(s_pcnt_unit, &chan_a_config, &pcnt_chan_a);
    if (ret != ESP_OK) return ret;

    pcnt_channel_set_edge_action(pcnt_chan_a, PCNT_CHANNEL_EDGE_ACTION_DECREASE, PCNT_CHANNEL_EDGE_ACTION_INCREASE);
    pcnt_channel_set_level_action(pcnt_chan_a, PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_INVERSE);

    // Channel B setup
    pcnt_chan_config_t chan_b_config = {
        .edge_gpio_num = pin_b,
        .level_gpio_num = pin_a,
    };
    pcnt_channel_handle_t pcnt_chan_b = NULL;
    ret = pcnt_new_channel(s_pcnt_unit, &chan_b_config, &pcnt_chan_b);
    if (ret != ESP_OK) return ret;

    pcnt_channel_set_edge_action(pcnt_chan_b, PCNT_CHANNEL_EDGE_ACTION_INCREASE, PCNT_CHANNEL_EDGE_ACTION_DECREASE);
    pcnt_channel_set_level_action(pcnt_chan_b, PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_INVERSE);

    ret = pcnt_unit_enable(s_pcnt_unit);
    if (ret != ESP_OK) return ret;

    pcnt_unit_clear_count(s_pcnt_unit);
    ret = pcnt_unit_start(s_pcnt_unit);
    return ret;
}

typedef struct {
    gpio_num_t pin;
    bool is_pressed;
    uint32_t press_start_ms;
    uint32_t release_time_ms;
    bool long_press_fired;
    bool pending_single_click;
    control_event_type_t click_evt;
    control_event_type_t dbl_click_evt;
    control_event_type_t long_evt;
} button_state_t;

static void controls_task(void *pvParameters)
{
    const TickType_t poll_period = pdMS_TO_TICKS(20); // 50 Hz poll rate
    button_state_t btn_enc = {
        .pin = (gpio_num_t)BOARD_ENC_SW_PIN,
        .click_evt = CONTROL_EVT_ENC_CLICK,
        .dbl_click_evt = CONTROL_EVT_ENC_DOUBLE_CLICK,
        .long_evt = CONTROL_EVT_ENC_LONG_PRESS,
    };
    button_state_t btn_1 = {
        .pin = (gpio_num_t)BOARD_BTN_1_PIN,
        .click_evt = CONTROL_EVT_BTN1_CLICK,
        .dbl_click_evt = CONTROL_EVT_BTN1_DOUBLE_CLICK,
        .long_evt = CONTROL_EVT_BTN1_LONG_PRESS,
    };
    button_state_t btn_2 = {
        .pin = (gpio_num_t)BOARD_BTN_2_PIN,
        .click_evt = CONTROL_EVT_BTN2_CLICK,
        .dbl_click_evt = CONTROL_EVT_BTN2_DOUBLE_CLICK,
        .long_evt = CONTROL_EVT_BTN2_LONG_PRESS,
    };

    while (s_running) {
        uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);

        // 1. Poll Rotary Encoder
        int delta = controls_read_encoder_delta();
        if (delta != 0 && s_ctrl_queue) {
            control_event_t evt = {
                .type = CONTROL_EVT_ROTARY_DELTA,
                .value = delta,
            };
            xQueueSend(s_ctrl_queue, &evt, 0);
        }

        // 2. Poll Buttons
        button_state_t *buttons[3] = {&btn_enc, &btn_1, &btn_2};
        for (int i = 0; i < 3; i++) {
            button_state_t *b = buttons[i];
            bool raw_pressed = (gpio_get_level(b->pin) == 0); // Active-LOW

            if (raw_pressed) {
                if (!b->is_pressed) {
                    // Button press edge
                    b->is_pressed = true;
                    b->press_start_ms = now_ms;
                    b->long_press_fired = false;

                    // If double-click is supported and pending single click exists
                    if (b->dbl_click_evt != CONTROL_EVT_NONE && b->pending_single_click) {
                        if ((now_ms - b->release_time_ms) <= 280) {
                            // Double click confirmed!
                            b->pending_single_click = false;
                            control_event_t evt = {.type = b->dbl_click_evt, .value = 0};
                            if (s_ctrl_queue) xQueueSend(s_ctrl_queue, &evt, 0);
                        }
                    }
                } else {
                    // Button held down
                    if (!b->long_press_fired && (now_ms - b->press_start_ms) >= 800) {
                        b->long_press_fired = true;
                        b->pending_single_click = false;
                        control_event_t evt = {.type = b->long_evt, .value = 0};
                        if (s_ctrl_queue) xQueueSend(s_ctrl_queue, &evt, 0);
                    }
                }
            } else {
                if (b->is_pressed) {
                    // Button release edge
                    b->is_pressed = false;
                    b->release_time_ms = now_ms;

                    if (!b->long_press_fired) {
                        uint32_t hold_duration = now_ms - b->press_start_ms;
                        if (hold_duration >= 30 && hold_duration < 800) {
                            if (b->dbl_click_evt != CONTROL_EVT_NONE) {
                                // Wait to see if double click arrives
                                b->pending_single_click = true;
                            } else {
                                // No double-click supported, fire single click immediately
                                control_event_t evt = {.type = b->click_evt, .value = 0};
                                if (s_ctrl_queue) xQueueSend(s_ctrl_queue, &evt, 0);
                            }
                        }
                    }
                }

                // Check timeout for pending single click
                if (b->pending_single_click && (now_ms - b->release_time_ms) > 300) {
                    b->pending_single_click = false;
                    control_event_t evt = {.type = b->click_evt, .value = 0};
                    if (s_ctrl_queue) xQueueSend(s_ctrl_queue, &evt, 0);
                }
            }
        }

        vTaskDelay(poll_period);
    }

    vTaskDelete(NULL);
}

esp_err_t controls_init(QueueHandle_t *out_event_queue)
{
    ESP_LOGI(TAG, "Initializing hardware controls (Schematic 2 Rev 1.1: EC11 Rotary + SW + Dual Buttons SW6/SW8)...");

    s_ctrl_queue = xQueueCreate(16, sizeof(control_event_t));
    if (!s_ctrl_queue) {
        ESP_LOGE(TAG, "Failed to allocate control event queue!");
        return ESP_ERR_NO_MEM;
    }
    if (out_event_queue) {
        *out_event_queue = s_ctrl_queue;
    }

    // 1. Initialize EC11 PCNT Quadrature on GPIO 15 and 16
    esp_err_t ret = init_pcnt_encoder((gpio_num_t)BOARD_ENC_A_PIN, (gpio_num_t)BOARD_ENC_B_PIN);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "init_pcnt_encoder failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // 2. Initialize GPIO 17 (SW), GPIO 18 (Btn 1), and GPIO 10 (Btn 2) with pull-ups
    gpio_config_t btn_cfg = {
        .pin_bit_mask = (1ULL << BOARD_ENC_SW_PIN) | (1ULL << BOARD_BTN_1_PIN) | (1ULL << BOARD_BTN_2_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,     // Hardware 10k on 17/18/10 (R11, R8, R12 in Rev 1.1) + internal backup
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ret = gpio_config(&btn_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure button GPIOs: %s", esp_err_to_name(ret));
        return ret;
    }

    s_running = true;
    // Core 0, priority 3, stack 3072 bytes (Invariant 9: UI & Controls on Core 0)
    BaseType_t task_ret = xTaskCreatePinnedToCore(controls_task, "controls_task", 3072, NULL, 3, &s_controls_task_handle, 0);
    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create controls_task!");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Hardware controls initialized successfully on Core 0.");
    return ESP_OK;
}

void controls_deinit(void)
{
    s_running = false;
    if (s_pcnt_unit) {
        pcnt_unit_stop(s_pcnt_unit);
        pcnt_unit_disable(s_pcnt_unit);
        pcnt_del_unit(s_pcnt_unit);
        s_pcnt_unit = NULL;
    }
    if (s_ctrl_queue) {
        vQueueDelete(s_ctrl_queue);
        s_ctrl_queue = NULL;
    }
}
