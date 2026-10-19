# Module 08: Dual-MCU Inter-Chip Bridge Protocol Function Specifications

> **Module Identifier:** `DUAL_MCU_BRIDGE`  
> **Primary References:** [`dual_esp32_architecture.md`](../research/dual_esp32_architecture.md)  
> **Target Hardware:** Auxiliary ESP32 Classic Co-Processor (Bluetooth Classic A2DP Sink Hub) $\longleftrightarrow$ ESP32-S3 Master DSP/DAC  
> **Interconnect Header:**
> * I2S Slave RX: `GPIO 1 (WS)`, `GPIO 2 (DIN)`, `GPIO 3 (BCK)`
> * Control UART: `GPIO 11 (RX)`, `GPIO 12 (TX)` at 921,600 baud

---

## 1. Architectural Overview

In the Dual-MCU configuration, network connectivity, Bluetooth Classic A2DP audio sink, and file storage are offloaded to an auxiliary ESP32 Classic co-processor. The ESP32-S3 acts as a pure, dedicated DSP and DAC audio processing endpoint.

Audio is transferred over dedicated inter-chip I2S lines (`GPIO 1, 2, 3`), while metadata, track titles, and volume control commands are framed over high-speed bidirectional UART (`GPIO 11, 12`).

```
    ESP32 Classic (A2DP / Wi-Fi Hub)              ESP32-S3 (DSP & DAC Master)
   ┌────────────────────────────────┐            ┌────────────────────────────┐
   │ • Bluetooth Classic A2DP Sink  │            │ • Slave I2S RX (GPIO 1,2,3)│
   │ • Master I2S Audio TX          ├───────────►│ • 10-Band PEQ SIMD DSP     │
   │ • Control UART TX (GPIO 17)    ├───────────►│ • Control UART RX (GPIO 11)│
   │ • Control UART RX (GPIO 16)    │◄───────────┤ • Control UART TX (GPIO 12)│
   └────────────────────────────────┘            └─────────────┬──────────────┘
                                                               │
                                                               ▼
                                                        PCM5102A DAC
```

---

## 2. Function Specifications

### 2.1 `dual_mcu_i2s_slave_init`

Initializes I2S1 peripheral in **Slave Receiver Mode** to accept stereo linear PCM audio clocked externally by the ESP32 Classic co-processor.

```c
esp_err_t dual_mcu_i2s_slave_init(int sample_rate);
```

#### Production Reference Implementation
```c
static i2s_chan_handle_t s_inter_rx_chan = NULL;

esp_err_t dual_mcu_i2s_slave_init(int sample_rate)
{
    ESP_LOGI("INTER_CHIP", "Initializing Slave I2S RX on WS=%d, DIN=%d, BCK=%d...",
             BOARD_INTER_I2S_WS_PIN, BOARD_INTER_I2S_DIN_PIN, BOARD_INTER_I2S_BCK_PIN);

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_SLAVE);
    chan_cfg.dma_desc_num = 8;
    chan_cfg.dma_frame_num = 256;

    esp_err_t ret = i2s_new_channel(&chan_cfg, NULL, &s_inter_rx_chan);
    if (ret != ESP_OK) return ret;

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = (gpio_num_t)BOARD_INTER_I2S_BCK_PIN,
            .ws   = (gpio_num_t)BOARD_INTER_I2S_WS_PIN,
            .dout = I2S_GPIO_UNUSED,
            .din  = (gpio_num_t)BOARD_INTER_I2S_DIN_PIN,
            .invert_flags = {0},
        },
    };

    ret = i2s_channel_init_std_mode(s_inter_rx_chan, &std_cfg);
    if (ret == ESP_OK) {
        ret = i2s_channel_enable(s_inter_rx_chan);
    }
    return ret;
}
```

---

### 2.2 `dual_mcu_uart_init`

Initializes UART1 on `GPIO 11 (RX)` and `GPIO 12 (TX)` at 921,600 baud with a 2 KB hardware ringbuffer for inter-processor control messages.

```c
esp_err_t dual_mcu_uart_init(void);
```

#### Production Reference Implementation
```c
#include "driver/uart.h"

#define INTER_UART_PORT      UART_NUM_1
#define INTER_UART_BAUD      921600

esp_err_t dual_mcu_uart_init(void)
{
    ESP_LOGI("INTER_CHIP", "Initializing Inter-MCU UART1 on RX=%d, TX=%d @ %d baud...",
             BOARD_INTER_UART_RX_PIN, BOARD_INTER_UART_TX_PIN, INTER_UART_BAUD);

    uart_config_t uart_config = {
        .baud_rate = INTER_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t ret = uart_param_config(INTER_UART_PORT, &uart_config);
    if (ret != ESP_OK) return ret;

    ret = uart_set_pin(INTER_UART_PORT, BOARD_INTER_UART_TX_PIN, BOARD_INTER_UART_RX_PIN,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (ret != ESP_OK) return ret;

    ret = uart_driver_install(INTER_UART_PORT, 2048, 2048, 10, NULL, 0);
    return ret;
}
```

---

### 2.3 `dual_mcu_send_command`

Encapsulates an outgoing command in a framed packet with start-of-frame delimiter, length, payload, CRC16 checksum, and end delimiter.

```c
esp_err_t dual_mcu_send_command(uint8_t cmd_id, const void *payload, uint16_t len);
```

#### Packet Frame Format
```
 [ 0xAA 0x55 ] [ Cmd: 1B ] [ Len: 2B ] [ Payload: N Bytes ] [ CRC16: 2B ] [ 0x0D 0x0A ]
```

#### Production Reference Implementation
```c
static uint16_t calculate_crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 0x0001) crc = (crc >> 1) ^ 0xA001;
            else crc = crc >> 1;
        }
    }
    return crc;
}

esp_err_t dual_mcu_send_command(uint8_t cmd_id, const void *payload, uint16_t len)
{
    uint8_t packet[256];
    if (len > 240) return ESP_ERR_INVALID_SIZE;

    packet[0] = 0xAA; // Sync byte 1
    packet[1] = 0x55; // Sync byte 2
    packet[2] = cmd_id;
    packet[3] = (uint8_t)(len & 0xFF);
    packet[4] = (uint8_t)((len >> 8) & 0xFF);

    if (payload && len > 0) {
        memcpy(&packet[5], payload, len);
    }

    uint16_t crc = calculate_crc16(&packet[2], len + 3);
    packet[5 + len] = (uint8_t)(crc & 0xFF);
    packet[6 + len] = (uint8_t)((crc >> 8) & 0xFF);
    packet[7 + len] = 0x0D; // End delimiter 1
    packet[8 + len] = 0x0A; // End delimiter 2

    int sent = uart_write_bytes(INTER_UART_PORT, (const char *)packet, len + 9);
    return (sent == len + 9) ? ESP_OK : ESP_FAIL;
}
```
