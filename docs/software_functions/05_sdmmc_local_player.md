# Module 05: MicroSD (SDMMC) Local Player Function Specifications

> **Module Identifier:** `SDMMC_LOCAL_PLAYER`  
> **Primary References:** [`sdmmc_player_guide.md`](../research/sdmmc_player_guide.md), ESP-IDF SDMMC Host API  
> **ESP-IDF Components:** `esp_driver_sdmmc`, `fatfs`, `vfs`  
> **Decoders:** `dr_flac` (FLAC 16/24-bit), `dr_wav` (RIFF WAV), `minimp3` (MP3)  
> **Hardware Configuration:** Slot 1, 1-bit bus (`GPIO 14 CLK`, `GPIO 13 CMD`, `GPIO 21 D0`)

---

## 1. Architectural Overview

The MicroSD Local Player reads lossless audio files (FLAC, WAV, MP3) from a FAT32/exFAT MicroSD card and streams them to the I2S master pipeline. 

### Latency Mitigation & Ringbuffering
Because flash memory cards undergo periodic internal garbage collection and wear-leveling stall cycles (which block read commands for **50 ms to 120 ms**), direct synchronous reading from the audio decoding task causes severe buffer starvation and audio stutter. 

To solve this, the pipeline splits work across two decoupled tasks connected by a **128 KB Octal PSRAM ringbuffer**:
* **`sd_reader_task` (Core 0, Prio 8):** Reads 4 KB multi-block clusters into the PSRAM ringbuffer.
* **Audio Decoder Task (Core 1, Prio 10):** Consumes data from the ringbuffer, decodes audio frames, and pushes PCM to I2S.

---

## 2. Function Specifications

### 2.1 `sdmmc_player_init`

Initializes the SDMMC Host peripheral, configures the 1-bit bus width, verifies pull-up presence, and mounts the FAT32 filesystem at `/sdcard`.

```c
esp_err_t sdmmc_player_init(void);
```

#### Return Values
* `ESP_OK`: Card mounted successfully.
* `ESP_ERR_TIMEOUT`: Card did not respond on `CMD` line (check 10k pull-up resistor on GPIO 13).
* `ESP_FAIL`: Mount failure or unformatted card.

#### Production Reference Implementation
```c
#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"

static sdmmc_card_t *s_card = NULL;

esp_err_t sdmmc_player_init(void)
{
    ESP_LOGI("SDMMC", "Mounting MicroSD card on Slot 1 (1-bit bus)...");

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_DEFAULT; // 20 MHz

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 1; // 1-bit bus
    slot_config.clk = (gpio_num_t)BOARD_SDMMC_CLK_PIN;
    slot_config.cmd = (gpio_num_t)BOARD_SDMMC_CMD_PIN;
    slot_config.d0  = (gpio_num_t)BOARD_SDMMC_D0_PIN;

    esp_err_t ret = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot_config,
                                            &mount_config, &s_card);
    if (ret != ESP_OK) {
        ESP_LOGE("SDMMC", "Failed to mount filesystem: %s", esp_err_to_name(ret));
        return ret;
    }

    sdmmc_card_print_info(stdout, s_card);
    return ESP_OK;
}
```

---

### 2.2 `sdmmc_player_deinit`

Flushes open file descriptors and unmounts the FAT32 filesystem.

```c
esp_err_t sdmmc_player_deinit(void);
```

#### Production Reference Implementation
```c
esp_err_t sdmmc_player_deinit(void)
{
    if (s_card == NULL) return ESP_OK;
    esp_err_t ret = esp_vfs_fat_sdcard_unmount("/sdcard", s_card);
    s_card = NULL;
    ESP_LOGI("SDMMC", "MicroSD card unmounted.");
    return ret;
}
```

---

### 2.3 `flac_decode_stream`

Streams and decodes a `.flac` audio file using `dr_flac` with zero stack allocation. Automatically configures I2S clock dividers to match the FLAC native sample rate (44.1 kHz, 48 kHz, 96 kHz, 192 kHz).

```c
esp_err_t flac_decode_stream(const char *filepath);
```

#### Parameters
| Parameter | Direction | Type | Description |
| :--- | :---: | :---: | :--- |
| `filepath` | `[in]` | `const char *` | Absolute POSIX file path (e.g. `"/sdcard/music/track01.flac"`). |

#### Production Reference Implementation
```c
#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_STDIO
#include "dr_flac.h"

esp_err_t flac_decode_stream(const char *filepath)
{
    FILE *f = fopen(filepath, "rb");
    if (!f) {
        ESP_LOGE("FLAC", "Cannot open file: %s", filepath);
        return ESP_ERR_NOT_FOUND;
    }

    drflac *pFlac = drflac_open_file(filepath, NULL);
    if (!pFlac) {
        ESP_LOGE("FLAC", "Failed to parse FLAC header");
        fclose(f);
        return ESP_FAIL;
    }

    ESP_LOGI("FLAC", "Playing: %s | %u Hz | %u Channels | %u-bit",
             filepath, pFlac->sampleRate, pFlac->channels, pFlac->bitsPerSample);

    i2s_output_set_sample_rate((int)pFlac->sampleRate);

    // Allocate 1024 stereo frame scratchpad buffer in DRAM
    int32_t *sample_buf = heap_caps_malloc(1024 * pFlac->channels * sizeof(int32_t), MALLOC_CAP_INTERNAL);
    int16_t *pcm_out    = heap_caps_malloc(1024 * 2 * sizeof(int16_t), MALLOC_CAP_INTERNAL);

    if (!sample_buf || !pcm_out) {
        if (sample_buf) free(sample_buf);
        if (pcm_out) free(pcm_out);
        drflac_close(pFlac);
        fclose(f);
        return ESP_ERR_NO_MEM;
    }

    drflac_uint64 frames_read = 0;
    while ((frames_read = drflac_read_pcm_frames_s32(pFlac, 1024, sample_buf)) > 0) {
        // Convert 24/32-bit to 16-bit stereo for PCM5102A
        for (drflac_uint64 i = 0; i < frames_read; i++) {
            int32_t sl = sample_buf[i * pFlac->channels];
            int32_t sr = (pFlac->channels == 2) ? sample_buf[i * 2 + 1] : sl;
            pcm_out[2 * i]     = (int16_t)(sl >> 16);
            pcm_out[2 * i + 1] = (int16_t)(sr >> 16);
        }
        i2s_output_write(pcm_out, frames_read * 2 * sizeof(int16_t));
    }

    free(sample_buf);
    free(pcm_out);
    drflac_close(pFlac);
    fclose(f);
    return ESP_OK;
}
```
