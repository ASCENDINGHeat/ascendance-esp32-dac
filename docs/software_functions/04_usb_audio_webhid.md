# Module 04: USB Audio Class (UAC) & WebHID Function Specifications

> **Module Identifier:** `USB_AUDIO_WEBHID`  
> **Primary References:** [`usb_dac_guide.md`](../guides/usb_dac_guide.md), USB 2.0 Full Speed Spec, USB Audio Class 1.0  
> **ESP-IDF Component:** `espressif/tinyusb` (`tinyusb.h`, `tusb_audio.h`, `tusb_hid.h`)  
> **Hardware Port:** ESP32-S3 Native USB OTG (`GPIO 19: D-`, `GPIO 20: D+`)

---

## 1. Architectural Overview

The USB subsystem configures the ESP32-S3 Native USB OTG PHY as a **USB Composite Device** containing:
1. **USB Audio Class 1.0 (UAC 1.0) Streaming Interface:** 16-bit / 44.1 kHz, 48 kHz, or 96 kHz stereo isochronous PCM sink with host asynchronous feedback synchronization.
2. **USB Human Interface Device (HID) Vendor Interface:** 64-byte feature reports allowing direct, driverless WebHID browser communication (FiiO JA11 / Moondrop DSP style live EQ tuning without Wi-Fi).

```
                      HOST PC / SMARTPHONE
              ┌──────────────────────────────────┐
              │  USB Audio Stream (Isochronous)  │
              │  WebHID Live Tuning (Interrupt)  │
              └────────────────┬─────────────────┘
                               │ Native USB OTG (GPIO 19/20)
                               ▼
                    TINYUSB COMPOSITE STACK
         ┌─────────────────────┴─────────────────────┐
         ▼                                           ▼
   UAC 1.0 Audio Endpoint                  WebHID Vendor Interface
 (tud_audio_rx_done_pre_read_cb)           (tud_hid_set_report_cb)
         │                                           │
         ▼                                           ▼
  PSRAM Ringbuffer                            peq_live_update_band()
 (Clock Drift Sync)                           (Volatile SRAM Table)
         │
         ▼
  I2S Master DMA
```

---

## 2. Function Specifications

### 2.1 `webhid_peq_init`

Initializes the TinyUSB driver with the composite UAC 1.0 (Audio Control + Audio Streaming) and WebHID vendor descriptor tables, pinned strictly to Core 0.

```c
esp_err_t webhid_peq_init(void);
```

#### Production Reference Implementation
```c
#include "webhid_peq.h"
#include "tinyusb.h"
#include "tusb.h"

esp_err_t webhid_peq_init(void)
{
    ESP_LOGI(TAG, "Initializing TinyUSB WebHID interface on GPIO %d (D-) and GPIO %d (D+)...",
             BOARD_USB_DM_PIN, BOARD_USB_DP_PIN);

    tinyusb_config_t const tusb_cfg = {
        .port = TINYUSB_PORT_FULL_SPEED_0,
        .phy = {
            .skip_setup = false,
            .self_powered = false,
            .vbus_monitor_io = -1,
        },
        .task = {
            .size = 4096,
            .priority = 5,
            .xCoreID = 0, // Pinned strictly to Core 0
        },
        .descriptor = {
            .device = &s_device_desc,
            .string = s_string_desc,
            .string_count = sizeof(s_string_desc) / sizeof(s_string_desc[0]),
            .full_speed_config = s_config_desc,
        },
        .event_cb = tinyusb_event_callback,
    };

    return tinyusb_driver_install(&tusb_cfg);
}
```

---

### 2.2 UAC 1.0 Audio Stream Polling & Consumer API

Called by Core 1's real-time `audio_dsp_task` to read incoming PCM frames from TinyUSB's 2048-byte software FIFO buffer.

```c
uint16_t webhid_peq_audio_available(void);
uint16_t webhid_peq_audio_read(void *buffer, uint16_t bufsize);
bool webhid_peq_is_streaming(void);
```

#### Production Usage in `audio_dsp_task` (Core 1)
```c
if (source == AUDIO_SOURCE_STANDALONE) {
    uint16_t avail = webhid_peq_audio_available();
    if (avail >= 192) {
        uint16_t to_read = avail > sizeof(buffer) ? sizeof(buffer) : avail;
        to_read = (to_read / 4) * 4; // Ensure stereo pair alignment (4 bytes per pair)
        if (to_read > 0) {
            uint16_t n_read = webhid_peq_audio_read(buffer, to_read);
            if (n_read > 0) {
                int pairs = n_read / (2 * sizeof(int16_t));
                peq_dsp_process_interleaved_s16(buffer, pairs);
                audio_pipeline_write(buffer, n_read);
                continue;
            }
        }
    }
    vTaskDelay(pdMS_TO_TICKS(2));
    continue;
}
```

---

### 2.3 UAC 1.0 Control & Sampling Rate Callbacks

Handles host operating system volume, mute, and sample rate negotiation:

```c
bool tud_audio_set_itf_cb(uint8_t rhport, tusb_control_request_t const *p_request);
bool tud_audio_get_req_entity_cb(uint8_t rhport, tusb_control_request_t const *p_request);
bool tud_audio_set_req_entity_cb(uint8_t rhport, tusb_control_request_t const *p_request, uint8_t *pBuff);
bool tud_audio_get_req_ep_cb(uint8_t rhport, tusb_control_request_t const *p_request);
bool tud_audio_set_req_ep_cb(uint8_t rhport, tusb_control_request_t const *p_request, uint8_t *pBuff);
```

* **`tud_audio_set_itf_cb`:** Tracks Alternate Setting 1 (active audio) vs Alternate Setting 0 (idle).
* **`tud_audio_set_req_entity_cb`:** Synchronizes host volume slider (1/256 dB) directly with `audio_pipeline_set_volume()`, and host mute state with `audio_pipeline_set_mute()` (GPIO 7).
* **`tud_audio_get_req_ep_cb`:** Confirms the 48,000 Hz discrete sample rate to host audio engines.

---

### 2.4 `tud_hid_set_report_cb`

Processes incoming 64-byte WebHID feature reports from browser-based live tuning apps (e.g. Chrome WebHID API at `chrome://device-log`). Parses live EQ frequency, gain, and Q updates, applying them immediately in volatile SRAM.

```c
void tud_hid_set_report_cb(uint8_t instance,
                           uint8_t report_id,
                           hid_report_type_t report_type,
                           uint8_t const* buffer,
                           uint16_t bufsize);
```

#### WebHID Packet Protocol (Report ID `0x01`, Size: 64 Bytes)
| Byte Offset | Field | Type | Description |
| :---: | :--- | :--- | :--- |
| `0` | Magic Header | `uint8_t` | Fixed signature `0x45` ('E'). |
| `1` | Command ID | `uint8_t` | `0x01` = Live Audition, `0x02` = Save to Preset, `0x03` = Query. |
| `2` | Band Index | `uint8_t` | Target EQ Band (`0` to `9`). |
| `3` | Filter Type | `uint8_t` | `0` = Peaking, `1` = Low Shelf, `2` = High Shelf. |
| `4..7` | Frequency (Hz) | `float` | 32-bit IEEE 754 float (Little-Endian). |
| `8..11`| Gain (dB) | `float` | 32-bit IEEE 754 float (Little-Endian). |
| `12..15`| Q Factor | `float` | 32-bit IEEE 754 float (Little-Endian). |
| `16..63`| Reserved / Pad | `uint8_t [48]`| Zero-padded. |

#### Production Reference Implementation
```c
void tud_hid_set_report_cb(uint8_t instance,
                           uint8_t report_id,
                           hid_report_type_t report_type,
                           uint8_t const* buffer,
                           uint16_t bufsize)
{
    (void)instance; (void)report_id; (void)report_type;

    if (bufsize < 16 || buffer[0] != 0x45) {
        return; // Invalid magic header
    }

    uint8_t cmd = buffer[1];
    uint8_t band_idx = buffer[2];
    uint8_t filter_type = buffer[3];

    float freq, gain, q;
    memcpy(&freq, &buffer[4], sizeof(float));
    memcpy(&gain, &buffer[8], sizeof(float));
    memcpy(&q,    &buffer[12], sizeof(float));

    if (cmd == 0x01) {
        // Volatile live auditioning (zero flash wear)
        extern peq_context_t g_peq_ctx;
        peq_live_update_band(&g_peq_ctx, band_idx, (peq_filter_type_t)filter_type, freq, gain, q);
    } else if (cmd == 0x02) {
        // Explicit Save to Preset
        extern peq_context_t g_peq_ctx;
        peq_save_preset_to_nvs(band_idx, &g_peq_ctx);
    }
}
```

#### 2.4.1 Browser GUI Custom Value Input & Safe DSP Bounds
The companion WebHID tuner client (`tools/webhid_tuner/index.html`) equips each band card and the preamp headroom stage with precision numeric inputs (`<input type="number">`) in addition to range sliders. Inputs are validated and clamped to safe DSP bounds:
* **Frequency:** `20 Hz` to `20,000 Hz` (Step: `1 Hz`, clamped via `clamp(val, 20, 20000)`)
* **Gain:** `-24.0 dB` to `+12.0 dB` (Step: `0.1 dB`, clamped via `clamp(val, -24.0, 12.0)`)
* **Q Factor:** `0.10` to `10.00` (Step: `0.01`, clamped via `clamp(val, 0.10, 10.00)`)
* **Preamp Headroom:** `-12.0 dB` to `0.0 dB` (Step: `0.1 dB`, clamped via `clamp(val, -12.0, 0.0)`)
* **Live Two-Way Binding:** Dragging a slider updates the numeric field immediately; typing in the numeric box updates the slider, recalculates the canvas frequency response curve, and streams live 64-byte HID reports to the ESP32-S3.

