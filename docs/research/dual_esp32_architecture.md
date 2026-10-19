# Dual-MCU Distributed Architecture: Connectivity Hub & Dedicated DSP/DAC

This technical guide specifies the architectural upgrade roadmap for splitting the system into a **Dual-ESP32 Distributed Audio System**:
1. **ESP32 Classic (Connectivity & Media Hub):** Handles Bluetooth Classic **A2DP Sink**, Wi-Fi streaming, MicroSD local playback, and User Interface (OLED/Rotary Encoder).
2. **ESP32-S3 (Dedicated DSP & DAC Engine):** Dedicated exclusively to Native USB UAC DAC input, Xtensa PIE SIMD **Parametric Equalization (PEQ)**, and ultra-low jitter I2S output to the **PCM5102A** DAC and **TDA1308** amplifier.

> **Note for Agentic AI / AI Assistants:**  
> This specification details the inter-chip physical bus (I2S audio + UART control), protocol framing schemas, FreeRTOS synchronization, RF isolation benefits, and an actionable implementation checklist for dual-chip firmware development.

---

## 1. Motivation: Solving Silicon Constraints & RF Interference

### 1.1 The Bluetooth Classic (A2DP) Bottleneck
The ESP32-S3 SoC features Wi-Fi 4 and Bluetooth Low Energy (BLE 5.0), but **physically lacks Bluetooth Classic (BR/EDR) baseband silicon**. Consumer smartphones, tablets, and computers transmit streaming media using Bluetooth Classic **A2DP** (Advanced Audio Distribution Profile). 
* By pairing the **ESP32-S3** with an inexpensive **ESP32 Classic** (e.g. ESP32-WROOM-32 / ESP32-D0WDQ6-V3), full Bluetooth A2DP audio sink functionality becomes natively supported via ESP-IDF's `esp_a2dp_api.h`.

### 1.2 Complete RF Noise Isolation
Wi-Fi and Bluetooth transmissions draw sharp 200–350 mA current pulses at 2.4 GHz. In a single-chip topology, these RF bursts can capacitively couple into high-impedance analog audio stages (producing "Wi-Fi chirp" or RF hum).
* In the **Dual-MCU architecture**, the entire radio subsystem (Wi-Fi, Bluetooth, antennas) is physically segregated to the **Connectivity Hub (ESP32 Classic)**.
* The **DSP/DAC Engine (ESP32-S3)** runs with its radio transmitter completely powered down, guaranteeing an ultra-quiet analog ground plane for the PCM5102A DAC and TDA1308 amplifier.

---

## 2. Distributed System Topology

```
   ┌─────────────────────────────────────────────────────────────┐
   │            ESP32 Classic (Connectivity & Media Hub)         │
   │                                                             │
   │  [Wireless Inputs]           [Local Storage & UI]           │
   │  • Bluetooth A2DP Sink       • MicroSD Card (SDMMC / SPI)   │
   │    (SBC / AAC Decoders)      • I2C OLED Screen (SSD1306)    │
   │  • Wi-Fi HTTP / Icecast      • Rotary Encoder & Buttons     │
   │                                                             │
   │  [Core 0: Network & UI]      [Core 1: Media Decoders]       │
   └──────────────┬───────────────────────────────┬──────────────┘
                  │ Inter-Chip I2S                │ Inter-Chip UART
                  │ Digital PCM Stream            │ Control & Metadata
                  │ (GPIO 25, 26, 22)             │ (GPIO 16, 17 @ 921600 baud)
                  ▼                               ▼
   ┌─────────────────────────────────────────────────────────────┐
   │          YD-ESP32-S3 (Dedicated DSP & DAC Engine)           │
   │                                                             │
   │  • Native USB OTG Input ──► USB Audio Class 1.0 (48kHz/16b) │
   │  • Inter-Chip I2S Input ──► Receives BT / Wi-Fi / SD PCM    │
   │  • Audio Source Mux     ──► Smooth Zero-Pop Switching       │
   │  • Hardware SIMD PEQ    ──► Xtensa PIE dsps_biquad_f32_ae32 │
   │  • Dynamic Master Clock ──► Dynamic Reclocking Engine       │
   │                                                             │
   │  [Output I2S Master: GPIO 4 (WS), GPIO 5 (DIN), GPIO 6 (BCK)]│
   └──────────────────────────────┬──────────────────────────────┘
                                  │ DirectPath Line Out
                                  ▼
                   ┌─────────────────────────────┐
                   │  TI GY-PCM5102A 32-bit DAC  │
                   └──────────────┬──────────────┘
                                  │ Analog L / R
                                  ▼
                   ┌─────────────────────────────┐
                   │  TDA1308 Headphone Pre-Amp  │
                   └──────────────┬──────────────┘
                                  │ 3.5mm Stereo
                                  ▼
                        [Headphones / Speakers]
```

---

## 3. Subsystem Workload Distribution

| Subsystem / Function | Assigned Processor | Rationale |
| :--- | :--- | :--- |
| **Bluetooth Classic A2DP** | **ESP32 Classic** | Hardware BR/EDR radio support with official ESP-IDF A2DP Sink API. |
| **Wi-Fi HTTP Radio / DLNA**| **ESP32 Classic** | Offloads network handshakes and socket buffering from the DSP engine. |
| **MicroSD FATFS Player** | **ESP32 Classic** | Handles file scanning and cluster boundary latency buffering. |
| **OLED Display & UI Menu** | **ESP32 Classic** | Isolates display refresh I2C traffic from real-time audio interrupts. |
| **Rotary Encoder & Buttons**| **ESP32 Classic** | Immediate user input handling and menu navigation. |
| **Native USB Audio Class** | **ESP32-S3** | Direct hardware USB OTG PHY (`GPIO 19/20`) on ESP32-S3. |
| **Parametric EQ (PEQ DSP)**| **ESP32-S3** | Utilizes ESP32-S3 **Xtensa PIE SIMD vector instructions** for biquad filtering. |
| **I2S DAC Master Driver** | **ESP32-S3** | Ultra-clean clock generation to PCM5102A with zero RF jitter. |
| **DAC Mute Hardware Pin** | **ESP32-S3** | Direct control of `GPIO 7 (XMT)` for hardware pop prevention. |

---

## 4. Inter-Chip Physical Interconnect & Pin Mapping

### 4.1 Bus Architecture
The two microcontrollers communicate across two dedicated buses:
1. **Digital Audio Bus (I2S):** Carries uncompressed interleaved 16-bit stereo PCM audio.
2. **Control & Telemetry Bus (UART):** High-speed asynchronous serial link (921,600 baud) for track metadata, volume commands, and sample rate negotiation.

```
       ESP32 Classic (Media Hub)             YD-ESP32-S3 (DSP Engine)
     ┌───────────────────────────┐         ┌───────────────────────────┐
     │                           │         │                           │
     │  [I2S Master Output]      │         │  [I2S Slave / RX Input]   │
     │  GPIO 25 (I2S_WS)        ├────────►│  GPIO 1 (I2S_RX_WS)       │
     │  GPIO 26 (I2S_DO)        ├────────►│  GPIO 2 (I2S_RX_DIN)      │
     │  GPIO 22 (I2S_BCK)       ├────────►│  GPIO 3 (I2S_RX_BCK)      │
     │                           │         │                           │
     │  [Control UART]           │         │  [Control UART]           │
     │  GPIO 17 (TXD2)          ├────────►│  GPIO 11 (RXD1)           │
     │  GPIO 16 (RXD2)          │◄────────┤  GPIO 12 (TXD1)           │
     │                           │         │                           │
     │  [Power & Ground]         │         │  [Power & Ground]         │
     │  GND                     ├─────────┼─► GND (Common Ground)     │
     │  5V                      ├─────────┼─► 5V (From Clean Supply)  │
     └───────────────────────────┘         └───────────────────────────┘
```

### 4.2 Pin Mapping Table

| Bus | Signal Name | ESP32 Classic Pin | ESP32-S3 Pin | Signal Description |
| :--- | :--- | :--- | :--- | :--- |
| **I2S Audio** | `INTER_I2S_WS` | **GPIO 25** (DOUT) | **GPIO 1** (DIN) | Inter-chip Word Select / Frame Sync |
| **I2S Audio** | `INTER_I2S_DATA` | **GPIO 26** (DOUT) | **GPIO 2** (DIN) | Inter-chip Serial PCM Audio Stream |
| **I2S Audio** | `INTER_I2S_BCK` | **GPIO 22** (DOUT) | **GPIO 3** (DIN) | Inter-chip Bit Clock |
| **Control** | `UART_HUB_TX` | **GPIO 17** (TXD2) | **GPIO 11** (RXD1)| Commands & metadata from Hub to S3 |
| **Control** | `UART_HUB_RX` | **GPIO 16** (RXD2) | **GPIO 12** (TXD1)| Telemetry & status from S3 to Hub |
| **Ground** | `GND` | **GND** | **GND** | Common Digital Ground Reference |

---

## 5. Bluetooth Classic A2DP Implementation on ESP32 Classic

### 5.1 Architecture & Component Selection
The ESP32 Classic runs the native ESP-IDF Bluetooth stack (`esp_bt.h` and `esp_a2dp_api.h`):

```c
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"

void bt_app_init(void)
{
    // Initialize controller and Bluedroid stack
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_bt_controller_init(&bt_cfg);
    esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT);
    esp_bluedroid_init();
    esp_bluedroid_enable();

    // Set device name visible on smartphones/PCs
    esp_bt_dev_set_device_name("Hi-Fi Audiophile Receiver");

    // Initialize A2DP Audio Sink
    esp_a2d_register_callback(bt_a2d_cb);
    esp_a2d_sink_register_data_callback(bt_audio_data_cb);
    esp_a2d_sink_init();

    // Initialize AVRCP Target (Play/Pause/Skip control)
    esp_avrc_ct_init();
    esp_avrc_ct_register_callback(bt_avrc_ct_cb);

    // Make device discoverable and connectable
    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
}
```

### 5.2 Audio Data Callback Pipeline
When Bluetooth audio packets arrive:
```c
void bt_audio_data_cb(const uint8_t *data, uint32_t len)
{
    // data contains interleaved 16-bit stereo PCM from SBC/AAC decoder
    // Transmit directly over Inter-Chip I2S bus to ESP32-S3
    size_t bytes_written = 0;
    i2s_channel_write(inter_i2s_tx_chan, data, len, &bytes_written, portMAX_DELAY);
}
```

---

## 6. Inter-Chip Control & Telemetry Protocol

Communication across the 921,600 baud UART link uses a compact binary framing protocol with CRC-16 integrity checking:

### 6.1 Packet Structure
```
┌────────┬────────┬────────┬────────┬──────────────────────────┬────────┬────────┐
│  SOF1  │  SOF2  │  CMD   │ LENGTH │         PAYLOAD          │ CRC_H  │ CRC_L  │
│  0xAA  │  0x55  │ 1 Byte │ 1 Byte │        0–64 Bytes        │ 1 Byte │ 1 Byte │
└────────┴────────┴────────┴────────┴──────────────────────────┴────────┴────────┘
```

### 6.2 Command Set
| Command ID | Name | Direction | Payload Description |
| :--- | :--- | :--- | :--- |
| `0x01` | `CMD_SET_SOURCE` | Hub $\rightarrow$ S3 | `0x00`: USB DAC, `0x01`: Inter-Chip I2S |
| `0x02` | `CMD_SET_SAMPLE_RATE` | Hub $\rightarrow$ S3 | 4-byte uint32 (e.g. `44100`, `48000`, `96000`) |
| `0x03` | `CMD_SET_VOLUME` | Hub $\rightarrow$ S3 | 1-byte uint8 (0–100) |
| `0x04` | `CMD_SET_PEQ_PROFILE` | Hub $\rightarrow$ S3 | 1-byte preset index (0: Flat, 1: Harman, 2: Bass) |
| `0x80` | `RESP_S3_STATUS` | S3 $\rightarrow$ Hub | Sample rate, peak VU level (L/R), active buffer level |
| `0x81` | `RESP_S3_ACK` | S3 $\rightarrow$ Hub | Acknowledges received command |

---

## 7. Reference for AI Agents: Implementation Checklist & Invariants

When implementing or extending this distributed dual-MCU architecture, autonomous agents must adhere to the following rules:

### 7.1 Architecture Invariants
1. **Never Enable Wi-Fi or Bluetooth on the ESP32-S3:**
   The ESP32-S3's entire radio stack must remain compiled out or stopped (`esp_wifi_stop()`). All RF tasks are strictly confined to the ESP32 Classic co-processor to guarantee zero analog interference.
2. **Master/Slave I2S Clock Discipline:**
   * **Option A (Recommended):** ESP32-S3 operates as **I2S Master** for the DAC output, and **I2S Slave** for the inter-chip input.
   * To prevent clock drift between the two microcontrollers, the ESP32-S3 can route its internal Master Clock to the ESP32 Classic, or use an asynchronous ringbuffer with sample drop/stuff watermark feedback.
3. **Strict Star-Grounding:**
   Connect the digital grounds of both boards with a low-impedance connection near the inter-chip header, but ensure analog audio ground (`AGND` on PCM5102A) returns exclusively to the ESP32-S3 ground plane.

### 7.2 Step-by-Step Implementation Checklist

- [ ] **Step 1: Flash ESP32 Classic with Media Hub Firmware**
  * Configure Bluetooth Classic A2DP Sink with `CONFIG_BT_ENABLED=y` and `CONFIG_BT_CLASSIC_ENABLED=y`.
  * Configure I2S Master TX on GPIO 25, 26, 22.
  * Connect I2C OLED display (GPIO 21: SDA, GPIO 22: SCL, or remapped).
- [ ] **Step 2: Update ESP32-S3 Firmware for Dual-Input Operation**
  * Configure I2S Channel 0 as Master TX to PCM5102A (`GPIO 4, 5, 6`).
  * Configure I2S Channel 1 as Slave RX from ESP32 Classic (`GPIO 1, 2, 3`).
  * Configure UART1 on `GPIO 11 (RX)` and `GPIO 12 (TX)` for command decoding.
- [ ] **Step 3: Implement Zero-Pop Source Arbitration on ESP32-S3**
  * Support instant switching between **Native USB DAC Mode** and **Inter-Chip I2S Input Mode** using the 4-phase muting protocol.
- [ ] **Step 4: Verify Bit-Perfect Audio Pipeline**
  * Play 1 kHz 0 dBFS sine wave over Bluetooth A2DP from a smartphone; inspect output on oscilloscope or audio interface to confirm clean, undistorted waveform and zero buffer underruns.
