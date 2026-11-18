/*
 * ESP32-S3 WebHID Live Parametric EQ Tuning Subsystem
 *
 * Provides a 100% driverless, offline, browser-based live PEQ tuning interface
 * over the Native USB OTG port (GPIO 19: D-, GPIO 20: D+) using TinyUSB.
 * Works seamlessly with Google Chrome, Microsoft Edge, Opera, and Android Chrome
 * via the standard WebHID API (navigator.hid).
 *
 * References:
 *   - docs/guides/usb_dac_guide.md (Section 5)
 *   - docs/software_functions/04_usb_audio_webhid.md
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "peq_dsp.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WEBHID_REPORT_SIZE   64
#define WEBHID_MAGIC_16      0x4551  // "EQ" in ASCII (Little Endian: 0x51, 0x45)
#define WEBHID_MAGIC_BYTE    0x45    // 'E' (0x45)

typedef enum {
    WEBHID_CMD_GET_ACTIVE_PROFILE = 0x01,  // Request current settings / profile
    WEBHID_CMD_SET_BAND_LIVE      = 0x02,  // Real-time slider update (volatile SRAM only, 0 flash wear)
    WEBHID_CMD_SET_PREAMP_LIVE    = 0x03,  // Preamp headroom change (volatile SRAM only)
    WEBHID_CMD_SELECT_PROFILE     = 0x04,  // Switch active slot (0=Flat, 1=Bass Boost, 2=Harman, 3=User Custom)
    WEBHID_CMD_SAVE_TO_PRESET     = 0x05,  // Explicit flash commit to target preset slot (Delta-checked)
    WEBHID_CMD_RESET_TO_PRESET    = 0x06,  // Reset user slot from factory preset
    WEBHID_CMD_SET_BAND_COUNT     = 0x07,  // Dynamically configure active band count (1 to PEQ_MAX_BANDS)
    WEBHID_CMD_SET_AB_BYPASS      = 0x08,  // Toggle instant preamp-normalized A/B bypass (band_idx: 0/1)
    WEBHID_CMD_SET_DUAL_MONO      = 0x09,  // Toggle linked stereo vs independent L/R (band_idx: 1=linked, 0=dual-mono)
    WEBHID_CMD_SET_MIXER          = 0x13,  // Configure 2x2 Channel Matrix & Chu Moy Crossfeed
} webhid_cmd_t;

// Packed 64-byte raw HID report matching browser WebHID client
typedef struct __attribute__((packed)) {
    uint16_t magic;         // WEBHID_MAGIC_16 (0x4551)
    uint8_t  cmd;           // webhid_cmd_t
    uint8_t  band_idx;      // 0..9 (or count, slot, toggle, mixer mode; 0xFE = query custom matrix)
    uint8_t  filter_type;   // 0=Bypass, 1=Peak, 2=LowShelf, 3=HighShelf, 4=HPF, 5=LPF, 6=Notch, 7=BPF
    uint8_t  enabled;       // 1=Active, 0=Bypassed (or width_pct for CMD_SET_MIXER)
    uint16_t freq_hz;       // 20 to 20,000 Hz (or crossfeed_level for CMD_SET_MIXER)
    int16_t  gain_db_x10;   // e.g. +35 = +3.5 dB, -45 = -4.5 dB
    uint16_t q_x100;        // e.g. 141 = 1.41, 71 = 0.71
    uint8_t  reserved[52];  // Bytes 12..63 (metadata, channel selector, mixer state, custom matrix floats)
} webhid_peq_packet_t;


/**
 * @brief Initialize TinyUSB stack with Composite UAC 1.0 (24-bit 44.1/48/96 kHz) + WebHID.
 *
 * Configures the Native USB OTG peripheral on GPIO 19 (D-) and GPIO 20 (D+).
 * Runs the TinyUSB event processing task pinned to Core 0.
 *
 * @return esp_err_t ESP_OK on success.
 */
esp_err_t webhid_peq_init(void);

/**
 * @brief Send active PEQ band profile to connected WebHID host for the given channel.
 *
 * @param band_idx Band index (0..9) or 0xFE for mixer custom matrix report.
 * @param ch Channel target (PEQ_CH_LINKED, PEQ_CH_LEFT, PEQ_CH_RIGHT).
 * @return esp_err_t ESP_OK if report queued successfully.
 */
esp_err_t webhid_peq_send_band_report_ch(uint8_t band_idx, peq_channel_t ch);

/**
 * @brief Send active PEQ band profile (primary/left bank) to connected WebHID host.
 */
esp_err_t webhid_peq_send_band_report(uint8_t band_idx);

/**
 * @brief Query if a USB host is currently mounted and active.
 */
bool webhid_peq_is_mounted(void);

/**
 * @brief Query if UAC 1.0 audio streaming is currently active (Alt Setting 1).
 */
bool webhid_peq_is_streaming(void);

/**
 * @brief Consume a pending USB sample-rate switch requested by the host on Core 0.
 *
 * Intended to be polled by Core 1 (audio_dsp_task) so I2S reclocking happens safely
 * outside active DMA writes with zero-pop hardware muting.
 *
 * @param out_sample_rate Receives target sample rate in Hz (44100, 48000, or 96000).
 * @return true if a rate change is pending, false otherwise.
 */
bool webhid_peq_consume_pending_sample_rate(uint32_t *out_sample_rate);

/**
 * @brief Get the currently negotiated UAC 1.0 sample rate in Hz.
 */
uint32_t webhid_peq_get_sample_rate(void);

/**
 * @brief Query number of PCM bytes available in the UAC 1.0 FIFO buffer.
 */
uint16_t webhid_peq_audio_available(void);

/**
 * @brief Read PCM audio data from the UAC 1.0 FIFO buffer.
 */
uint16_t webhid_peq_audio_read(void *buffer, uint16_t bufsize);

#ifdef __cplusplus
}
#endif

