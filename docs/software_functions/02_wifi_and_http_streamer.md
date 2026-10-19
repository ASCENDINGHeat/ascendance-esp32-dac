# Module 02: Wi-Fi & HTTP Audio Streamer Function Specifications

> **Module Identifier:** `WIFI_HTTP_STREAMER`  
> **Source Files:** [`main/main.c`](../../main/main.c), [`main/minimp3.h`](../../main/minimp3.h)  
> **ESP-IDF Components:** `esp_wifi`, `esp_event`, `esp_netif`, `esp_http_client`, `nvs_flash`  
> **Decoder Library:** `minimp3` (zero-stack allocation, MPEG-1/2/2.5 Layer III)

---

## 1. Architectural Overview

The Wi-Fi & HTTP streaming subsystem establishes a high-performance 802.11b/g/n station link to the local 2.4 GHz Access Point, connects to an external Icecast/Shoutcast/HTTP MP3 audio stream, pulls incoming chunks into a circular jitter buffer, parses frame sync headers, decodes variable-bitrate MP3 frames using `minimp3`, and feeds linear stereo PCM to the I2S DMA pipeline.

```
       [ HTTP Audio Stream Socket ]
                   │
                   ▼  esp_http_client_read() [4 KB Chunks]
     ┌────────────────────────────┐
     │ MP3 Stream Ringbuffer (32K)│
     └─────────────┬──────────────┘
                   ▼  mp3dec_decode_frame()
     ┌────────────────────────────┐
     │ minimp3 Frame Decoder      │ ──► Dynamic Sample Rate Sync:
     │ (Zero-Stack Scratchpad)    │     i2s_output_set_sample_rate(info.hz)
     └─────────────┬──────────────┘
                   ▼
     ┌────────────────────────────┐
     │ Linear 16-bit Stereo PCM   │ ──► i2s_output_write()
     └────────────────────────────┘
```

---

## 2. Function Specifications

### 2.1 `wifi_reason_to_str`

Translates raw 802.11 Wi-Fi disconnect reason codes (`wifi_err_reason_t`) into human-readable diagnostic messages with actionable troubleshooting hints.

```c
const char *wifi_reason_to_str(uint8_t reason);
```

#### Parameters
| Parameter | Direction | Type | Valid Range | Description |
| :--- | :---: | :---: | :--- | :--- |
| `reason` | `[in]` | `uint8_t` | `0` to `255` (`wifi_err_reason_t`) | Numeric disconnect reason passed in `wifi_event_sta_disconnected_t`. |

#### Return Value
* `const char *`: Statically allocated string literal describing the disconnection cause.

#### Key Translated Codes
* `WIFI_REASON_NO_AP_FOUND (201)`: "No AP found (check SSID / verify 2.4 GHz band)"
* `WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT (15)`: "4-way handshake timeout (check password / weak signal)"
* `WIFI_REASON_AUTH_FAIL (202)`: "Authentication failed (wrong password / WPA3 transition mode mismatch)"
* `WIFI_REASON_BEACON_TIMEOUT (200)`: "Beacon timeout (AP disappeared or modem sleep triggered)"

#### Production Reference Implementation
```c
static const char *wifi_reason_to_str(uint8_t reason)
{
    switch (reason) {
    case WIFI_REASON_UNSPECIFIED: return "Unspecified error";
    case WIFI_REASON_AUTH_EXPIRE: return "Authentication expired";
    case WIFI_REASON_AUTH_LEAVE: return "Deauthentication due to leaving";
    case WIFI_REASON_ASSOC_TOOMANY: return "Too many associated stations";
    case WIFI_REASON_ASSOC_NOT_AUTHED: return "Not authenticated";
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT: return "4-way handshake timeout (check password / weak signal)";
    case WIFI_REASON_BEACON_TIMEOUT: return "Beacon timeout (AP disappeared or weak signal)";
    case WIFI_REASON_NO_AP_FOUND: return "No AP found (check SSID / verify 2.4 GHz band)";
    case WIFI_REASON_AUTH_FAIL: return "Authentication failed (wrong password)";
    case WIFI_REASON_ASSOC_FAIL: return "Association failed";
    case WIFI_REASON_HANDSHAKE_TIMEOUT: return "Handshake timeout";
    case WIFI_REASON_CONNECTION_FAIL: return "Connection failed";
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY: return "No AP found with compatible security";
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD: return "Authmode threshold mismatch";
    default: return "Other disconnect reason";
    }
}
```

---

### 2.2 `wifi_event_handler`

Central event callback for the ESP-IDF default event loop. Manages station lifecycle, logs disconnections, initiates automatic reconnect with backoff, and sets/clears synchronization bits in `s_wifi_event_group`.

```c
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data);
```

#### Parameters
| Parameter | Direction | Type | Description |
| :--- | :---: | :---: | :--- |
| `arg` | `[in]` | `void *` | User registration pointer (unused, `NULL`). |
| `event_base` | `[in]` | `esp_event_base_t` | `WIFI_EVENT` or `IP_EVENT`. |
| `event_id` | `[in]` | `int32_t` | Specific event identifier (`WIFI_EVENT_STA_START`, `WIFI_EVENT_STA_DISCONNECTED`, `IP_EVENT_STA_GOT_IP`). |
| `event_data` | `[in]` | `void *` | Event payload structure pointer. |

#### Event Transitions
```
   WIFI_EVENT_STA_START ────► esp_wifi_connect()
                                     │
                 ┌───────────────────┴───────────────────┐
                 ▼                                       ▼
       IP_EVENT_STA_GOT_IP                 WIFI_EVENT_STA_DISCONNECTED
  (Set WIFI_CONNECTED_BIT)                   (Clear WIFI_CONNECTED_BIT)
                                                         │
                                                         ▼
                                            vTaskDelay(1000ms) ──► esp_wifi_connect()
```

#### Production Reference Implementation
```c
static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI("WIFI", "Wi-Fi station hardware ready. Initiating connection...");
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *disconn = (wifi_event_sta_disconnected_t *)event_data;
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);

        ESP_LOGW("WIFI", "Wi-Fi disconnected: reason %d (%s)",
                 disconn->reason, wifi_reason_to_str(disconn->reason));

        if (disconn->reason == WIFI_REASON_NO_AP_FOUND) {
            ESP_LOGW("WIFI", "Hint: Verify router broadcasts on 2.4 GHz (5 GHz unsupported)");
        } else if (disconn->reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
                   disconn->reason == WIFI_REASON_AUTH_FAIL) {
            ESP_LOGW("WIFI", "Hint: Check WPA2/WPA3 password credentials");
        }

        // Reconnect with a 1-second backoff delay
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI("WIFI", "Wi-Fi Connected! IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}
```

---

### 2.3 `wifi_scan_and_log`

Performs an active Wi-Fi scan on all 2.4 GHz channels (1–13) matching the configured SSID. Logs the BSSID, operating channel, RSSI (signal strength), and detected security authentication suite.

```c
static void wifi_scan_and_log(void);
```

#### Engineering Purpose
Identifies multi-AP environments (mesh routers, dual-band 2.4/5GHz unified SSIDs), verifies that 2.4 GHz broadcast is active, and detects WPA3-SAE vs WPA2-PSK security mode discrepancies.

#### Production Reference Implementation
```c
static void wifi_scan_and_log(void)
{
    ESP_LOGI("WIFI", "Scanning for APs matching SSID \"%s\"...", WIFI_SSID);
    wifi_scan_config_t scan_cfg = {
        .ssid = (uint8_t *)WIFI_SSID,
        .bssid = NULL,
        .channel = 0, // Scan all channels
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };
    esp_wifi_scan_start(&scan_cfg, true); // Blocking scan

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    ESP_LOGI("WIFI", "Found %d matching AP(s):", ap_count);

    if (ap_count > 0) {
        wifi_ap_record_t *ap_records = malloc(ap_count * sizeof(wifi_ap_record_t));
        if (ap_records) {
            esp_wifi_scan_get_ap_records(&ap_count, ap_records);
            for (int i = 0; i < ap_count; i++) {
                const char *auth_str = "Unknown";
                switch (ap_records[i].authmode) {
                case WIFI_AUTH_OPEN: auth_str = "OPEN"; break;
                case WIFI_AUTH_WEP: auth_str = "WEP"; break;
                case WIFI_AUTH_WPA_PSK: auth_str = "WPA_PSK"; break;
                case WIFI_AUTH_WPA2_PSK: auth_str = "WPA2_PSK"; break;
                case WIFI_AUTH_WPA_WPA2_PSK: auth_str = "WPA_WPA2_PSK"; break;
                case WIFI_AUTH_WPA2_ENTERPRISE: auth_str = "WPA2_ENTERPRISE"; break;
                case WIFI_AUTH_WPA3_PSK: auth_str = "WPA3_SAE"; break;
                case WIFI_AUTH_WPA2_WPA3_PSK: auth_str = "WPA2_WPA3_TRANSITION"; break;
                default: break;
                }
                ESP_LOGI("WIFI", "  -> BSSID %02x:%02x:%02x:%02x:%02x:%02x | Ch: %2d | RSSI: %3d dBm | Security: %s",
                         ap_records[i].bssid[0], ap_records[i].bssid[1], ap_records[i].bssid[2],
                         ap_records[i].bssid[3], ap_records[i].bssid[4], ap_records[i].bssid[5],
                         ap_records[i].primary, ap_records[i].rssi, auth_str);
            }
            free(ap_records);
        }
    } else {
        ESP_LOGW("WIFI", "No AP found for SSID \"%s\"! Check 2.4 GHz router broadcast.", WIFI_SSID);
    }
}
```

---

### 2.4 `wifi_init_sta`

Initializes the TCP/IP stack (`esp_netif`), creates the default Wi-Fi station interface, registers event handlers, configures WPA2/WPA3 PMF auto-negotiation, disables power-saving modem sleep, and initiates station connection.

```c
static void wifi_init_sta(void);
```

#### Critical Invariants
1. **`WIFI_STORAGE_RAM`:** Credentials are not written to flash on every boot, eliminating flash wear.
2. **`WIFI_PS_NONE`:** Disables Wi-Fi modem sleep. Default power-save sleep causes periodic 200 ms RF sleep states that drop incoming TCP audio packets and cause buffer underrun clicks.
3. **PMF Auto-Negotiation:** Sets `pmf_cfg.capable = true` and `pmf_cfg.required = false`. Enables seamless roaming on both legacy WPA2 networks and modern WPA3-SAE / Wi-Fi 6 routers without Reason 202 auth panics.

#### Production Reference Implementation
```c
static void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    // Support channels 1 through 13 (European / Asian regulatory compliance)
    wifi_country_t country = {
        .cc = "01",
        .schan = 1,
        .nchan = 13,
        .policy = WIFI_COUNTRY_POLICY_AUTO,
    };
    esp_wifi_set_country(&country);

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler, NULL, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    // CRITICAL: Disable modem sleep to prevent audio stutter and beacon drops
    esp_wifi_set_ps(WIFI_PS_NONE);

    // Active diagnostic scan
    wifi_scan_and_log();

    wifi_config_t wifi_config = {
        .sta = {
            .scan_method = WIFI_ALL_CHANNEL_SCAN,
            .sort_method = WIFI_CONNECT_AP_BY_SIGNAL,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
            .pmf_cfg = {
                .capable = true,
                .required = false,
            },
        },
    };
    strncpy((char *)wifi_config.sta.ssid, WIFI_SSID, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, WIFI_PASS, sizeof(wifi_config.sta.password) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_LOGI("WIFI", "Connecting to \"%s\"...", WIFI_SSID);
    esp_wifi_connect();
}
```

---

### 2.5 `audio_stream_task`

Long-running FreeRTOS worker task. Manages the HTTP client lifecycle, streams MP3 chunks over TCP sockets into a 32 KB sliding window ringbuffer, parses frame boundaries, decodes samples via `minimp3`, dynamically adapts I2S clocks, and writes linear PCM to the DAC.

```c
static void audio_stream_task(void *pvParameters);
```

#### Parameters
| Parameter | Direction | Type | Description |
| :--- | :---: | :---: | :--- |
| `pvParameters` | `[in]` | `void *` | Cast to `const char *stream_url`. The HTTP/HTTPS stream URL. |

#### Agentic AI Execution Context
* **Core Affinity:** **Core 1** (Real-Time Audio Worker).
* **Priority:** 5 (Above UI and IDLE; below I2S DMA interrupts).
* **Stack Budget:** 32,768 bytes (32 KB).
* **Dynamic Allocations:**
  * `stream_buf`: 32,768 bytes (sliding window MP3 buffer).
  * `pcm_out`: $2304 \times 2 = 4608$ bytes (decoded mono/stereo PCM).
  * `pcm_stereo`: 4608 bytes (mono expansion buffer).
  * `mp3d`: sizeof(`mp3dec_t`) $\approx 16$ KB (decoder internal scratchpad).

#### Frame Decoding & Resync Algorithm
```
 ┌────────────────────────────────────────────────────────────────┐
 │ [HTTP Socket Read: 4096 bytes] ──► append to stream_buf        │
 └───────────────────────────────┬────────────────────────────────┘
                                 ▼
           ┌────────────────────────────────────────────┐
           │ mp3dec_decode_frame(stream_buf + offset)   │
           └─────────────────────┬──────────────────────┘
                                 │
           ┌─────────────────────┴──────────────────────┐
           ▼                                            ▼
 [info.frame_bytes > 0]                       [info.frame_bytes == 0]
 • Advance offset by frame_bytes              • Incomplete frame: Need more data
 • Dynamic sample rate check:                 • memmove() unconsumed bytes to head
   if (info.hz != s_current_rate)             • Continue HTTP socket read loop
       i2s_output_set_sample_rate(info.hz)
 • Expand mono -> stereo if needed
 • i2s_output_write(pcm_data)
```

#### Production Reference Implementation
*(Refer to [`main/main.c#L342-L509`](../../main/main.c#L342-L509) for the verified, full implementation).*
