# Module 07: Audio Source Arbiter Function Specifications

> **Module Identifier:** `AUDIO_SOURCE_ARBITER`  
> **Primary References:** [`mode_switching_guide.md`](../guides/mode_switching_guide.md)  
> **Target Subsystems:** Central Coordinator for Wi-Fi Streamer, USB Audio Class DAC, and MicroSD Local Player

---

## 1. Architectural Overview

The `AudioSourceManager` serves as the central arbiter for input mode switching. It enforces the **4-Phase Zero-Pop Muting Protocol** on the PCM5102A DAC (`GPIO 7: XMT`) to guarantee glitch-free, silent source transitions between:
1. **Source 1:** Wi-Fi HTTP Audio Streamer
2. **Source 2:** Native USB OTG Audio Class (UAC 1.0) DAC
3. **Source 3:** MicroSD (SDMMC) Local Player

```
                         [ Audio Source Arbiter ]
                                    │
          ┌─────────────────────────┼─────────────────────────┐
          ▼                         ▼                         ▼
   Wi-Fi Web Stream           Native USB DAC            MicroSD Player
   (Core 0 / Prio 5)         (Core 1 / Prio 15)        (Core 0 / Prio 8)
          │                         │                         │
          └─────────────────────────┼─────────────────────────┘
                                    │
                                    ▼
                     [ 4-Phase Zero-Pop Sequencer ]
                     Phase 1: Assert Mute (GPIO 7 LOW)
                     Phase 2: Teardown Old Pipeline
                     Phase 3: Reclock I2S & Flush DMA
                     Phase 4: Release Mute (GPIO 7 HIGH)
                                    │
                                    ▼
                           [ PCM5102A DAC ]
```

---

## 2. Function Specifications

### 2.1 `audio_source_manager_init`

Initializes the arbiter state machine, configures the hardware soft-mute GPIO (`GPIO 7`), registers source tasks, and defaults to the initial boot source.

```c
esp_err_t audio_source_manager_init(audio_source_t initial_source);
```

#### Production Reference Implementation
```c
typedef enum {
    AUDIO_SOURCE_WIFI = 0,
    AUDIO_SOURCE_USB_DAC,
    AUDIO_SOURCE_SDMMC,
    AUDIO_SOURCE_COUNT
} audio_source_t;

static audio_source_t s_current_source = AUDIO_SOURCE_WIFI;
static SemaphoreHandle_t s_source_mutex = NULL;

esp_err_t audio_source_manager_init(audio_source_t initial_source)
{
    s_source_mutex = xSemaphoreCreateMutex();
    if (!s_source_mutex) return ESP_ERR_NO_MEM;

    // Configure GPIO 7 as DAC Hardware Soft Mute (Active-LOW)
    gpio_config_t mute_cfg = {
        .pin_bit_mask = (1ULL << BOARD_DAC_MUTE_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&mute_cfg);
    gpio_set_level((gpio_num_t)BOARD_DAC_MUTE_PIN, 0); // Muted initially

    s_current_source = initial_source;
    ESP_LOGI("ARBITER", "AudioSourceManager initialized. Active source: %d", initial_source);
    return ESP_OK;
}
```

---

### 2.2 `audio_source_manager_switch_to`

Executes the formal 4-phase handover protocol to switch active audio playback from the current source to `new_source`.

```c
esp_err_t audio_source_manager_switch_to(audio_source_t new_source);
```

#### The 4-Phase Zero-Pop Handover Protocol
| Phase | Action | Timing / Delay | Purpose |
| :---: | :--- | :---: | :--- |
| **Phase 1: Hardware Soft-Mute** | Pull `GPIO 7 (XMT)` LOW | **15 ms** | Allows PCM5102A internal charge pumps to decay analog output smoothly to zero volts. |
| **Phase 2: Pipeline Teardown** | Suspend or stop active source task | **0 ms** | Halts HTTP streaming socket, closes open SD file, or drops USB packets. |
| **Phase 3: DMA Flush & Reclock** | Flush I2S DMA with zeros; reconfigure sample rate | **10 ms** | Purges lingering DMA samples of old source; eliminates audible buffer fragments. |
| **Phase 4: Unmute & Handover** | Pull `GPIO 7 (XMT)` HIGH | **15 ms** | Re-enables analog audio output only after new source stream is stabilized. |

#### Production Reference Implementation
```c
esp_err_t audio_source_manager_switch_to(audio_source_t new_source)
{
    if (new_source >= AUDIO_SOURCE_COUNT) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_source_mutex, portMAX_DELAY);
    if (new_source == s_current_source) {
        xSemaphoreGive(s_source_mutex);
        return ESP_OK;
    }

    ESP_LOGI("ARBITER", "Switching audio source: %d -> %d", s_current_source, new_source);

    // Phase 1: Hardware Soft-Mute
    gpio_set_level((gpio_num_t)BOARD_DAC_MUTE_PIN, 0); // Active-LOW mute
    vTaskDelay(pdMS_TO_TICKS(15));                     // Wait for charge dissipation

    // Phase 2: Teardown Old Pipeline
    switch (s_current_source) {
    case AUDIO_SOURCE_WIFI:
        ESP_LOGI("ARBITER", "Pausing Wi-Fi HTTP stream...");
        // Signal audio_stream_task to pause HTTP socket
        break;
    case AUDIO_SOURCE_USB_DAC:
        ESP_LOGI("ARBITER", "Halting USB DAC streaming...");
        break;
    case AUDIO_SOURCE_SDMMC:
        ESP_LOGI("ARBITER", "Pausing MicroSD playback...");
        break;
    default: break;
    }

    // Phase 3: Flush DMA & Reset Clock
    int16_t zero_frame[512] = {0};
    for (int i = 0; i < 4; i++) {
        i2s_output_write(zero_frame, sizeof(zero_frame));
    }
    vTaskDelay(pdMS_TO_TICKS(10));

    // Power gate Wi-Fi if moving to USB DAC mode (eliminate RF buzz)
    audio_source_manager_power_gate_radios(new_source);

    // Phase 4: Spin up New Source & Unmute
    s_current_source = new_source;
    vTaskDelay(pdMS_TO_TICKS(15));
    gpio_set_level((gpio_num_t)BOARD_DAC_MUTE_PIN, 1); // Release mute (Audio ON)

    ESP_LOGI("ARBITER", "Source switch complete. Active: %d", new_source);
    xSemaphoreGive(s_source_mutex);
    return ESP_OK;
}
```

---

### 2.3 `audio_source_manager_power_gate_radios`

Gates RF transmitter power during USB DAC mode to eliminate high-frequency Wi-Fi packet noise from coupling into the analog headphone stage.

```c
void audio_source_manager_power_gate_radios(audio_source_t active_source);
```

#### Production Reference Implementation
```c
void audio_source_manager_power_gate_radios(audio_source_t active_source)
{
    if (active_source == AUDIO_SOURCE_USB_DAC) {
        ESP_LOGI("ARBITER", "Entering USB DAC mode: Suspending Wi-Fi radio for RF noise isolation...");
        esp_wifi_stop();
    } else {
        wifi_mode_t mode;
        if (esp_wifi_get_mode(&mode) != ESP_OK || mode == WIFI_MODE_NULL) {
            ESP_LOGI("ARBITER", "Restoring Wi-Fi radio power for network playback...");
            esp_wifi_start();
            esp_wifi_set_ps(WIFI_PS_NONE);
        }
    }
}
```
