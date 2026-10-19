/*
 * ESP32-S3 WebHID Live Parametric EQ Tuning Implementation
 *
 * Implements TinyUSB HID vendor device configuration and callbacks.
 * Decouples volatile SRAM tuning from persistent NVS commits (Golden Rule 7).
 * Pinned strictly to Core 0 (Golden Rule 1).
 */

#include "webhid_peq.h"

#include <stdio.h>
#include <string.h>
#include <math.h>

#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tusb.h"
#include "class/hid/hid_device.h"
#include "class/audio/audio_device.h"

#include "peq_dsp.h"
#include "audio_pipeline.h"
#include "oled_display.h"
#include "board_pins.h"

#define TAG "WEBHID_PEQ"

static bool s_is_mounted = false;
static bool s_is_streaming = false;

// Audio Control States
static uint8_t s_audio_mute[3] = {0, 0, 0};        // [0]=master, [1]=L, [2]=R
static int16_t s_audio_volume[3] = {0, 0, 0};      // 1/256 dB units (0 = 0 dB)
static uint32_t s_audio_sample_rate = 48000;

// =============================================================================
// USB Descriptors Definition (Composite UAC 1.0 Speaker + WebHID)
// =============================================================================

// 64-byte Generic In/Out Raw Report Descriptor for WebHID
static const uint8_t s_hid_report_desc[] = {
    TUD_HID_REPORT_DESC_GENERIC_INOUT(WEBHID_REPORT_SIZE)
};

// String Descriptor Index
enum {
    STRID_LANGID = 0,
    STRID_MANUFACTURER,
    STRID_PRODUCT,
    STRID_SERIAL,
    STRID_AUDIO_INTERFACE,
    STRID_HID_INTERFACE,
};

// Device Descriptor
static const tusb_desc_device_t s_device_desc = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    // Use Interface Association Descriptor (IAD) for Audio + HID Composite
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0x303A,  // Espressif Vendor ID
    .idProduct = 0x4002, // TinyUSB Composite Product ID
    .bcdDevice = 0x0100,
    .iManufacturer = STRID_MANUFACTURER,
    .iProduct = STRID_PRODUCT,
    .iSerialNumber = STRID_SERIAL,
    .bNumConfigurations = 0x01
};

// String Descriptors
static const char *s_string_desc[] = {
    (char[]){0x09, 0x04},        // 0: English (0x0409)
    "Ascension Audio",           // 1: Manufacturer
    "ESP32-S3 Hi-Fi DSP DAC",    // 2: Product
    "123456",                    // 3: Serial
    "Ascension Hi-Fi Audio",     // 4: Audio Interface String
    "WebHID Live PEQ Interface", // 5: HID Interface String
};

// Interface Numbers
enum {
    ITF_NUM_AUDIO_CONTROL = 0,
    ITF_NUM_AUDIO_STREAMING,
    ITF_NUM_HID,
    ITF_NUM_TOTAL
};

// Endpoint Definitions
#define EPNUM_AUDIO_OUT 0x01
#define EPNUM_HID_OUT   0x02
#define EPNUM_HID_IN    0x82

// Entity IDs for UAC 1.0
#define UAC1_ENTITY_INPUT_TERMINAL   0x01
#define UAC1_ENTITY_FEATURE_UNIT     0x02
#define UAC1_ENTITY_OUTPUT_TERMINAL  0x03

// Interface Association Descriptor Macro for UAC 1.0
#define TUD_AUDIO10_DESC_IAD_LEN 8
#define TUD_AUDIO10_DESC_IAD(_firstitf, _nitfs, _stridx) \
    8, TUSB_DESC_INTERFACE_ASSOCIATION, _firstitf, _nitfs, TUSB_CLASS_AUDIO, AUDIO_FUNCTION_SUBCLASS_UNDEFINED, AUDIO_FUNC_PROTOCOL_CODE_V1, _stridx

// Audio Descriptor Length Calculation
#define CONFIG_AUDIO_LEN ( \
    TUD_AUDIO10_DESC_IAD_LEN + \
    TUD_AUDIO10_DESC_STD_AC_LEN + \
    TUD_AUDIO10_DESC_CS_AC_LEN(1) + \
    TUD_AUDIO10_DESC_INPUT_TERM_LEN + \
    TUD_AUDIO10_DESC_OUTPUT_TERM_LEN + \
    TUD_AUDIO10_DESC_FEATURE_UNIT_LEN(2) + \
    TUD_AUDIO10_DESC_STD_AS_LEN + \
    TUD_AUDIO10_DESC_STD_AS_LEN + \
    TUD_AUDIO10_DESC_CS_AS_INT_LEN + \
    TUD_AUDIO10_DESC_TYPE_I_FORMAT_LEN(1) + \
    TUD_AUDIO10_DESC_STD_AS_ISO_EP_LEN + \
    TUD_AUDIO10_DESC_CS_AS_ISO_EP_LEN \
)

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + CONFIG_AUDIO_LEN + TUD_HID_INOUT_DESC_LEN)

static const uint8_t s_config_desc[] = {
    // Configuration Descriptor: 1 config, 3 interfaces, bus powered, 100mA
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 100),

    // Interface Association Descriptor (IAD) for Audio Class (AC + AS)
    TUD_AUDIO10_DESC_IAD(ITF_NUM_AUDIO_CONTROL, 2, STRID_AUDIO_INTERFACE),

    // Standard AC Interface Descriptor (Interface 0, 0 EPs)
    TUD_AUDIO10_DESC_STD_AC(ITF_NUM_AUDIO_CONTROL, 0x00, STRID_AUDIO_INTERFACE),

    // Class-Specific AC Interface Header Descriptor (1 streaming interface)
    TUD_AUDIO10_DESC_CS_AC(0x0100, (TUD_AUDIO10_DESC_INPUT_TERM_LEN + TUD_AUDIO10_DESC_OUTPUT_TERM_LEN + TUD_AUDIO10_DESC_FEATURE_UNIT_LEN(2)), ITF_NUM_AUDIO_STREAMING),

    // Input Terminal Descriptor (Terminal 1, USB Streaming, 2-ch stereo)
    TUD_AUDIO10_DESC_INPUT_TERM(UAC1_ENTITY_INPUT_TERMINAL, AUDIO_TERM_TYPE_USB_STREAMING, 0x00, 0x02, AUDIO10_CHANNEL_CONFIG_LEFT_FRONT | AUDIO10_CHANNEL_CONFIG_RIGHT_FRONT, 0x00, 0x00),

    // Output Terminal Descriptor (Terminal 3, Generic Speaker / DAC, Source FU 2)
    TUD_AUDIO10_DESC_OUTPUT_TERM(UAC1_ENTITY_OUTPUT_TERMINAL, AUDIO_TERM_TYPE_OUT_GENERIC_SPEAKER, 0x00, UAC1_ENTITY_FEATURE_UNIT, 0x00),

    // Feature Unit Descriptor (Unit 2, Source IT 1, Mute + Volume on Master, Ch1, Ch2)
    TUD_AUDIO10_DESC_FEATURE_UNIT(UAC1_ENTITY_FEATURE_UNIT, UAC1_ENTITY_INPUT_TERMINAL, 0x00, \
        (AUDIO10_FU_CONTROL_BM_MUTE | AUDIO10_FU_CONTROL_BM_VOLUME), \
        (AUDIO10_FU_CONTROL_BM_MUTE | AUDIO10_FU_CONTROL_BM_VOLUME), \
        (AUDIO10_FU_CONTROL_BM_MUTE | AUDIO10_FU_CONTROL_BM_VOLUME)),

    // Standard AS Interface (Interface 1, Alt 0, 0 EPs - zero bandwidth idle)
    TUD_AUDIO10_DESC_STD_AS_INT(ITF_NUM_AUDIO_STREAMING, 0x00, 0x00, 0x00),

    // Standard AS Interface (Interface 1, Alt 1, 1 EP OUT - active PCM streaming)
    TUD_AUDIO10_DESC_STD_AS_INT(ITF_NUM_AUDIO_STREAMING, 0x01, 0x01, 0x00),

    // Class-Specific AS Interface Header (link to Terminal 1, 1 frame delay, PCM format)
    TUD_AUDIO10_DESC_CS_AS_INT(UAC1_ENTITY_INPUT_TERMINAL, 0x01, AUDIO10_DATA_FORMAT_TYPE_I_PCM),

    // Type I Format Type Descriptor (2 channels, 2 bytes/sample, 16 bits, 48000 Hz)
    TUD_AUDIO10_DESC_TYPE_I_FORMAT(0x02, 2, 16, 48000),

    // Standard AS Isochronous Audio Data Endpoint (EP 0x01 OUT, Adaptive, 196 max bytes, 1ms)
    TUD_AUDIO10_DESC_STD_AS_ISO_EP(EPNUM_AUDIO_OUT, (uint8_t)(TUSB_XFER_ISOCHRONOUS | TUSB_ISO_EP_ATT_ADAPTIVE), 196, 0x01, 0x00),

    // Class-Specific AS Isochronous Audio Data Endpoint (sampling freq control enabled)
    TUD_AUDIO10_DESC_CS_AS_ISO_EP(AUDIO10_CS_AS_ISO_DATA_EP_ATT_SAMPLING_FRQ, AUDIO10_CS_AS_ISO_DATA_EP_LOCK_DELAY_UNIT_UNDEFINED, 0x0000),

    // HID Interface Descriptor (Interface 2, String 5, EP 0x02 OUT, EP 0x82 IN, 64-byte raw reports)
    TUD_HID_INOUT_DESCRIPTOR(ITF_NUM_HID, STRID_HID_INTERFACE, HID_ITF_PROTOCOL_NONE, sizeof(s_hid_report_desc), EPNUM_HID_OUT, EPNUM_HID_IN, WEBHID_REPORT_SIZE, 1)
};

TU_VERIFY_STATIC(sizeof(s_config_desc) == CONFIG_TOTAL_LEN, "s_config_desc size mismatch");

// =============================================================================
// TinyUSB Event & Audio / HID Callbacks
// =============================================================================

static void tinyusb_event_callback(tinyusb_event_t *event, void *arg)
{
    (void)arg;
    switch (event->id) {
    case TINYUSB_EVENT_ATTACHED:
        s_is_mounted = true;
        ESP_LOGI(TAG, "USB Host attached and configured (UAC 1.0 DAC + WebHID).");
        oled_display_show_eq_overlay("USB Connected");
        break;
    case TINYUSB_EVENT_DETACHED:
        s_is_mounted = false;
        s_is_streaming = false;
        ESP_LOGI(TAG, "USB Host detached.");
        break;
    default:
        break;
    }
}

// -----------------------------------------------------------------------------
// UAC 1.0 Class-Specific Callbacks
// -----------------------------------------------------------------------------

bool tud_audio_set_itf_cb(uint8_t rhport, tusb_control_request_t const *p_request)
{
    (void)rhport;
    uint8_t const itf = tu_u16_low(p_request->wIndex);
    uint8_t const alt = tu_u16_low(p_request->wValue);
    if (itf == ITF_NUM_AUDIO_STREAMING) {
        s_is_streaming = (alt != 0);
        ESP_LOGI(TAG, "UAC 1.0 Audio Streaming: %s", s_is_streaming ? "ACTIVE (48 kHz 16-bit)" : "STOPPED");
        if (s_is_streaming) {
            oled_display_show_eq_overlay("USB DAC Active");
        }
    }
    return true;
}

bool tud_audio_set_itf_close_ep_cb(uint8_t rhport, tusb_control_request_t const *p_request)
{
    (void)rhport;
    uint8_t const itf = tu_u16_low(p_request->wIndex);
    uint8_t const alt = tu_u16_low(p_request->wValue);
    if (itf == ITF_NUM_AUDIO_STREAMING && alt == 0) {
        s_is_streaming = false;
    }
    return true;
}

bool tud_audio_get_req_entity_cb(uint8_t rhport, tusb_control_request_t const *p_request)
{
    uint8_t channelNum = TU_U16_LOW(p_request->wValue);
    uint8_t ctrlSel = TU_U16_HIGH(p_request->wValue);
    uint8_t entityID = TU_U16_HIGH(p_request->wIndex);

    if (channelNum > 2) channelNum = 0;

    if (entityID == UAC1_ENTITY_FEATURE_UNIT) {
        switch (ctrlSel) {
        case AUDIO10_FU_CTRL_MUTE:
            return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &s_audio_mute[channelNum], 1);

        case AUDIO10_FU_CTRL_VOLUME:
            switch (p_request->bRequest) {
            case AUDIO10_CS_REQ_GET_CUR:
                return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &s_audio_volume[channelNum], sizeof(int16_t));
            case AUDIO10_CS_REQ_GET_MIN: {
                int16_t min_vol = -90 * 256; // -90 dB
                return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &min_vol, sizeof(int16_t));
            }
            case AUDIO10_CS_REQ_GET_MAX: {
                int16_t max_vol = 0; // 0 dB
                return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &max_vol, sizeof(int16_t));
            }
            case AUDIO10_CS_REQ_GET_RES: {
                int16_t res_vol = 256; // 1.0 dB
                return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, &res_vol, sizeof(int16_t));
            }
            default:
                return false;
            }
        default:
            return false;
        }
    }
    return false;
}

bool tud_audio_set_req_entity_cb(uint8_t rhport, tusb_control_request_t const *p_request, uint8_t *pBuff)
{
    (void)rhport;
    uint8_t channelNum = TU_U16_LOW(p_request->wValue);
    uint8_t ctrlSel = TU_U16_HIGH(p_request->wValue);
    uint8_t entityID = TU_U16_HIGH(p_request->wIndex);

    if (channelNum > 2) channelNum = 0;

    if (entityID == UAC1_ENTITY_FEATURE_UNIT) {
        switch (ctrlSel) {
        case AUDIO10_FU_CTRL_MUTE:
            if (p_request->bRequest == AUDIO10_CS_REQ_SET_CUR && p_request->wLength == 1) {
                s_audio_mute[channelNum] = pBuff[0];
                ESP_LOGI(TAG, "UAC1 Set Mute: ch %u -> %u", channelNum, pBuff[0]);
                if (channelNum == 0) {
                    audio_pipeline_set_mute(pBuff[0] != 0);
                }
                return true;
            }
            return false;

        case AUDIO10_FU_CTRL_VOLUME:
            if (p_request->bRequest == AUDIO10_CS_REQ_SET_CUR && p_request->wLength == 2) {
                s_audio_volume[channelNum] = (int16_t)tu_unaligned_read16(pBuff);
                if (channelNum == 0) {
                    int vol_db = s_audio_volume[channelNum] / 256;
                    int pct = (vol_db + 90) * 100 / 90;
                    if (pct < 0) pct = 0;
                    if (pct > 100) pct = 100;
                    audio_pipeline_set_volume(pct);
                }
                return true;
            }
            return false;

        default:
            return false;
        }
    }
    return false;
}

bool tud_audio_get_req_ep_cb(uint8_t rhport, tusb_control_request_t const *p_request)
{
    uint8_t ctrlSel = TU_U16_HIGH(p_request->wValue);
    if (ctrlSel == AUDIO10_EP_CTRL_SAMPLING_FREQ) {
        if (p_request->bRequest == AUDIO10_CS_REQ_GET_CUR) {
            uint8_t freq[3];
            freq[0] = (uint8_t)(s_audio_sample_rate & 0xFF);
            freq[1] = (uint8_t)((s_audio_sample_rate >> 8) & 0xFF);
            freq[2] = (uint8_t)((s_audio_sample_rate >> 16) & 0xFF);
            return tud_audio_buffer_and_schedule_control_xfer(rhport, p_request, freq, sizeof(freq));
        }
    }
    return false;
}

bool tud_audio_set_req_ep_cb(uint8_t rhport, tusb_control_request_t const *p_request, uint8_t *pBuff)
{
    (void)rhport;
    uint8_t ctrlSel = TU_U16_HIGH(p_request->wValue);
    if (ctrlSel == AUDIO10_EP_CTRL_SAMPLING_FREQ) {
        if (p_request->bRequest == AUDIO10_CS_REQ_SET_CUR && p_request->wLength == 3) {
            s_audio_sample_rate = tu_unaligned_read32(pBuff) & 0x00FFFFFF;
            ESP_LOGI(TAG, "UAC1 Set Sample Rate: %lu Hz", (unsigned long)s_audio_sample_rate);
            return true;
        }
    }
    return false;
}

// Invoked when received GET HID REPORT DESCRIPTOR
uint8_t const * tud_hid_descriptor_report_cb(uint8_t instance)
{
    (void)instance;
    return s_hid_report_desc;
}

// Invoked when host requests a report via GET_REPORT
uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t* buffer, uint16_t reqlen)
{
    (void)instance; (void)report_id; (void)report_type;
    if (reqlen < sizeof(webhid_peq_packet_t)) {
        return 0;
    }

    peq_profile_t profile;
    if (peq_dsp_get_profile(PEQ_PRESET_USER_CUSTOM, &profile) == ESP_OK) {
        webhid_peq_packet_t pkt = {0};
        pkt.magic = WEBHID_MAGIC_16;
        pkt.cmd = WEBHID_CMD_GET_ACTIVE_PROFILE;
        pkt.band_idx = 0;
        pkt.filter_type = profile.bands[0].type;
        pkt.enabled = profile.bands[0].enabled ? 1 : 0;
        pkt.freq_hz = (uint16_t)roundf(profile.bands[0].freq_hz);
        pkt.gain_db_x10 = (int16_t)roundf(profile.bands[0].gain_db * 10.0f);
        pkt.q_x100 = (uint16_t)roundf(profile.bands[0].q_factor * 100.0f);
        pkt.reserved[0] = profile.num_bands; // Expose dynamic band count to host
        memcpy(buffer, &pkt, sizeof(pkt));
        return sizeof(pkt);
    }
    return 0;
}

// Invoked when host sends a report via SET_REPORT (WebHID live tuning packets)
void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t const* buffer, uint16_t bufsize)
{
    (void)instance; (void)report_id; (void)report_type;

    if (!buffer || bufsize < 16) {
        return;
    }

    // Check Schema A: uint16 magic 0x4551 ("EQ")
    uint16_t magic_u16 = 0;
    memcpy(&magic_u16, &buffer[0], sizeof(uint16_t));

    if (magic_u16 == WEBHID_MAGIC_16 && bufsize >= sizeof(webhid_peq_packet_t)) {
        const webhid_peq_packet_t *pkt = (const webhid_peq_packet_t *)buffer;

        switch (pkt->cmd) {
        case WEBHID_CMD_GET_ACTIVE_PROFILE: {
            uint8_t b_idx = (pkt->band_idx < PEQ_MAX_BANDS) ? pkt->band_idx : 0;
            webhid_peq_send_band_report(b_idx);
            break;
        }

        case WEBHID_CMD_SET_BAND_LIVE: {
            if (pkt->band_idx < PEQ_MAX_BANDS) {
                peq_band_param_t band = {
                    .type = (peq_filter_type_t)pkt->filter_type,
                    .freq_hz = (float)pkt->freq_hz,
                    .gain_db = (float)pkt->gain_db_x10 / 10.0f,
                    .q_factor = (float)pkt->q_x100 / 100.0f,
                    .enabled = (pkt->enabled != 0)
                };

                // Automatically engage User Custom preset in volatile SRAM for live auditioning
                if (peq_dsp_get_current_preset(NULL) != PEQ_PRESET_USER_CUSTOM) {
                    peq_dsp_select_preset(PEQ_PRESET_USER_CUSTOM);
                }

                // Zero flash wear: updates volatile biquad coefficients in SRAM only!
                peq_dsp_update_user_band(pkt->band_idx, &band);
                oled_display_show_eq_overlay("WebHID Audition");

                ESP_LOGD(TAG, "Live Band %d: %.1fHz, %.1fdB, Q=%.2f (type=%d)",
                         pkt->band_idx, band.freq_hz, band.gain_db, band.q_factor, band.type);
            }
            break;
        }

        case WEBHID_CMD_SET_BAND_COUNT: {
            uint8_t count = pkt->band_idx;
            if (count >= 1 && count <= PEQ_MAX_BANDS) {
                peq_dsp_set_band_count(count);
                char msg[24];
                snprintf(msg, sizeof(msg), "%d Bands Active", count);
                oled_display_show_eq_overlay(msg);
                ESP_LOGI(TAG, "WebHID configured active band count: %d", count);
            }
            break;
        }

        case WEBHID_CMD_SET_PREAMP_LIVE: {
            float preamp_db = (float)pkt->gain_db_x10 / 10.0f;
            if (peq_dsp_get_current_preset(NULL) != PEQ_PRESET_USER_CUSTOM) {
                peq_dsp_select_preset(PEQ_PRESET_USER_CUSTOM);
            }
            peq_dsp_set_preamp(preamp_db);
            oled_display_show_eq_overlay("Preamp Tuned");
            ESP_LOGI(TAG, "WebHID live preamp updated: %.1f dB", preamp_db);
            break;
        }

        case WEBHID_CMD_SELECT_PROFILE: {
            peq_preset_id_t slot = (peq_preset_id_t)pkt->band_idx;
            if (slot < PEQ_PRESET_COUNT) {
                peq_dsp_select_preset(slot);
                char name[24] = {0};
                peq_dsp_get_current_preset(name);
                oled_display_show_eq_overlay(name);
                ESP_LOGI(TAG, "WebHID switched active preset to [%d]: %s", slot, name);
            }
            break;
        }

        case WEBHID_CMD_SAVE_TO_PRESET: {
            // Explicit user save request: Commits volatile curve to NVS flash with delta check
            peq_dsp_save_user_profile_nvs();
            oled_display_show_eq_overlay("Saved to NVS");
            ESP_LOGI(TAG, "WebHID explicit commit: User Custom profile persisted to NVS.");
            break;
        }

        case WEBHID_CMD_RESET_TO_PRESET: {
            // Reset user profile to reference flat curves
            peq_band_param_t flat_band = {
                .type = PEQ_FILTER_PEAKING,
                .freq_hz = 1000.0f,
                .gain_db = 0.0f,
                .q_factor = 0.707f,
                .enabled = true
            };
            const float default_freqs[PEQ_MAX_BANDS] = {
                31.0f, 63.0f, 125.0f, 250.0f, 500.0f, 1000.0f, 2000.0f, 4000.0f, 8000.0f, 16000.0f
            };
            for (int b = 0; b < PEQ_MAX_BANDS; b++) {
                flat_band.freq_hz = default_freqs[b];
                flat_band.type = (b == 0) ? PEQ_FILTER_LOW_SHELF : ((b == PEQ_MAX_BANDS - 1) ? PEQ_FILTER_HIGH_SHELF : PEQ_FILTER_PEAKING);
                peq_dsp_update_user_band(b, &flat_band);
            }
            peq_dsp_set_band_count(PEQ_DEFAULT_NUM_BANDS);
            oled_display_show_eq_overlay("Reset Custom");
            ESP_LOGI(TAG, "WebHID reset user profile to default curves.");
            break;
        }

        default:
            ESP_LOGW(TAG, "Unknown WebHID Schema A command: 0x%02X", pkt->cmd);
            break;
        }
        return;
    }


    // Check Schema B: byte 0 magic 'E' (0x45) with float parameters (04_usb_audio_webhid.md)
    if (buffer[0] == WEBHID_MAGIC_BYTE) {
        uint8_t cmd = buffer[1];
        uint8_t band_idx = buffer[2];
        uint8_t filter_type = buffer[3];

        float freq = 1000.0f, gain = 0.0f, q = 0.707f;
        memcpy(&freq, &buffer[4], sizeof(float));
        memcpy(&gain, &buffer[8], sizeof(float));
        memcpy(&q,    &buffer[12], sizeof(float));

        if (cmd == 0x01 && band_idx < PEQ_MAX_BANDS) {
            // Live auditioning in volatile SRAM
            peq_band_param_t band = {
                .type = (peq_filter_type_t)filter_type,
                .freq_hz = freq,
                .gain_db = gain,
                .q_factor = q,
                .enabled = true
            };
            if (peq_dsp_get_current_preset(NULL) != PEQ_PRESET_USER_CUSTOM) {
                peq_dsp_select_preset(PEQ_PRESET_USER_CUSTOM);
            }
            peq_dsp_update_user_band(band_idx, &band);
            oled_display_show_eq_overlay("WebHID Audition");
            ESP_LOGD(TAG, "Schema B Live Band %d: %.1fHz, %.1fdB, Q=%.2f", band_idx, freq, gain, q);
        } else if (cmd == 0x02) {
            // Save to NVS
            peq_dsp_save_user_profile_nvs();
            oled_display_show_eq_overlay("Saved to NVS");
            ESP_LOGI(TAG, "Schema B: Explicit commit to NVS.");
        }
    }
}

// =============================================================================
// Public API
// =============================================================================

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
            .xCoreID = 0, // Pinned strictly to Core 0 (Golden Rule 1: DSP on Core 1, USB on Core 0)
        },
        .descriptor = {
            .device = &s_device_desc,
            .qualifier = NULL,
            .string = s_string_desc,
            .string_count = sizeof(s_string_desc) / sizeof(s_string_desc[0]),
            .full_speed_config = s_config_desc,
            .high_speed_config = NULL,
        },
        .pm_lock_enable = false,
        .event_cb = tinyusb_event_callback,
        .event_arg = NULL,
    };

    esp_err_t ret = tinyusb_driver_install(&tusb_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb_driver_install failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "TinyUSB WebHID initialized successfully (VID: 0x%04X, PID: 0x%04X).",
             s_device_desc.idVendor, s_device_desc.idProduct);
    return ESP_OK;
}

esp_err_t webhid_peq_send_band_report(uint8_t band_idx)
{
    if (band_idx >= PEQ_MAX_BANDS) return ESP_ERR_INVALID_ARG;
    if (!tud_hid_n_ready(0)) return ESP_ERR_NOT_FOUND;

    peq_profile_t profile;
    if (peq_dsp_get_profile(PEQ_PRESET_USER_CUSTOM, &profile) != ESP_OK) {
        return ESP_FAIL;
    }

    webhid_peq_packet_t pkt = {0};
    pkt.magic = WEBHID_MAGIC_16;
    pkt.cmd = WEBHID_CMD_GET_ACTIVE_PROFILE;
    pkt.band_idx = band_idx;
    pkt.filter_type = profile.bands[band_idx].type;
    pkt.enabled = profile.bands[band_idx].enabled ? 1 : 0;
    pkt.freq_hz = (uint16_t)roundf(profile.bands[band_idx].freq_hz);
    pkt.gain_db_x10 = (int16_t)roundf(profile.bands[band_idx].gain_db * 10.0f);
    pkt.q_x100 = (uint16_t)roundf(profile.bands[band_idx].q_factor * 100.0f);
    pkt.reserved[0] = profile.num_bands; // Expose dynamic band count to host

    bool res = tud_hid_n_report(0, 0, &pkt, sizeof(pkt));
    return res ? ESP_OK : ESP_FAIL;
}


bool webhid_peq_is_mounted(void)
{
    return s_is_mounted;
}

bool webhid_peq_is_streaming(void)
{
    return s_is_streaming;
}

uint16_t webhid_peq_audio_available(void)
{
    return tud_audio_available();
}

uint16_t webhid_peq_audio_read(void *buffer, uint16_t bufsize)
{
    return tud_audio_read(buffer, bufsize);
}
