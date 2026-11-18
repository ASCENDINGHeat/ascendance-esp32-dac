#include "audio_source_arbiter.h"
#include "audio_pipeline.h"
#include "oled_display.h"
#include "peq_dsp.h"
#include "board_pins.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"

#define TAG "ARBITER"

static audio_source_t s_current_source = AUDIO_SOURCE_STANDALONE;
static SemaphoreHandle_t s_arbiter_mutex = NULL;

static const char *source_names[AUDIO_SOURCE_COUNT] = {
    [AUDIO_SOURCE_STANDALONE] = "USB DAC / DSP",
    [AUDIO_SOURCE_TONE_TEST]  = "Tone Generator",
    [AUDIO_SOURCE_SWEEP]      = "Frequency Sweep"
};

esp_err_t audio_source_arbiter_init(audio_source_t initial_source)
{
    s_arbiter_mutex = xSemaphoreCreateMutex();
    if (!s_arbiter_mutex) {
        return ESP_ERR_NO_MEM;
    }

    s_current_source = initial_source;
    ESP_LOGI(TAG, "Audio Source Arbiter initialized. Boot source: %s", source_names[s_current_source]);
    return ESP_OK;
}

audio_source_t audio_source_arbiter_get_current(char *out_name)
{
    if (out_name) {
        snprintf(out_name, 20, "%s", source_names[s_current_source]);
    }
    return s_current_source;
}

esp_err_t audio_source_arbiter_switch_to(audio_source_t new_source)
{
    if (new_source >= AUDIO_SOURCE_COUNT) return ESP_ERR_INVALID_ARG;
    if (!s_arbiter_mutex) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_arbiter_mutex, portMAX_DELAY);
    if (new_source == s_current_source) {
        xSemaphoreGive(s_arbiter_mutex);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Executing 4-Phase Zero-Pop Handover: [%s] -> [%s]",
             source_names[s_current_source], source_names[new_source]);

    // Phase 1: Hardware Soft-Mute (XMT LOW)
    // Allows PCM5102A analog charge pumps to smoothly decay to 0V (15 ms)
    audio_pipeline_set_mute(true);
    vTaskDelay(pdMS_TO_TICKS(15));

    // Phase 2: Pipeline Teardown
    audio_source_t old_source = s_current_source;
    s_current_source = new_source;

    // Phase 3: Flush DMA with Silence & Reclock (10 ms)
    int16_t silence[256 * 2] = {0};
    for (int i = 0; i < 4; i++) {
        audio_pipeline_write(silence, sizeof(silence));
    }
    vTaskDelay(pdMS_TO_TICKS(10));

    // Update OLED Display
    char cur_eq[24] = "Flat";
    peq_dsp_get_current_preset(cur_eq);

    int cur_sr = audio_pipeline_get_sample_rate();
    if (cur_sr <= 0) cur_sr = 48000;

    ui_display_state_t uistate;
    memset(&uistate, 0, sizeof(uistate));
    snprintf(uistate.mode_str, sizeof(uistate.mode_str), "%s",
             (new_source == AUDIO_SOURCE_STANDALONE) ? "USB DAC" :
             ((new_source == AUDIO_SOURCE_TONE_TEST) ? "Synth" : "Sweep"));
    snprintf(uistate.sample_rate_str, sizeof(uistate.sample_rate_str), "%.1fk 24b", (float)cur_sr / 1000.0f);
    snprintf(uistate.title, sizeof(uistate.title), "ESP32-S3 Audio DSP");
    snprintf(uistate.subtitle, sizeof(uistate.subtitle), "%s", source_names[new_source]);
    snprintf(uistate.eq_preset, sizeof(uistate.eq_preset), "%.15s", cur_eq);
    uistate.volume = audio_pipeline_get_volume();
    uistate.is_playing = true;
    uistate.is_muted = false;
    oled_display_set_state(&uistate);

    // Phase 4: Spin up New Source & Unmute (15 ms)
    vTaskDelay(pdMS_TO_TICKS(15));
    audio_pipeline_set_mute(false);

    if (new_source == AUDIO_SOURCE_TONE_TEST) {
        // Play brief ascending arpeggio to audition active EQ profile
        audio_pipeline_play_tone(440.0f, 150, cur_sr);
        audio_pipeline_play_tone(554.37f, 150, cur_sr);
        audio_pipeline_play_tone(659.25f, 250, cur_sr);
    } else if (new_source == AUDIO_SOURCE_SWEEP) {
        // Play a short 100Hz to 1kHz diagnostic sweep
        for (float f = 100.0f; f <= 1000.0f; f += 100.0f) {
            audio_pipeline_play_tone(f, 50, cur_sr);
        }
    }

    ESP_LOGI(TAG, "Source handover complete. Active: %s (old: %s)",
             source_names[new_source], source_names[old_source]);
    xSemaphoreGive(s_arbiter_mutex);
    return ESP_OK;
}

audio_source_t audio_source_arbiter_cycle(void)
{
    audio_source_t next = (s_current_source + 1) % AUDIO_SOURCE_COUNT;
    audio_source_arbiter_switch_to(next);
    return next;
}
