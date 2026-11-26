#include "audio_source_arbiter.h"
#include "audio_pipeline.h"
#include "oled_display.h"
#include "peq_dsp.h"
#include "board_pins.h"

#include <stdio.h>
#include <string.h>
#include <stdatomic.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"

#define TAG "ARBITER"

static _Atomic audio_source_t s_current_source = AUDIO_SOURCE_STANDALONE;
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

    atomic_store_explicit(&s_current_source, initial_source, memory_order_release);
    ESP_LOGI(TAG, "Audio Source Arbiter initialized. Boot source: %s", source_names[initial_source]);
    return ESP_OK;
}

audio_source_t audio_source_arbiter_get_current(char *out_name)
{
    audio_source_t cur = atomic_load_explicit(&s_current_source, memory_order_acquire);
    if (out_name) {
        snprintf(out_name, 20, "%s", source_names[cur]);
    }
    return cur;
}

esp_err_t audio_source_arbiter_switch_to(audio_source_t new_source)
{
    if (new_source >= AUDIO_SOURCE_COUNT) return ESP_ERR_INVALID_ARG;
    if (!s_arbiter_mutex) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_arbiter_mutex, portMAX_DELAY);
    audio_source_t old_source = atomic_load_explicit(&s_current_source, memory_order_acquire);
    if (new_source == old_source) {
        xSemaphoreGive(s_arbiter_mutex);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Executing 4-Phase Zero-Pop Handover: [%s] -> [%s]",
             source_names[old_source], source_names[new_source]);

    // Phase 1: Hardware Soft-Mute (XMT LOW)
    // Allows PCM5102A analog charge pumps to smoothly decay to 0V (15 ms)
    audio_pipeline_set_mute(true);
    vTaskDelay(pdMS_TO_TICKS(15));

    // Phase 2: Atomic Source Switch (Core 1 audio_dsp_task exclusively owns I2S TX writes)
    atomic_store_explicit(&s_current_source, new_source, memory_order_release);

    // Phase 3: Allow Core 1 DMA pipeline to flush with muted silence (20 ms)
    vTaskDelay(pdMS_TO_TICKS(20));

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
    snprintf(uistate.title, sizeof(uistate.title), "ASCENDANCE Hi-Fi");
    snprintf(uistate.subtitle, sizeof(uistate.subtitle), "%s", source_names[new_source]);
    snprintf(uistate.eq_preset, sizeof(uistate.eq_preset), "%.15s", cur_eq);
    uistate.volume = audio_pipeline_get_volume();
    uistate.is_playing = true;
    uistate.is_muted = false;
    oled_display_set_state(&uistate);

    // Phase 4: Unmute with 1 ms digital wake ramp
    peq_dsp_trigger_wake_ramp();
    audio_pipeline_set_mute(false);

    ESP_LOGI(TAG, "Source handover complete. Active: %s (old: %s)",
             source_names[new_source], source_names[old_source]);
    xSemaphoreGive(s_arbiter_mutex);
    return ESP_OK;
}

audio_source_t audio_source_arbiter_cycle(void)
{
    audio_source_t cur = atomic_load_explicit(&s_current_source, memory_order_acquire);
    audio_source_t next = (cur + 1) % AUDIO_SOURCE_COUNT;
    audio_source_arbiter_switch_to(next);
    return next;
}
