# Multi-Mode Audio Switching Architecture & Pop-Free Handover

This guide specifies the software architecture, FreeRTOS state machine, and hardware muting sequences required to seamlessly switch between **Wi-Fi Streaming**, **USB DAC**, and **MicroSD Player** modes on the ESP32-S3.

> **Note for Agentic AI / AI Assistants:**  
> This specification details the centralized `AudioSourceManager` state machine, FreeRTOS event groups, RF power-domain gating, and the 4-phase zero-pop audio handover protocol.

---

## 1. System Mode Overview

The unified audio firmware operates in one of three active source modes (or Standby):

```
                       ┌──────────────────────────────┐
                       │   SYSTEM INITIALIZATION      │
                       │   • I2S Driver (GPIO 4, 5, 6)│
                       │   • DAC Hardware Mute (GPIO 7)│
                       │   • NVS Configuration Read   │
                       └──────────────┬───────────────┘
                                      │
                                      ▼
                      ┌────────────────────────────────┐
                      │      AUDIO MODE ARBITER        │
                      └──────────────┬─────────────────┘
         ┌───────────────────────────┼───────────────────────────┐
         ▼                           ▼                           ▼
┌─────────────────┐         ┌─────────────────┐         ┌─────────────────┐
│   MODE 1:       │         │   MODE 2:       │         │   MODE 3:       │
│  Wi-Fi Streamer │         │  USB UAC DAC    │         │  MicroSD Player │
│ • Web Radio/HTTP│         │ • Native USB OTG│         │ • SDMMC Host    │
│ • minimp3       │         │ • Isochronous   │         │ • FLAC/WAV/MP3  │
│ • Core 1 Decode │         │ • 48kHz / 16-bit│         │ • 128KB PSRAM   │
└────────┬────────┘         └────────┬────────┘         └────────┬────────┘
         │                           │                           │
         └───────────────────────────┼───────────────────────────┘
                                     │ PCM Audio Stream (Interleaved L/R)
                                     ▼
                      ┌────────────────────────────────┐
                      │   PARAMETRIC EQUALIZER (PEQ)   │
                      │  • SIMD Biquad Filter Bank     │
                      │  • User AutoEQ / REW Profiles  │
                      └──────────────┬─────────────────┘
                                     │ Filtered PCM
                                     ▼
                      ┌────────────────────────────────┐
                      │        I2S DMA CONTROLLER      │
                      │  GPIO 4 (LCK), 5 (DIN), 6 (BCK)│
                      └──────────────┬─────────────────┘
                                     │ DirectPath Line Out
                                     ▼
                      ┌────────────────────────────────┐
                      │   GY-PCM5102A + TDA1308 AMP    │
                      └────────────────────────────────┘
```

---

## 2. Pop-Free Handover: The 4-Phase Muting Protocol

Switching audio sources or changing sample rates (e.g. 44.1 kHz $\leftrightarrow$ 48 kHz $\leftrightarrow$ 96 kHz) abruptly cuts the continuous I2S bit clock or creates a DC offset jump. This produces an objectionable, potentially speaker-damaging **"pop" or "thump"**.

To guarantee **100% pop-free transitions**, the state machine executes a 4-phase protocol:

```
  Source A Active ──────┐
                        ▼
  [Phase 1] Digital Fade-Out    : Multiply PCM buffer by 15 ms cosine curve down to 0
                        ▼
  [Phase 2] Hardware DAC Mute   : Drive GPIO 7 (XMT) LOW -> PCM5102A engages internal soft-mute
                        ▼
  [Phase 3] Teardown & Reclock  : 
            • Stop Source A task & flush I2S DMA buffers
            • Update I2S sample rate: i2s_output_set_sample_rate(new_rate)
            • If entering USB or SD mode: Put Wi-Fi to sleep (eliminates RF noise)
            • Initialize Source B task
                        ▼
  [Phase 4] Hardware Unmute &   : Drive GPIO 7 (XMT) HIGH -> Ramp new stream from 0 to 100%
            Digital Fade-In
                        ▼
  Source B Active ──────┘
```

### 2.1 Hardware Mute Driver (`mute_control.c`)
```c
#include "driver/gpio.h"

#define PIN_DAC_XMT GPIO_NUM_7

void dac_mute_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << PIN_DAC_XMT),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
    };
    gpio_config(&io_conf);
    gpio_set_level(PIN_DAC_XMT, 0); // Start muted (LOW = Mute)
}

void dac_set_mute(bool muted)
{
    // High = Unmuted; Low = Muted
    gpio_set_level(PIN_DAC_XMT, muted ? 0 : 1);
}
```

---

## 3. State Machine & Event Arbiter

### 3.1 Mode Definitions & System State
```c
typedef enum {
    AUDIO_MODE_WIFI = 0,
    AUDIO_MODE_USB_DAC,
    AUDIO_MODE_SD_CARD,
    AUDIO_MODE_STANDBY,
    AUDIO_MODE_MAX
} audio_mode_t;

typedef struct {
    audio_mode_t current_mode;
    audio_mode_t target_mode;
    bool is_playing;
    uint8_t master_volume; // 0 to 100
    char current_track_title[64];
    uint32_t current_sample_rate;
} audio_manager_state_t;
```

### 3.2 FreeRTOS Mode Switch Task
```c
void audio_mode_manager_task(void *pvParameters)
{
    audio_manager_state_t state = {
        .current_mode = AUDIO_MODE_WIFI,
        .is_playing = false,
        .master_volume = 80
    };

    while (1) {
        audio_mode_t new_mode;
        // Wait for mode switch command from Rotary Encoder, Web UI, or Button
        if (xQueueReceive(s_mode_switch_queue, &new_mode, portMAX_DELAY) == pdTRUE) {
            if (new_mode == state.current_mode) continue;

            ESP_LOGI("AUDIO_MGR", "Switching mode: %d -> %d", state.current_mode, new_mode);

            // Phase 1 & 2: Mute
            audio_dsp_fade_out(15 /* ms */);
            dac_set_mute(true);

            // Phase 3: Teardown Old Source
            switch (state.current_mode) {
                case AUDIO_MODE_WIFI:
                    stop_wifi_streaming_task();
                    break;
                case AUDIO_MODE_USB_DAC:
                    stop_usb_audio_task();
                    break;
                case AUDIO_MODE_SD_CARD:
                    stop_sd_player_task();
                    break;
                default: break;
            }

            // Power Management: Manage Wi-Fi RF power
            if (new_mode == AUDIO_MODE_USB_DAC || new_mode == AUDIO_MODE_SD_CARD) {
                // Sleep Wi-Fi radio to eliminate RF audio buzz and save 120mA
                esp_wifi_stop();
            } else if (new_mode == AUDIO_MODE_WIFI) {
                esp_wifi_start();
            }

            // Phase 3: Initialize New Source
            switch (new_mode) {
                case AUDIO_MODE_WIFI:
                    start_wifi_streaming_task();
                    break;
                case AUDIO_MODE_USB_DAC:
                    start_usb_audio_task();
                    break;
                case AUDIO_MODE_SD_CARD:
                    start_sd_player_task();
                    break;
                default: break;
            }

            state.current_mode = new_mode;
            // Update NVS so player boots into last selected mode
            nvs_set_u8(s_nvs_handle, "last_mode", (uint8_t)new_mode);
            nvs_commit(s_nvs_handle);

            // Phase 4: Unmute & Fade-In
            vTaskDelay(pdMS_TO_TICKS(20)); // Allow PLL clock lock
            dac_set_mute(false);
            audio_dsp_fade_in(20 /* ms */);

            // Notify UI Task
            xQueueSend(s_ui_refresh_queue, &state, 0);
        }
    }
}
```

---

## 4. Multi-Source Trigger Interconnect

The mode switch can be triggered through four distinct interfaces:

1. **Hardware Rotary Encoder:** Long-press switch (`GPIO 17`) opens the Mode Carousel screen; turn to select; short-press to confirm.
2. **Dedicated Tactile Button:** Pressing `GPIO 18` cycles sequentially: `Wi-Fi` $\rightarrow$ `USB DAC` $\rightarrow$ `SD Card` $\rightarrow$ `Wi-Fi`.
3. **Web UI REST API:** Send `POST /api/mode` with JSON payload `{"mode": "usb_dac"}`.
4. **Auto-Sense Hot-Plugging:**
   * **USB VBUS Detect:** When a USB-C host connects to the Native USB port, the player can optionally auto-switch to USB DAC mode.
   * **SD Card Detect:** When a MicroSD card is inserted (`GPIO 40: CD` transitions LOW), the UI displays a toast notification offering instant SD playback.

---

## 5. Reference for AI Agents: Implementation Checklist & Invariants

### 5.1 Mode Handover Invariants
1. **Zero-Pop Order of Operations:**
   Never change the active source task or reclock I2S while audio is unmuted. The sequence MUST strictly follow: `Fade Out -> Hardware Mute (GPIO 7 LOW) -> Stop Old Source -> Reclock I2S -> Start New Source -> Hardware Unmute (GPIO 7 HIGH) -> Fade In`. Reversing or skipping steps produces loud DC thumps.
2. **Wi-Fi RF Gating Invariant:**
   To guarantee audiophile SNR in USB DAC and SD Player modes, power down the Wi-Fi radio via `esp_wifi_stop()`. If the Wi-Fi synthesizer remains running during local playback, 2.4 GHz RF bursts induce audible buzzing in the high-impedance input traces of the TDA1308 amplifier.

### 5.2 Implementation Checklist
Autonomous agents implementing multi-mode switching should follow this sequence:

- [ ] **Step 1: Wire PCM5102A `XMT` to GPIO 7**
  * Remove static jumper to 3.3V and connect to ESP32-S3 `GPIO 7` for firmware-controlled soft-muting.
- [ ] **Step 2: Implement Raised-Cosine Fade Engine (`fade_engine.c`)**
  * Apply $y[n] = x[n] \cdot 0.5 \cdot (1 - \cos(\pi n / N))$ to avoid discontinuities.
- [ ] **Step 3: Create Mode Arbiter Task (`mode_manager.c`)**
  * Pinned to **Core 0**, priority **5**.
  * Owns the master audio mutex protecting the I2S DMA handle.
- [ ] **Step 4: Configure Persistent State in NVS**
  * Store `last_mode` and `last_volume` so user preferences survive power cycles.
- [ ] **Step 5: Integrate UI Dispatch**
  * Broadcast state changes to the display driver (`ui_task`) to update status icons and mode badges.
