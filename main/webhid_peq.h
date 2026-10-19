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
    WEBHID_CMD_SELECT_PROFILE     = 0x04,  // Switch active slot (0=User, 1=Flat, 2=Bass Boost, 3=Harman)
    WEBHID_CMD_SAVE_TO_PRESET     = 0x05,  // Explicit flash commit to target preset slot (Delta-checked)
    WEBHID_CMD_RESET_TO_PRESET    = 0x06,  // Reset user slot from factory preset
    WEBHID_CMD_SET_BAND_COUNT     = 0x07,  // Dynamically configure active band count (1 to PEQ_MAX_BANDS)
} webhid_cmd_t;

// Packed 64-byte raw HID report matching docs/guides/usb_dac_guide.md and browser JS client
typedef struct __attribute__((packed)) {
    uint16_t magic;         // WEBHID_MAGIC_16 (0x4551)
    uint8_t  cmd;           // webhid_cmd_t
    uint8_t  band_idx;      // 0 to 9 (or band count for CMD_SET_BAND_COUNT, or slot for CMD_SELECT_PROFILE)
    uint8_t  filter_type;   // 0=Bypass, 1=Peaking, 2=LowShelf, 3=HighShelf
    uint8_t  enabled;       // 1=Active, 0=Bypassed
    uint16_t freq_hz;       // 20 to 20,000 Hz
    int16_t  gain_db_x10;   // e.g. +35 = +3.5 dB, -45 = -4.5 dB
    uint16_t q_x100;        // e.g. 141 = 1.41, 71 = 0.71
    uint8_t  reserved[52];  // Zero-padded to exactly 64 bytes (reserved[0] = active num_bands)
} webhid_peq_packet_t;


/**
 * @brief Initialize TinyUSB stack with Generic In/Out HID report descriptor for WebHID.
 *
 * Configures the Native USB OTG peripheral on GPIO 19 (D-) and GPIO 20 (D+).
 * Runs the TinyUSB event processing task pinned to Core 0.
 *
 * @return esp_err_t ESP_OK on success.
 */
esp_err_t webhid_peq_init(void);

/**
 * @brief Send active PEQ band profile to connected WebHID host.
 *
 * @param band_idx Band index to report (0 to PEQ_NUM_BANDS - 1).
 * @return esp_err_t ESP_OK if report queued successfully.
 */
esp_err_t webhid_peq_send_band_report(uint8_t band_idx);

/**
 * @brief Query if a USB host is currently mounted and active.
 *
 * @return true if USB host is connected and mounted.
 */
bool webhid_peq_is_mounted(void);

/**
 * @brief Query if UAC 1.0 audio streaming is currently active (Alt Setting 1).
 *
 * @return true if host is actively streaming audio.
 */
bool webhid_peq_is_streaming(void);

/**
 * @brief Query number of PCM bytes available in the UAC 1.0 FIFO buffer.
 *
 * @return Number of bytes ready to read.
 */
uint16_t webhid_peq_audio_available(void);

/**
 * @brief Read PCM audio data from the UAC 1.0 FIFO buffer.
 *
 * @param buffer Destination buffer for interleaved 16-bit stereo samples.
 * @param bufsize Size of buffer in bytes.
 * @return Number of bytes actually read.
 */
uint16_t webhid_peq_audio_read(void *buffer, uint16_t bufsize);

#ifdef __cplusplus
}
#endif
