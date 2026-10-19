#include "led_indicator.h"
#include "board_pins.h"

#include <string.h>
#include "esp_log.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"

#define TAG "LED_INDICATOR"

// RMT Channel & Encoder handles for WS2812B
static rmt_channel_handle_t s_led_chan = NULL;
static rmt_encoder_handle_t s_bytes_encoder = NULL;
static bool s_initialized = false;

// Optical Safety Maximum Brightness:
// WS2812B at 100% duty cycle draws up to 60 mA and produces uncomfortable glare.
// As defined in docs/hardware/hardware.md Section 3.5, maximum software brightness
// is capped at ~25% duty cycle (60 out of 255) for optimal eye comfort and low power draw.
#define LED_MAX_BRIGHTNESS 60

esp_err_t led_indicator_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing onboard WS2812B LED indicator on GPIO %d via RMT...", BOARD_RGB_LED_PIN);

    // Configure RMT TX Channel
    rmt_tx_channel_config_t tx_chan_config = {
        .gpio_num = (gpio_num_t)BOARD_RGB_LED_PIN,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000, // 10 MHz resolution (1 tick = 100 ns)
        .mem_block_symbols = 64,           // 64 symbols buffer (24 bits needed for 1 LED)
        .trans_queue_depth = 4,
    };

    esp_err_t ret = rmt_new_tx_channel(&tx_chan_config, &s_led_chan);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create RMT TX channel on GPIO %d: %s", BOARD_RGB_LED_PIN, esp_err_to_name(ret));
        return ret;
    }

    // Configure WS2812B Non-Return-to-Zero (NRZ) Timing:
    // Resolution = 10 MHz -> 1 tick = 0.1 us (100 ns)
    // Bit 0: ~300 ns HIGH, ~900 ns LOW (Total 1.20 us)
    // Bit 1: ~900 ns HIGH, ~300 ns LOW (Total 1.20 us)
    rmt_bytes_encoder_config_t bytes_encoder_config = {
        .bit0 = {
            .duration0 = 3, // 300 ns
            .level0 = 1,
            .duration1 = 9, // 900 ns
            .level1 = 0,
        },
        .bit1 = {
            .duration0 = 9, // 900 ns
            .level0 = 1,
            .duration1 = 3, // 300 ns
            .level1 = 0,
        },
        .flags = {
            .msb_first = 1, // WS2812B transmits MSB first
        },
    };

    ret = rmt_new_bytes_encoder(&bytes_encoder_config, &s_bytes_encoder);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create RMT bytes encoder: %s", esp_err_to_name(ret));
        rmt_del_channel(s_led_chan);
        s_led_chan = NULL;
        return ret;
    }

    ret = rmt_enable(s_led_chan);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable RMT channel: %s", esp_err_to_name(ret));
        rmt_del_encoder(s_bytes_encoder);
        rmt_del_channel(s_led_chan);
        s_bytes_encoder = NULL;
        s_led_chan = NULL;
        return ret;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "WS2812B LED indicator initialized successfully.");

    // Initial state: subtle cyan glow at default volume
    led_indicator_clear();

    return ESP_OK;
}

void led_indicator_set_rgb(uint8_t red, uint8_t green, uint8_t blue)
{
    if (!s_initialized || s_led_chan == NULL || s_bytes_encoder == NULL) {
        return;
    }

    // WS2812B color order is Green - Red - Blue (GRB)
    uint8_t grb[3] = {green, red, blue};

    rmt_transmit_config_t tx_config = {
        .loop_count = 0, // Transmit once
        .flags = {
            .eot_level = 0, // Keep line LOW after transmission (WS2812B reset code)
        },
    };

    esp_err_t ret = rmt_transmit(s_led_chan, s_bytes_encoder, grb, sizeof(grb), &tx_config);
    if (ret == ESP_OK) {
        // Wait for 3-byte transmission to finish (takes ~30 us)
        rmt_tx_wait_all_done(s_led_chan, 50);
    }
}

void led_indicator_set_volume(int volume_percent, bool is_muted)
{
    if (!s_initialized) {
        return;
    }

    if (volume_percent < 0) volume_percent = 0;
    if (volume_percent > 100) volume_percent = 100;

    // 1. Mute Indicator: Clear, non-intrusive soft red alert
    if (is_muted) {
        led_indicator_set_rgb(25, 0, 0); // Gentle red glow
        return;
    }

    // 2. Zero Volume: Completely dark/quiescent
    if (volume_percent == 0) {
        led_indicator_set_rgb(0, 0, 0);
        return;
    }

    // 3. Brightness Scaling:
    // Scales smoothly from 1 (subtle whisper glow) up to LED_MAX_BRIGHTNESS (60, ~25% duty)
    uint32_t brightness = 1 + ((uint32_t)volume_percent * (LED_MAX_BRIGHTNESS - 1)) / 100;

    // 4. Perceptual Hi-Fi Audio Color Spectrum:
    // - Low Volume (1% - 35%): Deep Blue / Teal
    // - Mid Volume (36% - 75%): Pure Vibrant Cyan
    // - High Volume (76% - 100%): Electric Cyan to Crisp White-Mint
    uint8_t r = 0, g = 0, b = 0;

    if (volume_percent <= 35) {
        // Deep Blue to Cyan
        uint32_t g_val = (brightness * (uint32_t)volume_percent) / 35;
        r = 0;
        g = (uint8_t)g_val;
        b = (uint8_t)brightness;
    } else if (volume_percent <= 75) {
        // Pure Cyan
        r = 0;
        g = (uint8_t)brightness;
        b = (uint8_t)brightness;
    } else {
        // High Loudness: Add Red component for a brighter, high-energy Mint/White-Cyan
        uint32_t r_val = (brightness * (uint32_t)(volume_percent - 75)) / (100 - 75) / 3;
        r = (uint8_t)r_val;
        g = (uint8_t)brightness;
        b = (uint8_t)((brightness * 4) / 5);
    }

    led_indicator_set_rgb(r, g, b);
}

void led_indicator_clear(void)
{
    led_indicator_set_rgb(0, 0, 0);
}
