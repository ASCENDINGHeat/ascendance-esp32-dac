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
#include <stdatomic.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_pm.h"
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
#include "led_indicator.h"
#include "board_pins.h"

#define TAG "WEBHID_PEQ"

// Atomic cross-core USB state flags (written on Core 0, read on Core 1)
static _Atomic bool s_is_mounted = false;
static _Atomic bool s_is_streaming = false;
static _Atomic bool s_is_suspended = false;
static esp_pm_lock_handle_t s_pm_lock = NULL;
static bool s_pm_lock_acquired = false;

// Audio Control States
static portMUX_TYPE s_uac_rate_lock = portMUX_INITIALIZER_UNLOCKED;
static uint8_t s_audio_mute[3] = {0, 0, 0};        // [0]=master, [1]=L, [2]=R
static int16_t s_audio_volume[3] = {0, 0, 0};      // 1/256 dB units (0 = 0 dB)
static uint32_t s_audio_sample_rate = 48000;
static uint32_t s_pending_sample_rate = 48000;
static bool s_sample_rate_pending = false;

// =============================================================================
// Internal-SRAM Lock-Free SPSC Ring Buffer (Core 0 Producer -> Core 1 Consumer)
// Capacity: 2048 stereo 24-bit frames (12,288 B, ~42.7 ms @ 48 kHz)
// =============================================================================
#define AUDIO_SPSC_FRAME_CAP   2048U   // Must be power of 2
#define AUDIO_SPSC_FRAME_MASK  (AUDIO_SPSC_FRAME_CAP - 1U)
#define AUDIO_BYTES_PER_FRAME  6U      // 2 channels * 3 bytes (24-bit packed)

typedef struct {
    uint8_t data[AUDIO_SPSC_FRAME_CAP * AUDIO_BYTES_PER_FRAME];
    _Atomic uint32_t wr_frames; // Written only by Core 0 (TinyUSB task)
    _Atomic uint32_t rd_frames; // Written only by Core 1 (audio_dsp_task)
    _Atomic uint32_t flush_gen; // Incremented by Core 0 on stream stop/start/reclock
    uint32_t cons_gen;          // Owned exclusively by Core 1
} audio_spsc_t;

static DRAM_ATTR audio_spsc_t s_audio_spsc;

static inline uint32_t audio_spsc_level_frames(void)
{
    uint32_t wr = atomic_load_explicit(&s_audio_spsc.wr_frames, memory_order_acquire);
    uint32_t rd = atomic_load_explicit(&s_audio_spsc.rd_frames, memory_order_acquire);
    return wr - rd;
}

static inline void audio_spsc_request_flush(void)
{
    atomic_fetch_add_explicit(&s_audio_spsc.flush_gen, 1U, memory_order_release);
}

// Producer: Core 0 only (TinyUSB RX callback)
static uint32_t audio_spsc_push_frames(const uint8_t *src, uint32_t num_frames)
{
    uint32_t wr = atomic_load_explicit(&s_audio_spsc.wr_frames, memory_order_relaxed);
    uint32_t rd = atomic_load_explicit(&s_audio_spsc.rd_frames, memory_order_acquire);
    uint32_t free_frames = AUDIO_SPSC_FRAME_CAP - (wr - rd);
    if (num_frames > free_frames) {
        num_frames = free_frames;
    }
    if (num_frames == 0) {
        return 0;
    }

    uint32_t idx = wr & AUDIO_SPSC_FRAME_MASK;
    uint32_t first = AUDIO_SPSC_FRAME_CAP - idx;
    if (first > num_frames) {
        first = num_frames;
    }

    memcpy(&s_audio_spsc.data[idx * AUDIO_BYTES_PER_FRAME],
           src, first * AUDIO_BYTES_PER_FRAME);

    uint32_t second = num_frames - first;
    if (second > 0) {
        memcpy(&s_audio_spsc.data[0],
               src + (first * AUDIO_BYTES_PER_FRAME),
               second * AUDIO_BYTES_PER_FRAME);
    }

    // Release fence guarantees payload writes are visible to Core 1 before wr_frames advances
    atomic_store_explicit(&s_audio_spsc.wr_frames, wr + num_frames, memory_order_release);
    return num_frames;
}

// Compute and push clamped 10.14 asynchronous feedback to EP 0x81 IN (Core 0)
static void webhid_update_async_feedback(bool nominal_only)
{
#if CFG_TUD_AUDIO_ENABLE_EP_OUT && CFG_TUD_AUDIO_ENABLE_FEEDBACK_EP
    uint32_t sr = webhid_peq_get_sample_rate();
    // Exact Full-Speed UAC 1.0 nominal 10.14 rate: (sr * 2^14) / 1000
    // 48000 Hz -> 786432 (48.000 * 16384), 44100 Hz -> 722534 (44.100 * 16384)
    int32_t nom_10_14 = (int32_t)(((uint64_t)sr << 14) / 1000ULL);
    int32_t fb_10_14 = nom_10_14;

    if (!nominal_only) {
        const int32_t target_frames = (int32_t)(AUDIO_SPSC_FRAME_CAP / 2U);
        const int32_t cur_frames = (int32_t)audio_spsc_level_frames();
        const int32_t err_frames = target_frames - cur_frames; // >0 => host sends slightly faster

        // Gentle proportional gain: 1 frame error -> 8 LSBs in 10.14 (~0.00049 samples/ms)
        fb_10_14 = nom_10_14 + (err_frames * 8);

        // Hard clamp to nominal +/- 1.0 sample/ms to prevent host starvation or jitter blowout
        int32_t min_10_14 = nom_10_14 - (1 << 14);
        int32_t max_10_14 = nom_10_14 + (1 << 14);
        if (fb_10_14 < min_10_14) fb_10_14 = min_10_14;
        if (fb_10_14 > max_10_14) fb_10_14 = max_10_14;
    }

    // TinyUSB tud_audio_fb_set() expects Q16.16 and automatically converts >> 2 to 10.14 on FS
    tud_audio_fb_set((uint32_t)fb_10_14 << 2);
#else
    (void)nominal_only;
#endif
}

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

// Endpoint Definitions (strictly partitioned: EP1 = Audio/Feedback, EP2 = WebHID)
#define EPNUM_AUDIO_OUT 0x01
#define EPNUM_AUDIO_FB  0x81
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
    TUD_AUDIO10_DESC_TYPE_I_FORMAT_LEN(2) + \
    TUD_AUDIO10_DESC_STD_AS_ISO_EP_LEN + \
    TUD_AUDIO10_DESC_CS_AS_ISO_EP_LEN + \
    TUD_AUDIO10_DESC_STD_AS_ISO_SYNC_EP_LEN \
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

    // Standard AS Interface (Interface 1, Alt 1, 2 EPs: EP 0x01 OUT + EP 0x81 IN Feedback)
    TUD_AUDIO10_DESC_STD_AS_INT(ITF_NUM_AUDIO_STREAMING, 0x01, 0x02, 0x00),

    // Class-Specific AS Interface Header (link to Terminal 1, 1 frame delay, PCM format)
    TUD_AUDIO10_DESC_CS_AS_INT(UAC1_ENTITY_INPUT_TERMINAL, 0x01, AUDIO10_DATA_FORMAT_TYPE_I_PCM),

    // Type I Format Type Descriptor (2 channels, 3 bytes/sample, 24 bits, 44.1/48 kHz)
    TUD_AUDIO10_DESC_TYPE_I_FORMAT(0x02, 3, 24, 44100, 48000),

    // Standard AS Isochronous Audio Data Endpoint (EP 0x01 OUT, Asynchronous, 294 B max, 1 ms, synch EP 0x81)
    TUD_AUDIO10_DESC_STD_AS_ISO_EP(EPNUM_AUDIO_OUT, (uint8_t)(TUSB_XFER_ISOCHRONOUS | TUSB_ISO_EP_ATT_ASYNCHRONOUS | TUSB_ISO_EP_ATT_DATA), 294, 0x01, EPNUM_AUDIO_FB),

    // Class-Specific AS Isochronous Audio Data Endpoint (sampling freq control enabled)
    TUD_AUDIO10_DESC_CS_AS_ISO_EP(AUDIO10_CS_AS_ISO_DATA_EP_ATT_SAMPLING_FRQ, AUDIO10_CS_AS_ISO_DATA_EP_LOCK_DELAY_UNIT_UNDEFINED, 0x0000),

    // Standard AS Isochronous Synch/Feedback Endpoint (EP 0x81 IN, 3-byte 10.14, bInterval=1, bRefresh=3 -> 8 ms)
    TUD_AUDIO10_DESC_STD_AS_ISO_SYNC_EP(EPNUM_AUDIO_FB, 0x03),

    // HID Interface Descriptor (Interface 2, String 5, EP 0x02 OUT, EP 0x82 IN, 64-byte raw reports, bInterval=4 ms)
    TUD_HID_INOUT_DESCRIPTOR(ITF_NUM_HID, STRID_HID_INTERFACE, HID_ITF_PROTOCOL_NONE, sizeof(s_hid_report_desc), EPNUM_HID_OUT, EPNUM_HID_IN, WEBHID_REPORT_SIZE, 4)
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
        atomic_store_explicit(&s_is_mounted, true, memory_order_release);
        atomic_store_explicit(&s_is_suspended, false, memory_order_release);
        ESP_LOGI(TAG, "USB Host attached and configured (Async UAC 1.0 DAC + WebHID).");
        oled_display_show_eq_overlay("USB Connected");
        break;
    case TINYUSB_EVENT_DETACHED:
        atomic_store_explicit(&s_is_mounted, false, memory_order_release);
        atomic_store_explicit(&s_is_streaming, false, memory_order_release);
        atomic_store_explicit(&s_is_suspended, false, memory_order_release);
        audio_spsc_request_flush();
        if (s_pm_lock && !s_pm_lock_acquired) {
            esp_pm_lock_acquire(s_pm_lock);
            s_pm_lock_acquired = true;
        }
        ESP_LOGI(TAG, "USB Host detached.");
        break;
    default:
        break;
    }
}

void tud_suspend_cb(bool remote_wakeup_en)
{
    (void)remote_wakeup_en;
    atomic_store_explicit(&s_is_suspended, true, memory_order_release);
    atomic_store_explicit(&s_is_streaming, false, memory_order_release);
    audio_spsc_request_flush();
    audio_pipeline_set_mute(true);
    oled_display_sleep();
    led_indicator_set_volume(0, true);
    if (s_pm_lock && s_pm_lock_acquired) {
        esp_pm_lock_release(s_pm_lock);
        s_pm_lock_acquired = false;
    }
    ESP_LOGI(TAG, "USB Bus Suspended: DAC muted, OLED asleep, CPU frequency unlocked.");
}

void tud_resume_cb(void)
{
    if (s_pm_lock && !s_pm_lock_acquired) {
        esp_pm_lock_acquire(s_pm_lock);
        s_pm_lock_acquired = true;
    }
    atomic_store_explicit(&s_is_suspended, false, memory_order_release);
    peq_dsp_trigger_wake_ramp();
    oled_display_wake();
    audio_pipeline_set_mute(false);
    led_indicator_set_volume(audio_pipeline_get_volume(), audio_pipeline_is_muted());
    ESP_LOGI(TAG, "USB Bus Resumed: CPU locked max, OLED awake.");
}

// -----------------------------------------------------------------------------
// UAC 1.0 Class-Specific Callbacks
// -----------------------------------------------------------------------------

#if CFG_TUD_AUDIO_ENABLE_EP_OUT && CFG_TUD_AUDIO_ENABLE_FEEDBACK_EP
void tud_audio_feedback_params_cb(uint8_t func_id, uint8_t alt_itf, audio_feedback_params_t *feedback_param)
{
    (void)func_id;
    (void)alt_itf;
    // Manual feedback computed from our C11 SPSC ring buffer occupancy via tud_audio_fb_set()
    feedback_param->method = AUDIO_FEEDBACK_METHOD_DISABLED;
}
#endif

bool tud_audio_rx_done_isr(uint8_t rhport, uint16_t n_bytes_received, uint8_t func_id,
                           uint8_t ep_out, uint8_t cur_alt_setting)
{
    (void)rhport;
    (void)n_bytes_received;
    (void)func_id;
    (void)ep_out;
    (void)cur_alt_setting;

    // Drain TinyUSB software RX FIFO strictly on Core 0 into our lock-free C11 SPSC ring buffer
    static uint8_t s_rx_tmp[294];
    uint16_t avail = tud_audio_available();
    while (avail >= AUDIO_BYTES_PER_FRAME) {
        uint16_t chunk = (avail > sizeof(s_rx_tmp)) ? (uint16_t)sizeof(s_rx_tmp) : avail;
        chunk = (uint16_t)((chunk / AUDIO_BYTES_PER_FRAME) * AUDIO_BYTES_PER_FRAME);
        uint16_t got = tud_audio_read(s_rx_tmp, chunk);
        if (got >= AUDIO_BYTES_PER_FRAME) {
            audio_spsc_push_frames(s_rx_tmp, got / AUDIO_BYTES_PER_FRAME);
        } else {
            break;
        }
        avail = tud_audio_available();
    }

    webhid_update_async_feedback(false);
    return true;
}

bool tud_audio_set_itf_cb(uint8_t rhport, tusb_control_request_t const *p_request)
{
    (void)rhport;
    uint8_t const itf = tu_u16_low(p_request->wIndex);
    uint8_t const alt = tu_u16_low(p_request->wValue);
    if (itf == ITF_NUM_AUDIO_STREAMING) {
        bool streaming = (alt != 0);
        atomic_store_explicit(&s_is_streaming, streaming, memory_order_release);
        audio_spsc_request_flush();
        ESP_LOGI(TAG, "UAC 1.0 Audio Streaming: %s (%lu Hz 24-bit Async)",
                 streaming ? "ACTIVE" : "STOPPED",
                 (unsigned long)webhid_peq_get_sample_rate());
        if (streaming) {
            webhid_update_async_feedback(true);
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
        atomic_store_explicit(&s_is_streaming, false, memory_order_release);
        audio_spsc_request_flush();
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
                    int pct;
                    if (vol_db <= -80) {
                        pct = 0;
                    } else {
                        pct = (int)roundf(100.0f * powf(10.0f, (float)vol_db / 40.0f));
                        if (pct < 0) pct = 0;
                        if (pct > 100) pct = 100;
                    }
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
            uint32_t req_rate = (uint32_t)pBuff[0] | ((uint32_t)pBuff[1] << 8) | ((uint32_t)pBuff[2] << 16);
            if (req_rate != 44100 && req_rate != 48000) {
                ESP_LOGW(TAG, "UAC1 Rejected unsupported sample rate: %lu Hz", (unsigned long)req_rate);
                return false;
            }
            bool changed = false;
            taskENTER_CRITICAL(&s_uac_rate_lock);
            if (req_rate != s_audio_sample_rate) {
                s_audio_sample_rate = req_rate;
                s_pending_sample_rate = req_rate;
                s_sample_rate_pending = true;
                changed = true;
            }
            taskEXIT_CRITICAL(&s_uac_rate_lock);
            if (changed) {
                audio_spsc_request_flush();
                webhid_update_async_feedback(true);
            }
            ESP_LOGI(TAG, "UAC1 Host Set Sample Rate: %lu Hz (24-bit Async)", (unsigned long)req_rate);
            return true;
        }
    }
    return false;
}

static bool webhid_build_band_packet(uint8_t band_idx, peq_channel_t ch, webhid_peq_packet_t *pkt)
{
    if (!pkt) return false;

    peq_mixer_config_t mixer;
    if (peq_dsp_get_mixer(&mixer) != ESP_OK) {
        memset(&mixer, 0, sizeof(mixer));
    }

    if (band_idx == WEBHID_REPORT_SUB_MATRIX) {
        memset(pkt, 0, sizeof(*pkt));
        pkt->magic = WEBHID_MAGIC_16;
        pkt->cmd = WEBHID_CMD_SET_MIXER;
        pkt->band_idx = (uint8_t)mixer.mode;
        pkt->filter_type = (uint8_t)mixer.balance;
        pkt->enabled = mixer.width_pct;
        pkt->freq_hz = (uint16_t)mixer.crossfeed_level;
        memcpy(&pkt->reserved[0], &mixer.custom_matrix[0][0], sizeof(float) * 4);
        return true;
    }

    if (band_idx == WEBHID_REPORT_SUB_EXT_STATE) {
        peq_dynamics_cfg_t dyn;
        peq_dsp_get_dynamics_cfg(&dyn);
        char slot_name[PEQ_SLOT_NAME_MAX] = {0};
        peq_preset_id_t active = peq_dsp_get_current_preset(slot_name);
        uint32_t sr = webhid_peq_get_sample_rate();
        uint16_t fir_max = peq_dsp_get_fir_max_taps_for_sr((int)sr);

        uint8_t flags = (dyn.limiter_enabled ? 0x01 : 0x00) |
                        (((uint8_t)dyn.loudness_mode & 0x03) << 1) |
                        (dyn.fir_enabled ? 0x08 : 0x00) |
                        (((uint8_t)dyn.oled_view & 0x03) << 4);

        memset(pkt, 0, sizeof(*pkt));
        pkt->magic = WEBHID_MAGIC_16;
        pkt->cmd = WEBHID_CMD_SET_DYNAMICS;
        pkt->band_idx = WEBHID_REPORT_SUB_EXT_STATE;
        pkt->filter_type = (uint8_t)active;
        pkt->enabled = flags;
        pkt->freq_hz = dyn.auto_mute_sec;
        pkt->gain_db_x10 = (int16_t)dyn.oled_dim_sec;
        pkt->q_x100 = dyn.fir_taps_l;
        memcpy(&pkt->reserved[0], slot_name, 16);
        memcpy(&pkt->reserved[16], &dyn.fir_taps_r, sizeof(uint16_t));
        memcpy(&pkt->reserved[18], &fir_max, sizeof(uint16_t));

        peq_harmonics_cfg_t harm;
        peq_dsp_get_harmonics_cfg(&harm);
        uint8_t harm_flags = (harm.bass_enabled ? 0x01 : 0x00) |
                             (harm.exciter_enabled ? 0x02 : 0x00) |
                             (((uint8_t)harm.exciter_mode & 0x03) << 2);
        uint16_t bass_hz = harm.bass_freq_hz;
        uint16_t exc_hz = harm.exciter_freq_hz;
        pkt->reserved[20] = harm_flags;
        pkt->reserved[21] = harm.bass_amount_pct;
        pkt->reserved[22] = harm.exciter_amount_pct;
        memcpy(&pkt->reserved[23], &bass_hz, sizeof(uint16_t));
        memcpy(&pkt->reserved[25], &exc_hz, sizeof(uint16_t));
        pkt->reserved[27] = 0;
        return true;
    }

    if (band_idx >= PEQ_MAX_BANDS) {
        return false;
    }

    peq_profile_t profile;
    peq_preset_id_t active = peq_dsp_get_current_preset(NULL);
    if (peq_dsp_get_profile_ch(active, ch, &profile) != ESP_OK) {
        return false;
    }

    uint32_t sr = webhid_peq_get_sample_rate();
    int16_t preamp_x10 = (int16_t)roundf(profile.preamp_gain_db * 10.0f);
    uint8_t flags = (peq_dsp_is_linked_stereo() ? 0x01 : 0x00) |
                    (peq_dsp_get_ab_bypass() ? 0x02 : 0x00);

    memset(pkt, 0, sizeof(*pkt));
    pkt->magic = WEBHID_MAGIC_16;
    pkt->cmd = WEBHID_CMD_GET_ACTIVE_PROFILE;
    pkt->band_idx = band_idx;
    pkt->filter_type = (uint8_t)profile.bands[band_idx].type;
    pkt->enabled = profile.bands[band_idx].enabled ? 1 : 0;
    pkt->freq_hz = (uint16_t)roundf(profile.bands[band_idx].freq_hz);
    pkt->gain_db_x10 = (int16_t)roundf(profile.bands[band_idx].gain_db * 10.0f);
    pkt->q_x100 = (uint16_t)roundf(profile.bands[band_idx].q_factor * 100.0f);
    pkt->reserved[0] = profile.num_bands;
    pkt->reserved[1] = (uint8_t)ch;
    pkt->reserved[2] = flags;
    pkt->reserved[3] = (uint8_t)active;
    memcpy(&pkt->reserved[4], &preamp_x10, sizeof(preamp_x10));
    pkt->reserved[6] = (uint8_t)mixer.mode;
    pkt->reserved[7] = (uint8_t)mixer.balance;
    pkt->reserved[8] = mixer.width_pct;
    pkt->reserved[9] = mixer.crossfeed_level;
    memcpy(&pkt->reserved[10], &sr, sizeof(sr));
    return true;
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

    webhid_peq_packet_t pkt;
    if (!webhid_build_band_packet(0, PEQ_CH_LEFT, &pkt)) {
        return 0;
    }
    memcpy(buffer, &pkt, sizeof(pkt));
    return sizeof(pkt);
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
            peq_channel_t ch = (pkt->reserved[1] <= PEQ_CH_RIGHT) ? (peq_channel_t)pkt->reserved[1] : PEQ_CH_LINKED;
            if (pkt->band_idx == WEBHID_REPORT_SUB_MATRIX || pkt->band_idx == WEBHID_REPORT_SUB_EXT_STATE) {
                webhid_peq_send_band_report_ch(pkt->band_idx, ch);
            } else {
                uint8_t b_idx = (pkt->band_idx < PEQ_MAX_BANDS) ? pkt->band_idx : 0;
                webhid_peq_send_band_report_ch(b_idx, ch);
            }
            break;
        }

        case WEBHID_CMD_SET_BAND_LIVE: {
            if (pkt->band_idx < PEQ_MAX_BANDS && pkt->filter_type < PEQ_FILTER_TYPE_COUNT) {
                peq_channel_t ch = (pkt->reserved[1] <= PEQ_CH_RIGHT) ? (peq_channel_t)pkt->reserved[1] : PEQ_CH_LINKED;
                peq_band_param_t band = {
                    .type = (peq_filter_type_t)pkt->filter_type,
                    .freq_hz = (float)pkt->freq_hz,
                    .gain_db = (float)pkt->gain_db_x10 / 10.0f,
                    .q_factor = (float)pkt->q_x100 / 100.0f,
                    .enabled = (pkt->enabled != 0)
                };

                peq_dsp_update_user_band_ch(ch, pkt->band_idx, &band);
                oled_display_invalidate_curve();
                oled_display_notify_activity();
            }
            break;
        }

        case WEBHID_CMD_SET_BAND_COUNT: {
            uint8_t count = pkt->band_idx;
            peq_channel_t ch = (pkt->reserved[1] <= PEQ_CH_RIGHT) ? (peq_channel_t)pkt->reserved[1] : PEQ_CH_LINKED;
            if (count >= 1 && count <= PEQ_MAX_BANDS) {
                peq_dsp_set_band_count_ch(ch, count);
                oled_display_invalidate_curve();
                char msg[24];
                snprintf(msg, sizeof(msg), "%d Bands Active", count);
                oled_display_show_eq_overlay(msg);
            }
            break;
        }

        case WEBHID_CMD_SET_PREAMP_LIVE: {
            float preamp_db = (float)pkt->gain_db_x10 / 10.0f;
            peq_dsp_set_preamp(preamp_db);
            oled_display_invalidate_curve();
            oled_display_notify_activity();
            break;
        }

        case WEBHID_CMD_SELECT_PROFILE: {
            peq_preset_id_t slot = (peq_preset_id_t)pkt->band_idx;
            if (slot < PEQ_USER_SLOT_COUNT) {
                peq_dsp_select_preset(slot);
                char name[24] = {0};
                peq_dsp_get_current_preset(name);
                oled_display_set_eq_preset(name);
                oled_display_show_eq_overlay(name);
                ESP_LOGI(TAG, "WebHID switched active slot to [%d]: %s", (int)slot, name);
            }
            break;
        }

        case WEBHID_CMD_SAVE_TO_PRESET: {
            peq_dsp_save_user_profile_nvs();
            oled_display_show_eq_overlay("Saved to NVS");
            ESP_LOGI(TAG, "WebHID explicit commit: Active slot & dynamics saved to NVS.");
            break;
        }

        case WEBHID_CMD_RESET_TO_PRESET: {
            int cur_slot = (int)peq_dsp_get_current_preset(NULL);
            peq_dsp_reset_slot_to_flat(cur_slot);
            char name[24] = {0};
            peq_dsp_get_current_preset(name);
            oled_display_set_eq_preset(name);
            oled_display_show_eq_overlay("Reset Slot Flat");
            break;
        }

        case WEBHID_CMD_SET_AB_BYPASS: {
            bool bypass = (pkt->band_idx != 0);
            peq_dsp_set_ab_bypass(bypass);
            oled_display_invalidate_curve();
            oled_display_show_eq_overlay(bypass ? "A/B: EQ Bypass" : "A/B: EQ Active");
            break;
        }

        case WEBHID_CMD_SET_DUAL_MONO: {
            bool linked = (pkt->band_idx != 0);
            peq_dsp_set_linked_stereo(linked);
            oled_display_invalidate_curve();
            oled_display_show_eq_overlay(linked ? "Stereo Linked" : "Dual-Mono L/R");
            break;
        }

        case WEBHID_CMD_RENAME_SLOT: {
            uint8_t slot_id = pkt->band_idx;
            if (slot_id < PEQ_USER_SLOT_COUNT) {
                char new_name[17];
                memcpy(new_name, &pkt->reserved[0], 16);
                new_name[16] = '\0';
                peq_dsp_rename_slot(slot_id, new_name);
                if ((int)slot_id == (int)peq_dsp_get_current_preset(NULL)) {
                    char cur_name[24] = {0};
                    peq_dsp_get_current_preset(cur_name);
                    oled_display_set_eq_preset(cur_name);
                    oled_display_show_eq_overlay(cur_name);
                }
            }
            break;
        }

        case WEBHID_CMD_SET_DYNAMICS: {
            bool lim_en = (pkt->enabled & 0x01) != 0;
            bool fir_en = (pkt->enabled & 0x02) != 0;
            uint8_t loud = (pkt->filter_type < PEQ_LOUDNESS_COUNT) ? pkt->filter_type : 0;
            uint8_t view = (uint8_t)(pkt->band_idx & 0x03);

            peq_dsp_set_limiter_enabled(lim_en);
            peq_dsp_set_loudness_mode((peq_loudness_mode_t)loud);
            peq_dsp_set_fir_enabled(fir_en);
            peq_dsp_set_oled_view(view);
            oled_display_set_playback_view((oled_playback_view_t)view);
            oled_display_invalidate_curve();
            oled_display_show_eq_overlay("Dynamics Updated");
            break;
        }

        case WEBHID_CMD_SET_POWER_CFG: {
            uint16_t auto_mute = pkt->freq_hz;
            uint16_t oled_dim  = pkt->q_x100;
            peq_dsp_set_auto_mute_sec(auto_mute);
            peq_dsp_set_oled_dim_sec(oled_dim);
            oled_display_set_dim_sec(oled_dim);
            oled_display_show_eq_overlay("Power Cfg Set");
            break;
        }

        case WEBHID_CMD_FACTORY_RESET_SLOT: {
            uint8_t slot_id = pkt->band_idx;
            if (slot_id < PEQ_USER_SLOT_COUNT) {
                peq_dsp_reset_slot_to_flat(slot_id);
                if ((int)slot_id == (int)peq_dsp_get_current_preset(NULL)) {
                    char cur_name[24] = {0};
                    peq_dsp_get_current_preset(cur_name);
                    oled_display_set_eq_preset(cur_name);
                    oled_display_show_eq_overlay("Slot Reset");
                }
            }
            break;
        }

        case WEBHID_CMD_SET_HARMONICS: {
            uint8_t flags = pkt->band_idx;
            uint8_t bass_amt = (pkt->filter_type <= 100) ? pkt->filter_type : 100;
            uint8_t exc_amt = (pkt->enabled <= 100) ? pkt->enabled : 100;
            uint16_t bass_freq = pkt->freq_hz;
            uint16_t exc_freq = pkt->q_x100;

            if (bass_freq < 50) bass_freq = 50;
            if (bass_freq > 150) bass_freq = 150;
            if (exc_freq < 1500) exc_freq = 1500;
            if (exc_freq > 8000) exc_freq = 8000;

            uint8_t raw_mode = (flags >> 2) & 0x03;
            if (raw_mode >= (uint8_t)PEQ_EXCITER_MODE_COUNT) {
                raw_mode = (uint8_t)(PEQ_EXCITER_MODE_COUNT - 1);
            }

            peq_harmonics_cfg_t cfg = {
                .bass_enabled = (flags & 0x01) != 0,
                .bass_amount_pct = bass_amt,
                .bass_freq_hz = bass_freq,
                .exciter_enabled = (flags & 0x02) != 0,
                .exciter_mode = (peq_exciter_mode_t)raw_mode,
                .exciter_amount_pct = exc_amt,
                .exciter_freq_hz = exc_freq,
            };
            peq_dsp_set_harmonics_cfg(&cfg);
            oled_display_notify_activity();
            webhid_peq_send_band_report(WEBHID_REPORT_SUB_EXT_STATE);
            break;
        }

        case WEBHID_CMD_SET_MIXER: {
            peq_mixer_config_t cfg;
            memset(&cfg, 0, sizeof(cfg));
            cfg.mode = (pkt->band_idx < MIXER_MODE_COUNT) ? (mixer_mode_t)pkt->band_idx : MIXER_MODE_STEREO;
            cfg.balance = (int8_t)pkt->filter_type;
            cfg.width_pct = (pkt->enabled <= 200) ? pkt->enabled : 200;
            cfg.crossfeed_level = (uint8_t)(pkt->freq_hz <= 2 ? pkt->freq_hz : 2);
            memcpy(&cfg.custom_matrix[0][0], &pkt->reserved[0], sizeof(float) * 4);
            peq_dsp_set_mixer(&cfg);
            oled_display_show_eq_overlay("Mixer Updated");
            break;
        }

        case WEBHID_CMD_UPLOAD_FIR_CHUNK: {
            peq_channel_t ch = (pkt->band_idx <= PEQ_CH_RIGHT) ? (peq_channel_t)pkt->band_idx : PEQ_CH_LINKED;
            uint8_t count = (pkt->filter_type <= 12) ? pkt->filter_type : 0;
            bool commit = (pkt->enabled & 0x01) != 0;
            uint16_t total_taps = pkt->freq_hz;
            uint16_t offset = pkt->q_x100;
            float taps_buf[12] = {0};
            if (count > 0) {
                memcpy(taps_buf, &pkt->reserved[0], sizeof(float) * count);
            }
            esp_err_t ferr = peq_dsp_upload_fir_chunk(ch, total_taps, offset, count, taps_buf, commit);
            if (ferr == ESP_OK && (total_taps == 0 || (uint32_t)offset + count >= total_taps)) {
                oled_display_show_eq_overlay(total_taps == 0 ? "FIR Cleared" : "FIR Loaded");
            }
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
            peq_band_param_t band = {
                .type = (peq_filter_type_t)filter_type,
                .freq_hz = freq,
                .gain_db = gain,
                .q_factor = q,
                .enabled = true
            };
            peq_dsp_update_user_band(band_idx, &band);
            oled_display_invalidate_curve();
        } else if (cmd == 0x02) {
            peq_dsp_save_user_profile_nvs();
            oled_display_show_eq_overlay("Saved to NVS");
        }
    }
}

// =============================================================================
// Public API
// =============================================================================

esp_err_t webhid_peq_init(void)
{
    ESP_LOGI(TAG, "Initializing TinyUSB Async UAC 1.0 (24-bit 44.1/48 kHz) + WebHID on GPIO %d/%d...",
             BOARD_USB_DM_PIN, BOARD_USB_DP_PIN);

#ifdef CONFIG_PM_ENABLE
    esp_pm_config_t pm_cfg = {
        .max_freq_mhz = 240,
        .min_freq_mhz = 80,
        .light_sleep_enable = false
    };
    esp_err_t pm_ret = esp_pm_configure(&pm_cfg);
    if (pm_ret == ESP_OK) {
        if (esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "dac_dsp_max", &s_pm_lock) == ESP_OK && s_pm_lock) {
            esp_pm_lock_acquire(s_pm_lock);
            s_pm_lock_acquired = true;
        }
    } else {
        ESP_LOGW(TAG, "esp_pm_configure warning: %s", esp_err_to_name(pm_ret));
    }
#endif

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

    ESP_LOGI(TAG, "TinyUSB Async UAC 1.0 + WebHID initialized (VID: 0x%04X, PID: 0x%04X).",
             s_device_desc.idVendor, s_device_desc.idProduct);
    return ESP_OK;
}

esp_err_t webhid_peq_send_band_report_ch(uint8_t band_idx, peq_channel_t ch)
{
    if (band_idx != WEBHID_REPORT_SUB_MATRIX &&
        band_idx != WEBHID_REPORT_SUB_EXT_STATE &&
        band_idx >= PEQ_MAX_BANDS) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!tud_hid_n_ready(0)) return ESP_ERR_NOT_FOUND;

    webhid_peq_packet_t pkt;
    if (!webhid_build_band_packet(band_idx, ch, &pkt)) {
        return ESP_FAIL;
    }

    bool res = tud_hid_n_report(0, 0, &pkt, sizeof(pkt));
    return res ? ESP_OK : ESP_FAIL;
}

esp_err_t webhid_peq_send_band_report(uint8_t band_idx)
{
    return webhid_peq_send_band_report_ch(band_idx, PEQ_CH_LEFT);
}

esp_err_t webhid_peq_notify_state_changed(void)
{
    return webhid_peq_send_band_report(WEBHID_REPORT_SUB_EXT_STATE);
}

bool webhid_peq_is_mounted(void)
{
    return atomic_load_explicit(&s_is_mounted, memory_order_acquire);
}

bool webhid_peq_is_suspended(void)
{
    return atomic_load_explicit(&s_is_suspended, memory_order_acquire);
}

bool webhid_peq_is_streaming(void)
{
    return atomic_load_explicit(&s_is_streaming, memory_order_acquire);
}

bool webhid_peq_consume_pending_sample_rate(uint32_t *out_sample_rate)
{
    bool pending = false;
    uint32_t rate = 48000;
    taskENTER_CRITICAL(&s_uac_rate_lock);
    if (s_sample_rate_pending) {
        pending = true;
        rate = s_pending_sample_rate;
        s_sample_rate_pending = false;
    }
    taskEXIT_CRITICAL(&s_uac_rate_lock);
    if (pending && out_sample_rate) {
        *out_sample_rate = rate;
    }
    return pending;
}

uint32_t webhid_peq_get_sample_rate(void)
{
    taskENTER_CRITICAL(&s_uac_rate_lock);
    uint32_t sr = s_audio_sample_rate;
    taskEXIT_CRITICAL(&s_uac_rate_lock);
    return sr;
}

static inline void audio_spsc_sync_flush_consumer(void)
{
    uint32_t gen = atomic_load_explicit(&s_audio_spsc.flush_gen, memory_order_acquire);
    if (gen != s_audio_spsc.cons_gen) {
        uint32_t wr = atomic_load_explicit(&s_audio_spsc.wr_frames, memory_order_acquire);
        atomic_store_explicit(&s_audio_spsc.rd_frames, wr, memory_order_release);
        s_audio_spsc.cons_gen = gen;
    }
}

uint16_t webhid_peq_audio_available(void)
{
    audio_spsc_sync_flush_consumer();
    uint32_t avail_frames = audio_spsc_level_frames();
    uint32_t avail_bytes = avail_frames * AUDIO_BYTES_PER_FRAME;
    return (avail_bytes > UINT16_MAX) ? UINT16_MAX : (uint16_t)avail_bytes;
}

uint16_t webhid_peq_audio_read(void *buffer, uint16_t bufsize)
{
    if (!buffer || bufsize < AUDIO_BYTES_PER_FRAME) {
        return 0;
    }
    audio_spsc_sync_flush_consumer();

    uint32_t req_frames = bufsize / AUDIO_BYTES_PER_FRAME;
    uint32_t rd = atomic_load_explicit(&s_audio_spsc.rd_frames, memory_order_relaxed);
    uint32_t wr = atomic_load_explicit(&s_audio_spsc.wr_frames, memory_order_acquire);
    uint32_t avail = wr - rd;
    if (req_frames > avail) {
        req_frames = avail;
    }
    if (req_frames == 0) {
        return 0;
    }

    uint32_t idx = rd & AUDIO_SPSC_FRAME_MASK;
    uint32_t first = AUDIO_SPSC_FRAME_CAP - idx;
    if (first > req_frames) {
        first = req_frames;
    }

    uint8_t *dst = (uint8_t *)buffer;
    memcpy(dst, &s_audio_spsc.data[idx * AUDIO_BYTES_PER_FRAME],
           first * AUDIO_BYTES_PER_FRAME);

    uint32_t second = req_frames - first;
    if (second > 0) {
        memcpy(dst + (first * AUDIO_BYTES_PER_FRAME),
               &s_audio_spsc.data[0],
               second * AUDIO_BYTES_PER_FRAME);
    }

    // Release fence ensures Core 1 finishes copying before Core 0 can reuse those slots
    atomic_store_explicit(&s_audio_spsc.rd_frames, rd + req_frames, memory_order_release);
    return (uint16_t)(req_frames * AUDIO_BYTES_PER_FRAME);
}
