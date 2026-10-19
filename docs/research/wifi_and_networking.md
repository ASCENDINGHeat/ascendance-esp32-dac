# Wi-Fi & Network Architecture

> **Note for Agentic AI / AI Assistants:**  
> This document specifies radio configurations, power-save policies, and network stability safeguards. When modifying network initialization or HTTP streaming routines, strictly follow the rules in Section 5.

## 1. Wi-Fi Station Configuration

The networking stack runs on FreeRTOS using ESP-IDF's native `esp_wifi` and `esp_netif` components.

### 1.1 Radio Configuration
```c
wifi_config_t wifi_config;
memset(&wifi_config, 0, sizeof(wifi_config));
strncpy((char *)wifi_config.sta.ssid, WIFI_SSID, sizeof(wifi_config.sta.ssid) - 1);
strncpy((char *)wifi_config.sta.password, WIFI_PASS, sizeof(wifi_config.sta.password) - 1);

wifi_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
wifi_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
wifi_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
wifi_config.sta.pmf_cfg.capable = true;
wifi_config.sta.pmf_cfg.required = false;
```

---

## 2. Key Networking Optimizations

### 2.1 Disabling Modem Sleep (`WIFI_PS_NONE`)
By default, ESP-IDF enables Wi-Fi modem sleep (`WIFI_PS_MIN_MODEM`) to save power. During modem sleep, the ESP32 powers down its radio receiver between DTIM beacon intervals.
* **The Problem:** Modern dual-band routers, mesh systems, and smartphone hotspots frequently drop sleeping IoT devices due to beacon timeout disconnects.
* **The Solution:**
  ```c
  esp_wifi_set_ps(WIFI_PS_NONE);
  ```
  Keeping the receiver permanently energized eliminates disconnect loops and reduces HTTP socket latency to zero.

### 2.2 Compatibility with Wi-Fi 6 (802.11ax) Hotspots
The ESP32-S3 hardware supports **Wi-Fi 4 (802.11b/g/n) only**.
When connecting to a modern smartphone mobile hotspot:
* **Protocol Limiting:** The firmware explicitly instructs the radio layer to negotiate 802.11b/g/n:
  ```c
  ESP_ERROR_CHECK(esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N));
  ```
* **Hotspot Setting Advice:** If using an Android or iPhone hotspot that supports Wi-Fi 6 (802.11ax), ensure **"Wi-Fi 6 standard"** is turned **OFF** (Android) or **"Maximize Compatibility"** is turned **ON** (iOS) so the hotspot broadcasts compatible 802.11n beacon rates.

### 2.3 WPA2 & WPA3 Auto-Negotiation with PMF
* **WPA3-Personal (SAE):** Enables Simultaneous Authentication of Equals (`WPA3_SAE_PWE_BOTH`).
* **Protected Management Frames (PMF):** Required for all WPA3 and Wi-Fi 6 connections. Setting `.pmf_cfg.capable = true` ensures the ESP32 can negotiate PMF without refusing connections to standard WPA2 networks.

---

## 3. Active Access Point Scanning & Diagnostics

On bootup, before attempting association, the firmware runs an active scan (`wifi_scan_and_log`):
```c
wifi_scan_config_t scan_cfg = {
    .ssid = (uint8_t *)WIFI_SSID,
    .scan_type = WIFI_SCAN_TYPE_ACTIVE,
};
esp_wifi_scan_start(&scan_cfg, true);
```
This logs critical diagnostic parameters to the serial console:
* **BSSID:** Identifies whether the AP is a router or a mobile hotspot.
* **Channel:** Verifies the 2.4 GHz channel (channels 1–13 supported via `WIFI_COUNTRY_POLICY_AUTO`).
* **RSSI (Signal Strength):** Diagnoses whether low signal ($\le -80\text{ dBm}$) is causing packet loss.
* **Security Mode:** Detects whether the AP expects `WPA2_PSK`, `WPA3_SAE`, or `WPA2_WPA3_TRANSITION`.

---

## 4. HTTP Audio Streaming Client

### 4.1 Client Configuration
```c
esp_http_client_config_t config = {
    .url = stream_url,
    .timeout_ms = 10000,
    .buffer_size = 4096,
    .buffer_size_tx = 1024,
    .disable_auto_redirect = false, // Automatically follows 301/302 redirects
};
```

### 4.2 Two-Stage Handshake
To stream audio reliably, `esp_http_client` must perform header fetching before payload reads:
1. `esp_http_client_open(client, 0);` — Opens the TCP socket and transmits the HTTP GET request.
2. `esp_http_client_fetch_headers(client);` — Parses the server's HTTP status line (e.g. `200 OK`) and content headers.
3. `esp_http_client_read()` — Streams continuous MP3 audio chunks.

### 4.3 Loss of Connection Recovery
If the Wi-Fi link drops or the server terminates the stream:
* `audio_stream_task` checks `xEventGroupGetBits(s_wifi_event_group)`.
* It cleanly closes the HTTP socket, enters a 1.5-second backoff delay, and automatically reconnects once `WIFI_CONNECTED_BIT` is asserted.

---

## 5. Reference for AI Agents & Autonomous Coders

When generating or modifying networking logic in this codebase, AI agents must enforce these rules:

### 5.1 Mandatory Network Calls
1. **Disable Modem Sleep (`WIFI_PS_NONE`):**
   ```c
   ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
   ```
   Never omit this call. If omitted, ESP-IDF defaults to `WIFI_PS_MIN_MODEM`, causing beacon dropouts, packet jitter, and audio buffer underruns.
2. **Restrict Protocol to 802.11b/g/n:**
   ```c
   ESP_ERROR_CHECK(esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N));
   ```
   Do not remove this restriction; it prevents the radio from attempting unsupported 802.11ax operations on Wi-Fi 6 routers.
3. **Protected Management Frames (PMF):**
   ```c
   wifi_config.sta.pmf_cfg.capable = true;
   wifi_config.sta.pmf_cfg.required = false;
   ```
   Required for seamless negotiation across mixed WPA2/WPA3 routers.

### 5.2 Wi-Fi Radio Power Domain Management
* When switching to **USB DAC Mode** or **MicroSD Player Mode**, call `esp_wifi_stop()` to power down the RF synthesizer. This eliminates high-frequency RF buzzing in the analog audio path and reduces MCU current draw by ~100–140 mA.
* When returning to **Wi-Fi Streaming Mode**, call `esp_wifi_start()`.

