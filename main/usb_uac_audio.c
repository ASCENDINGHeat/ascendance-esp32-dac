/*
 * ESP32-S3 USB Composite UAC 1.0 Asynchronous Audio & TinyUSB Stack
 *
 * Isolates hard real-time IRAM_ATTR USB audio ISR, 10.14 Async feedback,
 * lock-free SPSC ring buffer, and composite USB descriptors from the
 * 64-byte WebHID protocol router (main/webhid_peq.c).
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
static DRAM_ATTR _Atomic uint32_t s_audio_sample_rate = 48000;
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
    _Atomic uint32_t wr_frames; // Written only by Core 0 (TinyUSB ISR)
    _Atomic uint32_t rd_frames; // Written only by Core 1 (audio_dsp_task)
    _Atomic uint32_t flush_gen; // Incremented by Core 0 on stream stop/start/reclock
    uint32_t cons_gen;          // Owned exclusively by Core 1
} audio_spsc_t;

static DRAM_ATTR audio_spsc_t s_audio_spsc;

static inline uint32_t IRAM_ATTR audio_spsc_level_frames(void)
{
    uint32_t wr = atomic_load_explicit(&s_audio_spsc.wr_frames, memory_order_acquire);
    uint32_t rd = atomic_load_explicit(&s_audio_spsc.rd_frames, memory_order_acquire);
    return wr - rd;
}

static inline void audio_spsc_request_flush(void)
{
    atomic_fetch_add_explicit(&s_audio_spsc.flush_gen, 1U, memory_order_release);
}

// Producer: Core 0 only (TinyUSB RX ISR)
static uint32_t IRAM_ATTR audio_spsc_push_frames(const uint8_t *src, uint32_t num_frames)
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

// Compute and push I2S-DMA-coupled PI + EMA-filtered 10.14 async feedback to EP 0x81 IN.
// CRITICAL: Called from TinyUSB hardware ISR (tud_audio_rx_done_isr) -> MUST use pure integer math
// (NO float/FPU, NO taskENTER_CRITICAL, NO ESP_LOG) to avoid Xtensa CoprocessorException!
static void IRAM_ATTR webhid_update_async_feedback(bool nominal_only)
{
#if CFG_TUD_AUDIO_ENABLE_EP_OUT && CFG_TUD_AUDIO_ENABLE_FEEDBACK_EP
    static DRAM_ATTR int32_t s_err_ema_q8 = 0;       // Q24.8 EMA-filtered SPSC error (frames * 256)
    static DRAM_ATTR int32_t s_integrator_q8 = 0;    // Q24.8 integral term (10.14 LSBs * 256)
    static DRAM_ATTR uint32_t s_prev_dma_frames = 0; // Last observed I2S DMA cumulative frame count
    static DRAM_ATTR uint8_t s_dma_idle_ms = 20;     // Counts 1 ms RX ticks since last I2S DMA completion

    uint32_t sr = atomic_load_explicit(&s_audio_sample_rate, memory_order_relaxed);
    // Exact Full-Speed UAC 1.0 nominal 10.14 rate: (sr * 2^14) / 1000
    // 48000 Hz -> 786432 (48.000 * 16384), 44100 Hz -> 722534 (44.100 * 16384)
    int32_t nom_10_14 = (int32_t)(((uint64_t)sr << 14) / 1000ULL);
    int32_t fb_10_14 = nom_10_14;

    uint32_t dma_frames = 0;
    audio_pipeline_get_dma_sync(&dma_frames, NULL, NULL);

    if (nominal_only) {
        s_err_ema_q8 = 0;
        s_integrator_q8 = 0;
        s_prev_dma_frames = dma_frames;
        s_dma_idle_ms = 20;
    } else {
        const int32_t target_frames = (int32_t)(AUDIO_SPSC_FRAME_CAP / 2U); // 1024 frames
        const int32_t cur_frames = (int32_t)audio_spsc_level_frames();
        const int32_t raw_err_q8 = (target_frames - cur_frames) << 8; // >0 => host sends slightly faster

        // Pure-integer EMA low-pass filter (alpha = 1/16, tau ~16 ms) removes 256-frame DMA burst ripple
        s_err_ema_q8 += (raw_err_q8 - s_err_ema_q8) >> 4;
        int32_t err_frames = s_err_ema_q8 >> 8;

        // Track I2S DMA liveness (256-frame descriptors complete every ~5.3 ms @ 48 kHz)
        if (dma_frames != s_prev_dma_frames) {
            s_prev_dma_frames = dma_frames;
            s_dma_idle_ms = 0;
        } else if (s_dma_idle_ms < 20) {
            s_dma_idle_ms++;
        }

        // Only integrate while I2S DMA is actively shifting frames (prevents windup on mute/reclock/stall)
        int32_t abs_err = (err_frames < 0) ? -err_frames : err_frames;
        if (s_dma_idle_ms < 20 && abs_err < (target_frames - (target_frames >> 3))) {
            // Ki = 4/256 = 0.015625 LSB_10.14 / (frame * ms), anti-windup clamp +/- 4096 LSB (+/- 0.25 sample/ms)
            s_integrator_q8 += err_frames * 4;
            const int32_t int_limit_q8 = (1 << 12) << 8; // +/- 4096 * 256
            if (s_integrator_q8 > int_limit_q8)  s_integrator_q8 = int_limit_q8;
            if (s_integrator_q8 < -int_limit_q8) s_integrator_q8 = -int_limit_q8;
        }

        // Proportional (Kp = 4 LSB_10.14 / frame) + Integral control
        int32_t adj = (err_frames * 4) + (s_integrator_q8 >> 8);
        fb_10_14 = nom_10_14 + adj;

        // Hard clamp to nominal +/- 1.0 sample/ms to prevent host starvation or feedback jitter blowout
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
        webhid_update_async_feedback(true);
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
    feedback_param->sample_freq = webhid_peq_get_sample_rate();
}
#endif

bool IRAM_ATTR tud_audio_rx_done_isr(uint8_t rhport, uint16_t n_bytes_received, uint8_t func_id,
                                     uint8_t ep_out, uint8_t cur_alt_setting)
{
    (void)rhport;
    (void)n_bytes_received;
    (void)func_id;
    (void)ep_out;
    (void)cur_alt_setting;

    // Drain TinyUSB software RX FIFO strictly on Core 0 ISR into our lock-free C11 SPSC ring buffer
    static DRAM_ATTR uint8_t s_rx_tmp[294];
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

    // Resync guard: if a partial FIFO overrun ever left 1..5 orphaned bytes, discard them
    // immediately so the 24-bit stereo byte phase (L0 L1 L2 R0 R1 R2) can never shift.
    if (avail > 0 && avail < AUDIO_BYTES_PER_FRAME) {
        uint8_t trash[AUDIO_BYTES_PER_FRAME];
        (void)tud_audio_read(trash, avail);
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
            uint32_t sr = atomic_load_explicit(&s_audio_sample_rate, memory_order_acquire);
            uint8_t freq[3];
            freq[0] = (uint8_t)(sr & 0xFF);
            freq[1] = (uint8_t)((sr >> 8) & 0xFF);
            freq[2] = (uint8_t)((sr >> 16) & 0xFF);
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
            if (req_rate != atomic_load_explicit(&s_audio_sample_rate, memory_order_relaxed)) {
                atomic_store_explicit(&s_audio_sample_rate, req_rate, memory_order_release);
                s_pending_sample_rate = req_rate;
                s_sample_rate_pending = true;
                changed = true;
            }
            taskEXIT_CRITICAL(&s_uac_rate_lock);
            if (changed) {
                audio_spsc_request_flush();
            }
            // Always prime nominal 10.14 feedback even if rate unchanged (hosts often send SET_CUR right before Alt 1)
            webhid_update_async_feedback(true);
            ESP_LOGI(TAG, "UAC1 Host Set Sample Rate: %lu Hz (24-bit Async)", (unsigned long)req_rate);
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
    return atomic_load_explicit(&s_audio_sample_rate, memory_order_acquire);
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
