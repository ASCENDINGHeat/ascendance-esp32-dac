# MicroSD (SDMMC) Lossless Audio Player Guide & Agentic AI Reference

This guide provides the complete technical specification, peripheral configuration, memory buffering architecture, and implementation roadmap for adding **MicroSD Card local audio playback** (supporting FLAC, WAV, and MP3) to the ESP32-S3 audio player via the hardware **SDMMC Host** controller.

> **Note for Agentic AI / AI Assistants:**  
> This document details ESP-IDF SDMMC slot configurations, FATFS mounting routines, ringbuffer sizing for SD latency mitigation, multi-format decoder pipelines (`dr_flac`, `dr_wav`, `minimp3`), and a step-by-step checklist for autonomous firmware generation.

---

## 1. SDMMC vs SPI Mode Comparison

The ESP32-S3 features an internal hardware **SDMMC Host** peripheral capable of 1-bit and 4-bit bus widths.

| Specification | SDMMC (4-Bit) | SDMMC (1-Bit) | SPI Mode (SD SPI) |
| :--- | :--- | :--- | :--- |
| **Bus Width** | 4 data lines (D0–D3) | 1 data line (D0) | 1 MOSI / 1 MISO |
| **Pins Required** | 6 pins (CLK, CMD, D0–D3) | 3 pins (CLK, CMD, D0) | 4 pins (SCK, MOSI, MISO, CS) |
| **Clock Frequency** | 20 MHz – 40 MHz | 20 MHz – 40 MHz | 10 MHz – 20 MHz |
| **Max Throughput** | ~10 MB/s – 20 MB/s | ~2.5 MB/s – 5 MB/s | ~1.2 MB/s – 2 MB/s |
| **CPU Overhead** | Extremely Low (Hardware DMA) | Extremely Low (Hardware DMA) | Moderate (SPI byte polling) |
| **Recommended Use** | High-Res FLAC (24-bit/96kHz), Fast Directory Indexing | Standard FLAC/MP3, Breadboards with limited GPIOs | Legacy fallback only |

* **1-Bit SDMMC** is recommended for standard setups because it only requires **3 GPIOs** (CLK, CMD, D0) while still delivering >2.5 MB/s—more than 14x faster than uncompressed CD-quality audio (176.4 KB/s).
* **4-Bit SDMMC** can be enabled when ultra-fast directory caching and 24-bit/96kHz or 192kHz multi-track FLAC streaming is required.

---

## 2. Hardware Pinout & Interconnection

### 2.1 MicroSD Breakout to YD-ESP32-S3 Pin Mapping

| MicroSD Module Pin | ESP32-S3 GPIO | SDMMC Signal | Pull-up Resistor Requirement |
| :--- | :--- | :--- | :--- |
| **CLK** | **GPIO 14** | `CLK` | None (driven by MCU clock output) |
| **CMD** | **GPIO 13** | `CMD` | **10 kΩ pull-up to 3.3V** (external or internal) |
| **D0** | **GPIO 21** | `DATA 0` | **10 kΩ pull-up to 3.3V** (external or internal) |
| **D1** *(4-bit mode)* | **GPIO 47** | `DATA 1` | **10 kΩ pull-up to 3.3V** |
| **D2** *(4-bit mode)* | **GPIO 38** | `DATA 2` | **10 kΩ pull-up to 3.3V** |
| **D3 / CS** *(4-bit mode)* | **GPIO 39** | `DATA 3` | **10 kΩ pull-up to 3.3V** |
| **VCC** | **3.3V (or 5V)** | Power Supply | 3.3V if breakout lacks LDO; 5V if breakout has 3.3V LDO |
| **GND** | **GND** | Common Ground | Tie to system GND |
| **CD / DET** *(optional)* | **GPIO 40** | Card Detect Switch | Active-low; internal pull-up enabled |

> [!IMPORTANT]
> **SD Card Pull-Up Resistors:** The SD specification strictly mandates pull-up resistors (10 kΩ to 3.3V) on `CMD` and all `DATA` lines. Many generic MicroSD breakout boards (especially those designed for Arduino 5V SPI) include built-in resistor arrays. If using a raw MicroSD socket or bare breakout, solder external 10 kΩ resistors to 3.3V.

### 2.2 Verified Hardware Schematic Reference

The complete electrical schematic wiring for the MicroSD local player (Slot 1, 1-bit bus) alongside the DAC, headphone amplifier, OLED display, rotary encoder, and mode switch is verified in:
* **Vector Schematic:** [`docs/DSP+DAC+LOCAL.svg`](../schematics/DSP+DAC+LOCAL.svg) (also [`docs/ESP32-S3-DSP-DAC with local player.svg`](../schematics/ESP32-S3-DSP-DAC%20with%20local%20player.svg))
* **Detailed Analysis & Net Matrix:** See [Hardware Schematics Guide Section 4](../hardware/schematics.md#4-schematic-3-verification-complete-dspdac-with-local-player-display--controls-dspdaclocalsvg--esp32-s3-dsp-dac-with-local-playersvg).

---

## 3. Storage Architecture: FATFS & Directory Layout

### 3.1 Partition & Mount Configuration
The firmware mounts the card at `/sdcard` using the ESP-IDF Virtual Filesystem (VFS) FAT component:

```c
#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"

#define MOUNT_POINT "/sdcard"

esp_err_t mount_sdcard(bool use_4bit_mode)
{
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_DEFAULT; // 20 MHz (or SDMMC_FREQ_HIGHSPEED for 40 MHz)

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = use_4bit_mode ? 4 : 1;
    slot_config.clk = GPIO_NUM_14;
    slot_config.cmd = GPIO_NUM_13;
    slot_config.d0  = GPIO_NUM_21;
    if (use_4bit_mode) {
        slot_config.d1 = GPIO_NUM_47;
        slot_config.d2 = GPIO_NUM_38;
        slot_config.d3 = GPIO_NUM_39;
    }
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    sdmmc_card_t *card;
    esp_err_t ret = esp_vfs_fat_sdmmc_mount(MOUNT_POINT, &host, &slot_config, &mount_config, &card);
    if (ret != ESP_OK) {
        ESP_LOGE("SDMMC", "Failed to mount SD card (%s)", esp_err_to_name(ret));
        return ret;
    }

    sdmmc_card_print_info(stdout, card);
    return ESP_OK;
}
```

### 3.2 Recommended MicroSD File System Structure
```
/sdcard/
├── music/
│   ├── Pink Floyd/
│   │   └── 01 - Speak to Me.flac
│   ├── Daft Punk/
│   │   └── 02 - Giorgio by Moroder.flac
│   └── Radiohead/
│       └── 01 - Airbag.mp3
├── playlists/
│   └── favorites.m3u
└── eq_presets/
    └── Sennheiser_HD600.json
```

---

## 4. Audio Pipeline & Memory Buffering Architecture

SD cards exhibit **latency spikes** during read operations (often 50 ms to 150 ms when crossing cluster boundaries or executing internal wear-leveling flash operations). To prevent audio stuttering or DMA underflow:

```
┌────────────────────────────────────────────────────────┐
│   Task 1: SD File Reader (Core 0, Priority 5)           │
│   • Reads 16 KB chunks from /sdcard/music/...          │
│   • Writes encoded bytes to PSRAM Ringbuffer           │
└───────────────────────────┬────────────────────────────┘
                            │
                            ▼ 64 KB – 128 KB Encoded Buffer (PSRAM)
┌────────────────────────────────────────────────────────┐
│   Task 2: Audio Decoder Task (Core 1, Priority 10)     │
│   • minimp3 (MP3) / dr_flac (FLAC) / dr_wav (WAV)      │
│   • Decodes frames into 16-bit / 24-bit stereo PCM     │
└───────────────────────────┬────────────────────────────┘
                            │
                            ▼ PCM Buffer
┌────────────────────────────────────────────────────────┐
│   Task 3: PEQ DSP & I2S DMA Feeder                     │
│   • Biquad IIR Filter Bank (see parametric_eq_guide)   │
│   • i2s_channel_write() to PCM5102A DAC                │
└────────────────────────────────────────────────────────┘
```

### 4.1 Ringbuffer Watermark Strategy
* **Total Buffer Allocation:** 128 KB allocated in Octal PSRAM via `heap_caps_malloc(128 * 1024, MALLOC_CAP_SPIRAM)`.
* **Pre-roll Threshold:** Audio playback does not begin until the ringbuffer has accumulated at least **32 KB** of encoded data (~1.5 seconds of 16-bit 44.1kHz stereo MP3, or ~300 ms of FLAC).
* **Underflow Recovery:** If the buffer level drops below 4 KB, the decoder holds the last audio frame with a gentle 5 ms cosine ramp-down to silence, preventing a harsh pop while the SD task catches up.

---

## 5. Multi-Format Decoder Engines

### 5.1 MP3 Decoding (`minimp3`)
* Single-header lightweight decoder already implemented in [`main/minimp3.h`](../../main/minimp3.h).
* Scratchpad must use static BSS memory allocation to prevent FreeRTOS task stack smashing:
  ```c
  static mp3dec_scratch_t s_scratch;
  mp3dec_decode_frame(&s_mp3d, input_buf, input_size, pcm_out, &info);
  ```

### 5.2 FLAC Decoding (`dr_flac`)
* `dr_flac` is a single-file public-domain C library for lossless FLAC decoding:
  ```c
  #define DR_FLAC_IMPLEMENTATION
  #define DR_FLAC_NO_STDIO // Use custom stream read callbacks connected to PSRAM ringbuffer
  #include "dr_flac.h"

  drflac* pFlac = drflac_open(my_flac_read_proc, my_flac_seek_proc, pUserData, NULL);
  drflac_uint64 framesRead = drflac_read_pcm_frames_s16(pFlac, FRAME_CHUNK_SIZE, pcm_out_buffer);
  ```
* Supports 16-bit and 24-bit audio depths. If 24-bit samples are decoded, scale down to 16-bit for PCM5102A standard I2S (`sample16 = sample32 >> 8`) or configure I2S for 32-bit slot width.

### 5.3 WAV Decoding (`dr_wav`)
* `dr_wav` supports uncompressed linear PCM (8, 16, 24, 32-bit integer and 32-bit float).
* Uncompressed WAV reads directly without significant CPU utilization, leaving >90% of Core 1 free for multi-band Parametric EQ filtering.

## 6. Reference for AI Agents: Implementation Checklist & Invariants

### 6.1 SDMMC Storage Invariants
1. **Pull-Up Resistors:** SDMMC command line (`GPIO 13`) and data lines (`GPIO 21`, `GPIO 47`, `GPIO 38`, `GPIO 39`) require 10 kΩ pull-ups to 3.3V. Always include `slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;` in software as an additional fail-safe.
2. **Cluster Alignment:** When reading from FATFS, read in chunks aligned to 4 KB, 8 KB, or 16 KB (`16384` bytes) to prevent multi-cluster partial reads and reduce SD card latency spikes.
3. **Decoupling Task Separation:** Never perform direct FATFS `f_read()` calls inside the real-time audio task (`audio_stream_task`). Always separate into an SD reader task on Core 0 and an audio decoder task on Core 1 linked by a PSRAM ringbuffer.

### 6.2 Implementation Checklist
Autonomous agents implementing SD card playback should follow this sequence:

- [ ] **Step 1: Enable FATFS & SDMMC in Kconfig (`sdkconfig.defaults`)**
  ```ini
  CONFIG_FATFS_VFS_ALL_BUFFERS_PSRAM=y
  CONFIG_FATFS_MAX_LFN=255
  CONFIG_FATFS_API_ENCODING_UTF_8=y
  CONFIG_FATFS_CODEPAGE_437=y
  ```
- [ ] **Step 2: Add Single-Header Decoders to `main/`**
  * Include [`minimp3.h`](../../main/minimp3.h) (already present).
  * Download `dr_flac.h` and `dr_wav.h` into `main/decoders/`.
- [ ] **Step 3: Implement Directory Indexer (`sd_scanner.c`)**
  * Implement recursive directory traversal using POSIX `opendir()` and `readdir()`.
  * Filter for extensions: `.mp3`, `.flac`, `.wav`, `.m3u`.
  * Store track metadata (file path, file size, duration) in a dynamic array in PSRAM.
- [ ] **Step 4: Implement Reader Task (`sd_reader_task`)**
  * Pinned to **Core 0**, stack size **4096 bytes**.
  * Reads in blocks of `16384` bytes (`16 KB`) to optimize FAT cluster read efficiency.
- [ ] **Step 5: Integrate with I2S Dynamic Sample Rate Switching**
  * Read sample rate from file headers (e.g. 44100 Hz, 48000 Hz, 96000 Hz).
  * Call `i2s_channel_disable()`, update clock with `i2s_output_set_sample_rate(rate)`, and call `i2s_channel_enable()`.
