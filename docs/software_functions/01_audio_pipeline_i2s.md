# Module 01: Audio Pipeline & I2S Driver Function Specifications

> **Module Identifier:** `AUDIO_PIPELINE_I2S`  
> **Source Files:** [`main/main.c`](../../main/main.c), [`main/board_pins.h`](../../main/board_pins.h)  
> **ESP-IDF Component:** `esp_driver_i2s` (`driver/i2s_std.h`, `driver/gpio.h`)  
> **Target Peripheral:** ESP32-S3 I2S0 Peripheral (Master Transmit Mode to PCM5102A DAC)

---

## 1. Architectural Overview

The I2S audio output driver configures the ESP32-S3 hardware I2S0 peripheral in **Master Standard Philips Mode**, delivering continuous stereo 16-bit or 32-bit linear PCM bitstreams to the external **GY-PCM5102A 32-bit Audio DAC**. The pipeline utilizes Direct Memory Access (DMA) ringbuffers to ensure uninterrupted playback during Wi-Fi bursts, SD card accesses, and UI updates.

### Hardware Pin Mapping (`board_pins.h`)
* `GPIO 4`: Word Select / Frame Clock (`WS` / `LCK`)
* `GPIO 5`: Serial Audio Data Out (`DOUT` / `DIN`)
* `GPIO 6`: Continuous Bit Clock (`BCK`)
* `GPIO 7`: Hardware Soft Mute (`XMT` / `XMST`, Active-LOW)

---

## 2. Function Specifications

### 2.1 `i2s_output_init`

Initializes the I2S0 transmitter channel in standard Philips stereo mode, allocates DMA frame descriptors, binds the hardware GPIO pins, and starts the I2S clock engine.

```c
esp_err_t i2s_output_init(int sample_rate);
```

#### Parameters
| Parameter | Direction | Type | Nullable | Valid Range | Description |
| :--- | :---: | :---: | :---: | :--- | :--- |
| `sample_rate` | `[in]` | `int` | No | `8000` to `384000` Hz (typically `44100`, `48000`, `96000`, `192000`) | Target audio sampling rate in Hertz. |

#### Return Value
* `ESP_OK`: I2S channel successfully initialized and enabled.
* `ESP_ERR_INVALID_ARG`: Invalid sample rate or invalid pin mapping.
* `ESP_ERR_NO_MEM`: Insufficient internal DRAM for DMA descriptors or channel structure.
* `ESP_ERR_INVALID_STATE`: I2S channel already active or resource busy.

#### Agentic AI Execution Context
* **Calling Context:** FreeRTOS Task (typically during `app_main` or `audio_manager_init`).
* **Core Affinity:** Core 0 or Core 1.
* **Stack Requirement:** $\ge 2048$ bytes.
* **DMA Allocation:** Allocated strictly from internal SRAM (`MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL`).

#### Pre-conditions & Invariants
* The global static channel handle `s_tx_chan` must be `NULL` before initialization.
* Solder jumper `IN-OUT` on the YD-ESP32-S3 must be bridged to provide 5V power to the DAC rail.

#### Production Reference Implementation
```c
static i2s_chan_handle_t s_tx_chan = NULL;
static int s_current_sample_rate = 44100;

esp_err_t i2s_output_init(int sample_rate)
{
    if (s_tx_chan != NULL) {
        ESP_LOGW("I2S", "I2S channel already initialized. Skipping.");
        return ESP_OK;
    }

    ESP_LOGI("I2S", "Initializing I2S on LCK=%d, DIN=%d, BCK=%d at %d Hz",
             BOARD_I2S_WS_PIN, BOARD_I2S_DATA_OUT_PIN, BOARD_I2S_BCK_PIN, sample_rate);

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;         // Auto zero-fill DMA buffer on underrun (prevents buzzing)
    chan_cfg.dma_desc_num = 8;          // 8 DMA descriptors
    chan_cfg.dma_frame_num = 256;       // 256 stereo frames per descriptor (512 samples)

    esp_err_t ret = i2s_new_channel(&chan_cfg, &s_tx_chan, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE("I2S", "Failed to allocate I2S channel: %s", esp_err_to_name(ret));
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
        ESP_LOGE("I2S", "Failed to init standard I2S mode: %s", esp_err_to_name(ret));
        i2s_del_channel(s_tx_chan);
        s_tx_chan = NULL;
        return ret;
    }

    ret = i2s_channel_enable(s_tx_chan);
    if (ret == ESP_OK) {
        s_current_sample_rate = sample_rate;
    }
    return ret;
}
```

---

### 2.2 `i2s_output_set_sample_rate`

Dynamically reconfigures the master bit clock and word select clock frequencies when the audio stream sample rate changes (e.g., transitions between 44.1 kHz, 48 kHz, 96 kHz).

```c
void i2s_output_set_sample_rate(int new_rate);
```

#### Parameters
| Parameter | Direction | Type | Nullable | Valid Range | Description |
| :--- | :---: | :---: | :---: | :--- | :--- |
| `new_rate` | `[in]` | `int` | No | `8000` to `384000` Hz | New sampling rate to configure. |

#### Return Value
* `void`: If `new_rate` equals `s_current_sample_rate` or is $\le 0$, the function performs a no-op return.

#### Critical Invariant (ESP-IDF v6.1 Assertion Prevention)
> [!IMPORTANT]
> The active I2S channel MUST be disabled (`i2s_channel_disable`) before calling `i2s_channel_reconfig_std_clock`. Attempting to reclock a live DMA channel triggers an `ESP_ERR_INVALID_STATE` assertion panic!

#### Production Reference Implementation
```c
void i2s_output_set_sample_rate(int new_rate)
{
    if (new_rate == s_current_sample_rate || new_rate <= 0 || s_tx_chan == NULL) {
        return;
    }

    ESP_LOGI("I2S", "Reconfiguring I2S sample rate: %d Hz -> %d Hz", s_current_sample_rate, new_rate);

    // Step 1: Must disable channel before clock reconfig
    i2s_channel_disable(s_tx_chan);

    // Step 2: Recalculate standard clock dividers
    i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(new_rate);
    esp_err_t ret = i2s_channel_reconfig_std_clock(s_tx_chan, &clk_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE("I2S", "Failed to reconfig clock to %d Hz: %s", new_rate, esp_err_to_name(ret));
    }

    // Step 3: Re-enable channel
    i2s_channel_enable(s_tx_chan);
    s_current_sample_rate = new_rate;
}
```

---

### 2.3 `i2s_output_write`

Pushes a buffer of interleaved stereo PCM audio samples into the I2S DMA ringbuffer. Blocks the calling FreeRTOS task if DMA descriptors are currently saturated.

```c
void i2s_output_write(const void *data, size_t size);
```

#### Parameters
| Parameter | Direction | Type | Nullable | Valid Range | Description |
| :--- | :---: | :---: | :---: | :--- | :--- |
| `data` | `[in]` | `const void *` | No | Valid memory pointer | Pointer to buffer containing 16-bit stereo PCM audio (`L, R, L, R...`). |
| `size` | `[in]` | `size_t` | No | $> 0$ bytes (multiple of 4) | Total size of the buffer in bytes ($N_{\text{frames}} \times 2_{\text{channels}} \times 2_{\text{bytes}}$). |

#### Concurrency & Thread Safety
* **Task Context:** Only called from real-time audio feeder tasks (`audio_stream_task`, `uac_sync_task`, `sd_player_task`) running on **Core 1**.
* **Blocking Behavior:** Blocks with `portMAX_DELAY` until all bytes are queued into DMA. Zero CPU spin-wait.

#### Production Reference Implementation
```c
void i2s_output_write(const void *data, size_t size)
{
    if (!s_tx_chan || !data || size == 0) {
        return;
    }
    size_t bytes_written = 0;
    esp_err_t ret = i2s_channel_write(s_tx_chan, data, size, &bytes_written, portMAX_DELAY);
    if (ret != ESP_OK) {
        ESP_LOGE("I2S", "i2s_channel_write failed: %s", esp_err_to_name(ret));
    }
}
```

---

### 2.4 `i2s_output_deinit`

Gracefully shuts down the I2S DMA pipeline, releases all DMA frame descriptors, unbinds hardware GPIOs, and clears the channel handle.

```c
esp_err_t i2s_output_deinit(void);
```

#### Production Reference Implementation
```c
esp_err_t i2s_output_deinit(void)
{
    if (s_tx_chan == NULL) {
        return ESP_OK;
    }
    ESP_LOGI("I2S", "Deinitializing I2S channel...");
    i2s_channel_disable(s_tx_chan);
    esp_err_t ret = i2s_del_channel(s_tx_chan);
    s_tx_chan = NULL;
    s_current_sample_rate = 0;
    return ret;
}
```

---

### 2.5 `play_tone`

Generates and outputs a synthesized pure sine wave tone directly through the I2S pipeline without dynamic heap allocation. Used for hardware bench testing, frequency response sweeps, and diagnostic beeps.

```c
void play_tone(float freq_hz, int duration_ms, int sample_rate);
```

#### Mathematical Formulation
$$\text{phase}[n] = \text{phase}[n-1] + \frac{2\pi \cdot f_{\text{tone}}}{f_s} \pmod{2\pi}$$
$$\text{sample}[n] = \text{round}\left(A_{\text{peak}} \cdot \sin(\text{phase}[n])\right)$$

#### Production Reference Implementation
```c
void play_tone(float freq_hz, int duration_ms, int sample_rate)
{
    int total_samples = (sample_rate * duration_ms) / 1000;
    int16_t buffer[256 * 2]; // 256 stereo pairs on stack
    const int buffer_pairs = 256;
    float phase = 0.0f;
    const float phase_step = (2.0f * (float)M_PI * freq_hz) / (float)sample_rate;

    int samples_left = total_samples;
    while (samples_left > 0) {
        int chunk = (samples_left > buffer_pairs) ? buffer_pairs : samples_left;
        for (int i = 0; i < chunk; i++) {
            float amp = 12000.0f; // Safe level (~-8 dBFS to avoid digital clipping)
            float sample_val = sinf(phase) * amp;
            int16_t s16 = (int16_t)sample_val;
            buffer[2 * i]     = s16; // Left channel
            buffer[2 * i + 1] = s16; // Right channel
            phase += phase_step;
            if (phase >= 2.0f * (float)M_PI) {
                phase -= 2.0f * (float)M_PI;
            }
        }
        i2s_output_write(buffer, chunk * 2 * sizeof(int16_t));
        samples_left -= chunk;
    }
}
```

---

### 2.6 `play_startup_chime`

Plays an ascending 4-note musical chord arpeggio (C5 - E5 - G5 - C6) during device initialization to physically verify DAC, amplifier, and headphone jack integrity.

```c
void play_startup_chime(void);
```

#### Musical Progression Table
| Note | Musical Pitch | Frequency | Duration |
| :--- | :--- | :--- | :--- |
| Note 1 | C5 (Tonic) | 523.25 Hz | 150 ms |
| Note 2 | E5 (Major Third) | 659.25 Hz | 150 ms |
| Note 3 | G5 (Perfect Fifth) | 783.99 Hz | 150 ms |
| Note 4 | C6 (Octave Resolve) | 1046.50 Hz | 350 ms |

#### Production Reference Implementation
```c
void play_startup_chime(void)
{
    ESP_LOGI("I2S", "Playing startup test chime to verify PCM5102A DAC output...");
    play_tone(523.25f, 150, s_current_sample_rate);
    play_tone(659.25f, 150, s_current_sample_rate);
    play_tone(783.99f, 150, s_current_sample_rate);
    play_tone(1046.50f, 350, s_current_sample_rate);
    vTaskDelay(pdMS_TO_TICKS(100)); // Allow analog charge dissipation
}
```

---

### 2.7 `audio_dac_set_hardware_mute`

Drives the PCM5102A hardware soft-mute pin (`GPIO 7: XMT`) to suppress audible pops, DC thumps, and switching transients during power-up, sample rate changes, and source handovers.

```c
void audio_dac_set_hardware_mute(bool mute);
```

#### Parameters
| Parameter | Direction | Type | Description |
| :--- | :---: | :---: | :--- |
| `mute` | `[in]` | `bool` | `true` = Assert mute (pull `GPIO 7` LOW); `false` = Release mute (pull `GPIO 7` HIGH). |

#### Production Reference Implementation
```c
void audio_dac_set_hardware_mute(bool mute)
{
    static bool s_gpio_inited = false;
    if (!s_gpio_inited) {
        gpio_config_t io_conf = {
            .pin_bit_mask = (1ULL << BOARD_DAC_MUTE_PIN),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_ENABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&io_conf);
        s_gpio_inited = true;
    }

    if (mute) {
        gpio_set_level((gpio_num_t)BOARD_DAC_MUTE_PIN, 0); // LOW = Soft Mute
        ESP_LOGD("I2S", "Hardware DAC Mute ASSERTED (XMT = LOW)");
    } else {
        gpio_set_level((gpio_num_t)BOARD_DAC_MUTE_PIN, 1); // HIGH = Normal Audio Output
        ESP_LOGD("I2S", "Hardware DAC Mute RELEASED (XMT = HIGH)");
    }
}
```
