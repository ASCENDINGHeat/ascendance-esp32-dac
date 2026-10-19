#include "audio_pipeline.h"
#include "board_pins.h"
#include "led_indicator.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"

#define TAG "AUDIO_PIPELINE"

static i2s_chan_handle_t s_tx_chan = NULL;
static int s_current_sample_rate = 44100;
static int s_master_volume = 80;       // Default master volume: 80%
static bool s_is_muted = false;

// Pre-calculated volume linear multiplier (0.0f to 1.0f)
static float s_volume_factor = 0.80f;

static inline int16_t clamp_s16(float v)
{
    if (v > 32767.0f) return 32767;
    if (v < -32768.0f) return -32768;
    return (int16_t)roundf(v);
}

void audio_pipeline_set_mute(bool mute)
{
    s_is_muted = mute;
    // Active-LOW soft mute on PCM5102A:
    // 0 = Mute asserted (analog outputs decay to 0V)
    // 1 = Normal playback enabled
    gpio_set_level((gpio_num_t)BOARD_DAC_MUTE_PIN, mute ? 0 : 1);
    ESP_LOGI(TAG, "Hardware DAC mute (GPIO %d) set to %s",
             BOARD_DAC_MUTE_PIN, mute ? "MUTED (LOW)" : "UNMUTED (HIGH)");

    // Update onboard WS2812B aRGB LED (displays soft Red alert when muted)
    led_indicator_set_volume(s_master_volume, s_is_muted);
}

bool audio_pipeline_is_muted(void)
{
    return s_is_muted;
}

esp_err_t audio_pipeline_init(int sample_rate)
{
    if (s_tx_chan != NULL) {
        ESP_LOGW(TAG, "I2S channel already initialized.");
        return ESP_OK;
    }

    // Initialize onboard WS2812B aRGB LED loudness indicator (GPIO 48)
    led_indicator_init();

    ESP_LOGI(TAG, "Initializing PCM5102A hardware mute on GPIO %d...", BOARD_DAC_MUTE_PIN);
    gpio_config_t mute_cfg = {
        .pin_bit_mask = (1ULL << BOARD_DAC_MUTE_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t ret = gpio_config(&mute_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure mute GPIO: %s", esp_err_to_name(ret));
        return ret;
    }
    // Start in MUTED state to prevent boot pop
    audio_pipeline_set_mute(true);

    ESP_LOGI(TAG, "Initializing I2S0: WS=%d, DOUT=%d, BCK=%d at %d Hz",
             BOARD_I2S_WS_PIN, BOARD_I2S_DATA_OUT_PIN, BOARD_I2S_BCK_PIN, sample_rate);

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;     // Auto-clear DMA buffer on underrun to prevent buzz
    chan_cfg.dma_desc_num = 8;      // 8 descriptors
    chan_cfg.dma_frame_num = 256;   // 256 stereo frames per descriptor (~46ms buffer)

    ret = i2s_new_channel(&chan_cfg, &s_tx_chan, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to allocate I2S channel: %s", esp_err_to_name(ret));
        return ret;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = (gpio_num_t)BOARD_I2S_BCK_PIN,
            .ws   = (gpio_num_t)BOARD_I2S_WS_PIN,
            .dout = (gpio_num_t)BOARD_I2S_DATA_OUT_PIN,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };

    ret = i2s_channel_init_std_mode(s_tx_chan, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init standard I2S mode: %s", esp_err_to_name(ret));
        i2s_del_channel(s_tx_chan);
        s_tx_chan = NULL;
        return ret;
    }

    ret = i2s_channel_enable(s_tx_chan);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable I2S channel: %s", esp_err_to_name(ret));
        i2s_del_channel(s_tx_chan);
        s_tx_chan = NULL;
        return ret;
    }

    s_current_sample_rate = sample_rate;
    audio_pipeline_set_volume(s_master_volume);

    // Release mute with 15ms settling time
    vTaskDelay(pdMS_TO_TICKS(15));
    audio_pipeline_set_mute(false);

    return ESP_OK;
}

void audio_pipeline_set_sample_rate(int new_rate)
{
    if (new_rate == s_current_sample_rate || new_rate <= 0 || s_tx_chan == NULL) {
        return;
    }

    ESP_LOGI(TAG, "Reconfiguring I2S sample rate: %d Hz -> %d Hz", s_current_sample_rate, new_rate);

    // Invariant 6: Must disable channel before clock reconfig
    i2s_channel_disable(s_tx_chan);
    i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(new_rate);
    esp_err_t ret = i2s_channel_reconfig_std_clock(s_tx_chan, &clk_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to reconfigure I2S clock to %d Hz: %s", new_rate, esp_err_to_name(ret));
    }
    i2s_channel_enable(s_tx_chan);
    s_current_sample_rate = new_rate;
}

int audio_pipeline_get_sample_rate(void)
{
    return s_current_sample_rate;
}

void audio_pipeline_set_volume(int volume_percent)
{
    if (volume_percent < 0) volume_percent = 0;
    if (volume_percent > 100) volume_percent = 100;
    s_master_volume = volume_percent;

    // Linear mapping with logarithmic feel or direct linear ratio
    s_volume_factor = (float)s_master_volume / 100.0f;
    ESP_LOGI(TAG, "Master volume set to %d%% (factor: %.2f)", s_master_volume, s_volume_factor);

    // Update onboard WS2812B aRGB LED brightness and color
    led_indicator_set_volume(s_master_volume, s_is_muted);
}

int audio_pipeline_get_volume(void)
{
    return s_master_volume;
}

int audio_pipeline_adjust_volume(int delta)
{
    int new_vol = s_master_volume + delta;
    audio_pipeline_set_volume(new_vol);
    return s_master_volume;
}

void audio_pipeline_write(const void *data, size_t size_bytes)
{
    if (!s_tx_chan || !data || size_bytes == 0) {
        return;
    }

    // If muted or volume is zero, write silence to keep DMA happy
    if (s_is_muted || s_master_volume == 0) {
        static const int16_t silence[256 * 2] = {0};
        size_t bytes_to_clear = size_bytes;
        size_t written = 0;
        while (bytes_to_clear > 0) {
            size_t chunk = (bytes_to_clear > sizeof(silence)) ? sizeof(silence) : bytes_to_clear;
            i2s_channel_write(s_tx_chan, silence, chunk, &written, portMAX_DELAY);
            bytes_to_clear -= written;
        }
        return;
    }

    // If at 100% volume, bypass software scaling for maximum efficiency
    if (s_master_volume >= 100) {
        size_t written = 0;
        i2s_channel_write(s_tx_chan, data, size_bytes, &written, portMAX_DELAY);
        return;
    }

    // Apply software volume scaling with saturation clamping
    const int16_t *src = (const int16_t *)data;
    size_t num_samples = size_bytes / sizeof(int16_t);
    int16_t scaled_buf[256 * 2]; // 256 stereo pairs on stack
    const size_t chunk_samples = sizeof(scaled_buf) / sizeof(int16_t);

    size_t samples_left = num_samples;
    size_t src_idx = 0;

    while (samples_left > 0) {
        size_t chunk = (samples_left > chunk_samples) ? chunk_samples : samples_left;
        for (size_t i = 0; i < chunk; i++) {
            float val = (float)src[src_idx++] * s_volume_factor;
            scaled_buf[i] = clamp_s16(val);
        }
        size_t written = 0;
        i2s_channel_write(s_tx_chan, scaled_buf, chunk * sizeof(int16_t), &written, portMAX_DELAY);
        samples_left -= chunk;
    }
}

void audio_pipeline_play_tone(float freq_hz, int duration_ms, int sample_rate)
{
    int total_samples = (sample_rate * duration_ms) / 1000;
    int16_t buffer[256 * 2]; // 256 stereo pairs
    const int buffer_pairs = 256;
    float phase = 0.0f;
    const float phase_step = (2.0f * (float)M_PI * freq_hz) / (float)sample_rate;

    int samples_left = total_samples;
    while (samples_left > 0) {
        int chunk = (samples_left > buffer_pairs) ? buffer_pairs : samples_left;
        for (int i = 0; i < chunk; i++) {
            float amp = 12000.0f; // ~-8 dBFS safe peak
            float sample_val = sinf(phase) * amp;
            int16_t s16 = clamp_s16(sample_val);
            buffer[2 * i]     = s16; // Left
            buffer[2 * i + 1] = s16; // Right
            phase += phase_step;
            if (phase >= 2.0f * (float)M_PI) {
                phase -= 2.0f * (float)M_PI;
            }
        }
        audio_pipeline_write(buffer, chunk * 2 * sizeof(int16_t));
        samples_left -= chunk;
    }
}

void audio_pipeline_play_startup_chime(void)
{
    ESP_LOGI(TAG, "Playing startup verification chime to verify PCM5102A DAC output...");
    audio_pipeline_set_mute(false);
    led_indicator_set_rgb(0, 10, 20);
    audio_pipeline_play_tone(523.25f, 150, s_current_sample_rate); // C5
    led_indicator_set_rgb(0, 25, 35);
    audio_pipeline_play_tone(659.25f, 150, s_current_sample_rate); // E5
    led_indicator_set_rgb(0, 40, 50);
    audio_pipeline_play_tone(783.99f, 150, s_current_sample_rate); // G5
    led_indicator_set_rgb(15, 60, 60);
    audio_pipeline_play_tone(1046.50f, 350, s_current_sample_rate); // C6
    vTaskDelay(pdMS_TO_TICKS(100)); // Allow analog charge dissipation
    led_indicator_set_volume(s_master_volume, s_is_muted);
}

esp_err_t audio_pipeline_deinit(void)
{
    if (s_tx_chan == NULL) {
        return ESP_OK;
    }
    ESP_LOGI(TAG, "Deinitializing I2S audio pipeline...");
    audio_pipeline_set_mute(true);
    led_indicator_clear();
    i2s_channel_disable(s_tx_chan);
    esp_err_t ret = i2s_del_channel(s_tx_chan);
    s_tx_chan = NULL;
    s_current_sample_rate = 0;
    return ret;
}
