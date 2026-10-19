# Module 06: UI Display & Hardware Controls Function Specifications

> **Module Identifier:** `UI_DISPLAY_CONTROLS`  
> **Primary References:** [`ui_and_controls_guide.md`](../guides/ui_and_controls_guide.md)  
> **ESP-IDF Components:** `esp_driver_i2c`, `esp_driver_pcnt`, `esp_driver_gpio`  
> **Hardware Displays:** 0.96" or 1.3" I2C Monochrome OLED (`SSD1306` / `SH1106`, Address: `0x3C`)  
> **Hardware Inputs:** EC11 Rotary Encoder (`GPIO 15, 16, 17`), Mode Switch (`GPIO 18`), or 3 Preset Buttons (`GPIO 15, 16, 17`)

---

## 1. Architectural Overview

This module manages the visual user interface and physical user controls:
1. **I2C OLED Graphics Engine (`0x3C`):** Renders playback status, track metadata, bit depth/rate badges, volume progress bars, and real-time 5-band PEQ frequency response curves.
2. **Hardware PCNT Rotary Encoder (`SW7` / `SW5`):** Uses the ESP32-S3 Pulse Counter (PCNT) peripheral with hardware glitch filters to provide 100% bounce-free rotary volume adjustment.
3. **Discrete Switch Debouncing:** Event-driven FreeRTOS task handling short-press, long-press, and multi-button combo actions for presets.

---

## 2. Function Specifications

### 2.1 `ui_display_init`

Initializes the I2C master bus on `GPIO 8 (SDA)` and `GPIO 9 (SCL)`, sends the SSD1306 initialization sequence, configures charge pump power, and clears the internal framebuffer.

```c
esp_err_t ui_display_init(void);
```

#### Production Reference Implementation
```c
#include "driver/i2c_master.h"

#define I2C_PORT_NUM            I2C_NUM_0
#define I2C_OLED_ADDR           0x3C // Standard 4-pin module address

static i2c_master_bus_handle_t s_i2c_bus = NULL;
static i2c_master_dev_handle_t s_oled_dev = NULL;

esp_err_t ui_display_init(void)
{
    ESP_LOGI("OLED", "Initializing I2C Master on SDA=%d, SCL=%d (Addr: 0x%02X)...",
             BOARD_I2C_SDA_PIN, BOARD_I2C_SCL_PIN, I2C_OLED_ADDR);

    i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_PORT_NUM,
        .sda_io_num = (gpio_num_t)BOARD_I2C_SDA_PIN,
        .scl_io_num = (gpio_num_t)BOARD_I2C_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t ret = i2c_new_master_bus(&bus_config, &s_i2c_bus);
    if (ret != ESP_OK) return ret;

    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = I2C_OLED_ADDR,
        .scl_speed_hz = 400000, // 400 kHz Fast Mode
    };
    ret = i2c_master_bus_add_device(s_i2c_bus, &dev_config, &s_oled_dev);
    if (ret != ESP_OK) return ret;

    // SSD1306 Display Initialization Command Sequence
    const uint8_t init_cmds[] = {
        0x00, // Control byte: Command stream
        0xAE, // Display OFF
        0xD5, 0x80, // Set display clock divide ratio/oscillator
        0xA8, 0x3F, // Set multiplex ratio (64 lines)
        0xD3, 0x00, // Set display offset (0)
        0x40,       // Set start line (0)
        0x8D, 0x14, // Enable charge pump regulator
        0x20, 0x00, // Set horizontal addressing mode
        0xA1,       // Set segment re-map (A0/A1)
        0xC8,       // Set COM output scan direction
        0xDA, 0x12, // Set COM pins hardware config
        0x81, 0xCF, // Set contrast control (0xCF)
        0xD9, 0xF1, // Set pre-charge period
        0xDB, 0x40, // Set VCOMH deselect level
        0xA4,       // Entire display ON from RAM
        0xA6,       // Normal display (non-inverted)
        0xAF        // Display ON
    };
    ret = i2c_master_transmit(s_oled_dev, init_cmds, sizeof(init_cmds), 100);
    ESP_LOGI("OLED", "SSD1306 initialized. Status: %s", esp_err_to_name(ret));
    return ret;
}
```

---

### 2.2 `encoder_pcnt_init`

Configures the ESP32-S3 hardware Pulse Counter (PCNT) peripheral in quadrature decoding mode (x4 resolution) with hardware glitch filtering on the rotary encoder phase pins.

```c
esp_err_t encoder_pcnt_init(gpio_num_t pin_a, gpio_num_t pin_b);
```

#### Parameters
| Parameter | Direction | Type | Description |
| :--- | :---: | :---: | :--- |
| `pin_a` | `[in]` | `gpio_num_t` | Encoder Phase A pin (`GPIO 15`). |
| `pin_b` | `[in]` | `gpio_num_t` | Encoder Phase B pin (`GPIO 16`). |

#### Production Reference Implementation
```c
#include "driver/pulse_cnt.h"

static pcnt_unit_handle_t s_pcnt_unit = NULL;

esp_err_t encoder_pcnt_init(gpio_num_t pin_a, gpio_num_t pin_b)
{
    ESP_LOGI("ENCODER", "Configuring hardware PCNT quadrature unit on A=%d, B=%d", pin_a, pin_b);

    pcnt_unit_config_t unit_config = {
        .high_limit = 100,
        .low_limit = -100,
    };
    esp_err_t ret = pcnt_new_unit(&unit_config, &s_pcnt_unit);
    if (ret != ESP_OK) return ret;

    // Glitch filter: Ignore electrical pulses shorter than 1000 ns (contact bounce suppression)
    pcnt_glitch_filter_config_t filter_config = {
        .max_glitch_ns = 1000,
    };
    pcnt_unit_set_glitch_filter(s_pcnt_unit, &filter_config);

    // Channel A configuration
    pcnt_chan_config_t chan_a_config = {
        .edge_gpio_num = pin_a,
        .level_gpio_num = pin_b,
    };
    pcnt_channel_handle_t pcnt_chan_a = NULL;
    pcnt_new_channel(s_pcnt_unit, &chan_a_config, &pcnt_chan_a);
    pcnt_channel_set_edge_action(pcnt_chan_a, PCNT_CHANNEL_EDGE_ACTION_DECREASE, PCNT_CHANNEL_EDGE_ACTION_INCREASE);
    pcnt_channel_set_level_action(pcnt_chan_a, PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_INVERSE);

    // Channel B configuration
    pcnt_chan_config_t chan_b_config = {
        .edge_gpio_num = pin_b,
        .level_gpio_num = pin_a,
    };
    pcnt_channel_handle_t pcnt_chan_b = NULL;
    pcnt_new_channel(s_pcnt_unit, &chan_b_config, &pcnt_chan_b);
    pcnt_channel_set_edge_action(pcnt_chan_b, PCNT_CHANNEL_EDGE_ACTION_INCREASE, PCNT_CHANNEL_EDGE_ACTION_DECREASE);
    pcnt_channel_set_level_action(pcnt_chan_b, PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_INVERSE);

    pcnt_unit_enable(s_pcnt_unit);
    pcnt_unit_clear_count(s_pcnt_unit);
    pcnt_unit_start(s_pcnt_unit);
    return ESP_OK;
}
```

---

### 2.3 `encoder_read_delta`

Polls the hardware PCNT counter, computes relative rotation change since the previous call, and clears the counter accumulator.

```c
int encoder_read_delta(void);
```

#### Production Reference Implementation
```c
int encoder_read_delta(void)
{
    if (!s_pcnt_unit) return 0;
    int count = 0;
    pcnt_unit_get_count(s_pcnt_unit, &count);
    if (count != 0) {
        pcnt_unit_clear_count(s_pcnt_unit);
        // Each detent produces 4 edge counts in x4 quadrature mode
        return count / 4;
    }
    return 0;
}
```
