# Troubleshooting & Failure Mode Analysis

> **Note for Agentic AI / AI Assistants:**  
> This document details diagnostic triage procedures, error signatures, and root-cause solutions. When automated diagnostic agents encounter runtime exceptions, use the Automated Diagnostic Decision Tree in Section 6.

This document compiles the issues, root causes, and engineering solutions discovered during the development and testing of this audio player.

---

## 1. Issue: `Reason 202 (Authentication failed / wrong password)` on Mobile Hotspots

### Symptom
```text
I (6951) wifi:state: init -> auth (0xb0)
I (6961) wifi:state: auth -> init (0x1c0)
W (6961) AUDIO_PLAYER: Wi-Fi disconnected: reason 202 (Authentication failed (wrong password))
```
The device disconnects in **10 milliseconds**, even when the password is verified 100% correct.

### Root Cause
1. **Password Was Not Even Checked:** In WPA2/WPA3 Wi-Fi, the password is only verified in the 4-way EAPOL handshake *after* 802.11 authentication and association (`auth -> assoc -> run`). Because the transition `auth -> init` occurs in 10 ms, the access point rejected the station at the radio link layer.
2. **Wi-Fi 6 (802.11ax) Hotspot Incompatibility:** Modern smartphone hotspots (broadcasting at rates $\ge 1170\text{ Mbit/s}$) enforce 802.11ax frame features. The ESP32-S3 is an **802.11b/g/n (Wi-Fi 4)** device; when it attempts to associate, the Wi-Fi 6 hotspot immediately drops the Open System auth frame.
3. **MAC Filtering / "Allowed Devices Only":** If device filtering is active on the phone, the phone drops unknown MACs.

### Solution
1. **On Android:** Go to *Settings $\rightarrow$ Connections $\rightarrow$ Mobile Hotspot $\rightarrow$ Configure $\rightarrow$ Advanced*.
   * Turn **OFF** **"Wi-Fi 6 standard"**.
   * Turn **OFF** **"Allowed devices only"**.
   * Set Security to **WPA2-Personal** or **WPA2/WPA3-Personal**.
2. **On iPhone:** Go to *Settings $\rightarrow$ Personal Hotspot* and turn **ON** **"Maximize Compatibility"** (enables 2.4 GHz 802.11n mode).
3. **In Firmware:** Set `.threshold.authmode = WIFI_AUTH_WPA2_PSK`, `.sae_pwe_h2e = WPA3_SAE_PWE_BOTH`, and `.pmf_cfg.capable = true`.

---

## 2. Issue: `***ERROR*** A stack overflow in task audio_stream_ta has been detected`

### Symptom
Immediately after connecting to Wi-Fi, the ESP32 crashes and reboots during stream initialization.

### Root Cause
1. The `minimp3` decoder state structure `mp3dec_t` was declared as a local variable on the task stack:
   ```c
   mp3dec_t mp3d; // Size: ~6,700 bytes
   ```
2. The FreeRTOS task stack was allocated only **8,192 bytes (8 KB)**.
3. 8 KB minus 6.7 KB left only ~1.4 KB for the entire task context, `esp_http_client`, and LwIP DNS resolution (`getaddrinfo`). The moment `esp_http_client_open()` was called, the stack overflow hook triggered.

### Solution
1. Moved `mp3dec_t` to the heap:
   ```c
   mp3dec_t *mp3d = malloc(sizeof(mp3dec_t));
   ```
2. Increased `audio_stream_task` stack size from 8 KB to **32 KB (32,768 bytes)** in `app_main`.

---

## 3. Issue: `Core 1 panic'ed (LoadProhibited) in ieee80211_search_node in ROM`

### Symptom
```text
I (8611) AUDIO_PLAYER: HTTP connected! Status = 200, Content-Length = 0
Guru Meditation Error: Core 1 panic'ed (LoadProhibited). Exception was unhandled.
PC: 0x40039726: ieee80211_search_node in ROM
EXCVADDR: 0x82012607
```
The crash occurs the exact instant the first MP3 packet is received and processed.

### Root Cause
Inside `minimp3.h:1718`, the decoder function declares a temporary scratchpad:
```c
mp3dec_scratch_t scratch; // Size: ~16,000 bytes (16 KB)!
```
Desktop MP3 decoders assume stack sizes of 1 MB+. In FreeRTOS, placing a 16 KB array on the stack caused it to smash past the stack boundary, silently corrupting adjacent SRAM containing the Wi-Fi driver's node descriptors. When the network stack (`tcpip_thread`) attempted to send an empty TCP ACK (`tcp_send_empty_ack`), it dereferenced the corrupted pointer and crashed.

### Solution
Inside `main/minimp3.h`, changed line 1718 to static BSS allocation:
```c
static mp3dec_scratch_t scratch;
```
Because only one task calls `mp3dec_decode_frame`, this is 100% thread-safe and reduces the function's stack footprint from 16 KB to under 200 bytes.

---

## 4. Issue: Silent Output from GY-PCM5102A DAC

### Symptoms
The firmware boots and reports audio playing, but no sound or chime is heard from the headphones.

### Checklist & Fixes
1. **SCK Pin Must Be Grounded:** The PCM5102A does not receive an external Master Clock (MCLK) line. Its internal PLL only generates MCLK if the `SCK` pin is connected to `GND`. If left floating, the DAC remains inactive.
2. **XMT (Mute) Pin Must Be High:** Connect `XMT` to **3.3V**. If left floating or tied to GND, the DAC soft-mutes all outputs.
3. **Format (FMT) Pin Must Be Grounded:** Connect `FMT` to `GND` to select standard Philips I2S.
4. **TDA1308 and GY-PCM5102A Power Rail (`5Vin`):** Verify with a multimeter that `5Vin` (Left Header Pin 21) receives a solid 4.8V–5.2V. On the YD-ESP32-S3, USB VBUS does **not** reach `5Vin` unless the `IN-OUT` solder jumper is bridged (see Section 6).

---

## 5. Issue: Wi-Fi Disconnect Loops on Home Routers

### Symptom
Device connects, obtains an IP address, but drops connection every few seconds.

### Root Cause
ESP-IDF's default power saving mode (`WIFI_PS_MIN_MODEM`) turns off the radio between AP beacons. Many Wi-Fi 6 or mesh routers drop sleeping clients due to beacon timeout.

### Solution
In `wifi_init_sta()`, disable modem sleep immediately after starting Wi-Fi:
```c
esp_wifi_set_ps(WIFI_PS_NONE);
```

---

## 6. Issue: `5Vin` Pin Reads ~3.3V or Floating Voltage When Powered via USB (Ref: rtek1000/YD-ESP32-23 Issue #3)

### Symptom
When the YD-ESP32-S3 is powered via USB-C, the firmware boots and streams correctly, but the GY-PCM5102A DAC and TDA1308 headphone amplifier are unpowered or output heavily clipped/distorted audio. A multimeter connected between `5Vin` (Left Pin 21) and `GND` reads only ~3.3V or 0V instead of 5V.

### Root Cause
As documented in [rtek1000/YD-ESP32-23](https://github.com/rtek1000/YD-ESP32-23) Issue #3 and schematic `YD-ESP32-S3-SCH-V1.4`:
* Diode `D3` isolates USB VBUS from the `5Vin` header pin by default.
* The onboard **`IN-OUT` solder jumper** (located near `5Vin` and the USB ports) is left **OPEN** at the factory.
* Without the jumper closed, USB VBUS supplies the onboard 3.3V LDO (`U3`), but does **not** output 5V power to the `5Vin` header pin. The reading of ~3.3V is parasitic back-leakage through ESD diodes and the LDO.

### Solution
1. Unplug the USB-C cable from the board.
2. Locate the **`IN-OUT`** solder jumper pads on the PCB (adjacent to `5Vin` on the component side and marked `IN-OUT`).
3. Apply a small bead of solder to bridge the two pads, shorting across diode `D3`.
4. Reconnect USB-C and verify with a digital multimeter that `5Vin` measures **$4.8\text{ V} - 5.2\text{ V}$** relative to GND.

> [!CAUTION]
> **Host USB Protection:** Once `IN-OUT` is bridged, **NEVER** apply an external 5V power supply to the `5Vin` pin while the board is simultaneously plugged into a computer or charger via USB-C. Because diode D3 is bypassed, external voltage will back-feed directly into your computer's USB port.

---

## 7. Issue: Denormal / Subnormal Floating-Point CPU Surges in PEQ Biquad Filters

### Symptom
When audio playback pauses, stops, or decays to silence, Core 1 CPU load suddenly spikes from ~15% to 100%, occasionally tripping the FreeRTOS Task Watchdog (`Task watchdog got triggered on Core 1`) or causing audible pops when audio resumes.

### Root Cause
In recursive IIR biquad filters:
$$y[n] = b_0 x[n] + b_1 x[n-1] + b_2 x[n-2] - a_1 y[n-1] - a_2 y[n-2]$$
When audio input $x[n]$ ceases, the recursive feedback states ($y[n-1], y[n-2]$) decay exponentially toward zero. Once values drop below the IEEE-754 single-precision normalized limit ($< 1.175 \times 10^{-38}$), they become **subnormal / denormal floats**.
On the Xtensa LX7 core (ESP32-S3), subnormal floating-point arithmetic is not processed in the single-cycle FPU hardware pipeline; it traps to software emulation, consuming 80–120 clock cycles per operation instead of 1 cycle. With 10 active biquad bands running on stereo 48 kHz audio, this produces over 960,000 subnormal operations per second, instantly saturating Core 1!

### Solution
1. **Add Inaudible Anti-Denormal DC Bias:** Add an infinitesimal DC bias (e.g. $10^{-25}\text{ f}$, approximately $-500\text{ dBFS}$, completely inaudible to human ears) to the input stream before filtering:
   ```c
   static inline float peq_anti_denormal(float in) {
       const float anti_denormal = 1e-25f;
       return in + anti_denormal;
   }
   ```
2. **Flush-to-Zero State Clamping:** In the biquad update loop, clamp sub-threshold filter state variables directly to zero:
   ```c
   if (fabsf(state->y1) < 1.0e-15f) state->y1 = 0.0f;
   if (fabsf(state->y2) < 1.0e-15f) state->y2 = 0.0f;
   ```

---

## 8. Issue: Low-Frequency High-Q Filter Instability, NaN Feedback & Loud Clicks

### Symptom
When a user configures a high Q (e.g., $Q > 10.0$) at very low frequencies (e.g., $< 30\text{ Hz}$), the audio output suddenly bursts into deafening white noise or permanent digital silence. Serial logs report `NaN` or `+INF` in the audio stream or biquad coefficients.

### Root Cause
1. **Unit Circle Proximity:** At low cutoff frequencies relative to sample rate ($\omega_0 = 2\pi f_0 / f_s \approx 2\pi(20) / 48000 \approx 0.0026\text{ rad}$), the biquad poles are clustered extremely close to $z = 1.0$ on the unit circle.
2. **Cancellation Catastrophe in Single-Precision Float:** In single-precision 32-bit floats (24-bit mantissa), calculating $1 - \cos(\omega_0)$ suffers from severe catastrophic cancellation error:
   $$\cos(0.0026) \approx 0.9999966 \implies 1 - \cos(\omega_0) \approx 0.0000034$$
   Multiplying this by high Q ($Q \ge 10$) causes extreme rounding errors in denominator coefficients ($a_0, a_1, a_2$), causing poles to drift outside the unit circle ($|z| \ge 1.0$) into unconditional mathematical instability.

### Solution
1. **Clamp Input Parameters in RBJ Calculator:**
   * Clamp center frequency: $f_0 = \text{fmaxf}(20.0\text{f}, \text{fminf}(f_0, 20000.0\text{f}))$.
   * Clamp Q factor: $Q = \text{fmaxf}(0.1\text{f}, \text{fminf}(Q, 10.0\text{f}))$.
   * Clamp Gain: $G = \text{fmaxf}(-24.0\text{f}, \text{fminf}(G, +15.0\text{f}))$.
2. **Pre-Validation Sanity Check:** Verify computed coefficients with `isnan()` / `isinf()` before copying them to the active filter bank. If invalid, reject the update and retain the existing stable coefficients:
   ```c
   if (isnan(b0) || isnan(b1) || isnan(b2) || isnan(a1) || isnan(a2)) {
       ESP_LOGE("PEQ", "Unstable filter coefficients detected! Skipping update.");
       return ESP_ERR_INVALID_ARG;
   }
   ```
3. **Double-Precision Coefficient Calculation:** Compute intermediate biquad coefficients using `double` (64-bit float) on Core 0, then cast to `float` when loading into the runtime SIMD kernel.

---

## 9. Issue: Periodic Audio Ticks/Clicks (~60s) from USB Isochronous Clock Drift

### Symptom
When operating in USB DAC mode (UAC 1.0) connected to a PC or Mac, playback sounds crisp and pristine, but a distinct click, tick, or micro-gap occurs predictably every 30 to 90 seconds.

### Root Cause
**Asynchronous Clock Decoupling:**
* The host PC streams audio timed by its internal USB host controller Start-of-Frame (SOF, 1000 Hz) packet clock.
* The ESP32-S3 feeds the PCM5102A DAC timed by its independent on-board crystal oscillator and I2S APLL.
* Even with high-precision crystals, physical clocks drift by 20–100 PPM (parts per million). For example, if the host sends 48,002 samples/second while the ESP32-S3 plays 47,998 samples/second, the intermediate ringbuffer accumulates a net drift of 4 samples per second.
* After 60 seconds, the 256-sample drift exhausts the ringbuffer margin, causing either a 1-packet buffer overflow (dropped samples) or underrun (zero-padding glitch).

### Solution
1. **Asynchronous Feedback Endpoint (UAC Feedback):**
   * Configure an isochronous feedback endpoint (`EP 0x81 IN`) in the TinyUSB audio descriptor.
   * Calculate current ringbuffer fullness and report the adjusted fractional sample rate (10.14 or 12.14 format) back to the host operating system every 1 ms. The host PC will dynamically throttle or accelerate its USB audio packets to match the ESP32-S3 I2S consumption rate.
2. **Adaptive Ringbuffer Watermarking (Software Fallback):**
   * Maintain a target ringbuffer watermark (e.g. 50% capacity).
   * If watermark exceeds 75%, seamlessly discard 1 sample at a zero-crossing point.
   * If watermark drops below 25%, duplicate 1 sample at a zero-crossing point. Zero-crossing micro-adjustments are mathematically imperceptible to human ears.

---

## 10. Issue: Windows WebHID `SecurityError: Failed to open the device`

### Symptom
In Chrome or Edge on Windows 10/11, the user visits the web tuning dashboard and calls `navigator.hid.requestDevice()`. The ESP32-S3 device appears in the pairing modal, but calling `await device.open()` fails with:
```text
SecurityError: Failed to open the device.
```
The exact same device and web page open without errors on Android, macOS, and Linux.

### Root Cause
**Windows OS Top-Level Collection Locking:**
The Windows HID class driver (`hidclass.sys`) aggressively enforces exclusive security locks on top-level HID collections that use:
* Usage Page `0x01` (Generic Desktop Controls: Mouse, Keyboard, Joysticks)
* Usage Page `0x0C` (Consumer Audio Controls: Volume Up/Down, Mute, Media Play/Pause)
* Usage Page `0x07` (Keyboards/Keypads)
If the custom PEQ HID interface accidentally declares Usage Page `0x0C` or `0x01`, Windows assigns exclusive access to the Windows Multimedia Subsystem and completely blocks WebHID browsers from opening the device handle.

### Solution
In the TinyUSB HID report descriptor, **always** use a dedicated **Vendor-Defined Usage Page** (`0xFF00` to `0xFFFF`):
```c
// Correct: Vendor-defined usage page (never blocked by Windows)
0x06, 0x60, 0xFF,  // Usage Page (Vendor-Defined 0xFF60)
0x09, 0x61,        // Usage (Vendor-Defined 0x61)
0xA1, 0x01,        // Collection (Application)
// ... 64-byte IN/OUT reports ...
0xC0               // End Collection
```

---

## 11. Issue: TinyUSB ISR Guru Meditation / Interrupt Watchdog Timeout in USB Callbacks

### Symptom
When interacting with USB Audio or WebHID, the ESP32-S3 suddenly crashes with:
```text
Guru Meditation Error: Core 0 panic'ed (Interrupt wdt timeout on CPU0)
- or -
assert failed: xQueueGenericSend queue.c:832
- or -
vTaskDelay called from ISR context!
```

### Root Cause
The TinyUSB stack callbacks (`tud_audio_rx_done_cb()`, `tud_hid_set_report_cb()`) execute inside **High-Priority Hardware Interrupt (ISR) Context**.
Common illegal actions inside these callbacks include:
1. Calling blocking FreeRTOS primitives (e.g. `vTaskDelay()`, `xSemaphoreTake()` with timeout).
2. Calling `nvs_commit()` or SPI flash operations (which take 10–50 ms and require task context).
3. Calling standard `malloc()` or `free()`.
4. Using non-ISR queue primitives (`xQueueSend` instead of `xQueueSendFromISR`).

### Solution
1. **Strict ISR Discipline:** In `tud_hid_set_report_cb()`, only copy the 64-byte packet into volatile SRAM or post a pointer to an ISR-safe queue:
   ```c
   void tud_hid_set_report_cb(...) {
       BaseType_t xHigherPriorityTaskWoken = pdFALSE;
       xQueueSendFromISR(s_hid_cmd_queue, buffer, &xHigherPriorityTaskWoken);
       portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
   }
   ```
2. **Execute DSP & NVS in Task Context:** Let a dedicated Core 0 worker task dequeue the command, recalculate Biquad coefficients, and handle flash persistence.

---

## 12. Issue: Core 1 Audio Stutter & Cache Panic (`Cache access while memory is disabled`) during NVS Flash Writes

### Symptom
Whenever the user explicitly clicks `[💾 Save to Preset]`, audio playback either stutters for 30–50 ms or the system crashes with:
```text
Guru Meditation Error: Core 1 panic'ed (Cache error)
Core 1 was executing code while Flash/PSRAM cache was disabled.
```

### Root Cause
When Core 0 calls `nvs_commit()`, the ESP32-S3's hardware SPI flash controller **disables the external memory bus cache** (both SPI Flash and Octal PSRAM) for the duration of the flash sector erase/write cycle (typically 20–40 ms).
If Core 1's real-time audio pipeline (`audio_stream_task`) attempts to:
1. Execute any function that is located in Flash rather than internal SRAM, or
2. Read/write audio buffers located in external PSRAM,
Core 1 immediately triggers a hardware Cache Error exception!

### Solution
1. **Place Real-Time Audio Code in IRAM:** Tag all time-critical audio functions with `IRAM_ATTR`:
   ```c
   IRAM_ATTR void peq_process_interleaved_s16(int16_t *samples, int num_samples, peq_bank_t *peq);
   IRAM_ATTR void i2s_tx_dma_isr(void *arg);
   ```
2. **Place Active Biquad Banks in DRAM:** Ensure `s_active_peq_bank` and active audio stream buffers reside in internal SRAM with `DRAM_ATTR` (never allocate with `MALLOC_CAP_SPIRAM` for the active playback ringbuffer).
3. **Deep I2S DMA Ringbuffer:** Configure the I2S DMA buffer depth to at least **64–100 ms** ($4 \times 1024$ samples in internal SRAM):
   ```c
   i2s_std_config_t std_cfg = {
       .dma_desc_num = 8,
       .dma_frame_num = 512, // 8 * 512 = 4096 frames (~85 ms @ 48 kHz)
   };
   ```
   This allows the hardware DMA controller to feed the PCM5102A autonomously from internal SRAM without CPU intervention while SPI flash write operations are in progress on Core 0.

---

## 13. Issue: `Brownout detector was triggered` during Wi-Fi Bursts or USB OTG Insertion

### Symptom
The ESP32-S3 suddenly resets during Wi-Fi AP scanning, HTTP connection establishment, or when plugging into a smartphone's USB-C port, printing:
```text
Brownout detector was triggered
```

### Root Cause
The ESP32-S3's 2.4 GHz RF power amplifier draws brief current spikes of **450–500 mA** during 802.11b preamble transmissions.
* If powered from a smartphone's OTG port or an unpowered USB hub, cable resistance and current limiting cause the $V_{in}$ rail to dip momentarily below the internal 2.8V brownout comparator threshold.

### Solution
1. **Hardware Decoupling:** Place a **100 µF to 220 µF low-ESR electrolytic capacitor** in parallel with a **10 µF MLCC** directly across `5Vin` and `GND` near Left Header Pin 21.
2. **Disable Wi-Fi in USB DAC Mode:** When operating as a USB DAC, call `esp_wifi_stop()` and `esp_wifi_deinit()` to power down the RF radio domain entirely.
3. **Configure Brownout Threshold in `sdkconfig`:**
   ```kconfig
   CONFIG_ESP_BROWNOUT_DET=y
   CONFIG_ESP_BROWNOUT_DET_LVL_SEL_0=y # 2.43V lowest trigger threshold
   ```

---

## 14. Issue: MicroSD (SDMMC) Audio Underruns during High-Bitrate FLAC/WAV Playback

### Symptom
MP3 streams play flawlessly, but 96 kHz / 24-bit or 44.1 kHz / 16-bit uncompressed WAV / FLAC files played from the MicroSD card suffer from intermittent audio dropouts every few seconds.

### Root Cause
1. **SD Card Internal Wear-Leveling Garbage Collection:** Consumer MicroSD cards periodically stall bus operations for **50 to 120 ms** to execute internal NAND wear-leveling and block relocation.
2. **Small FATFS Buffer:** If the application reads only 4 KB or 8 KB clusters synchronously in the audio task, an 80 ms SD card stall starves the I2S DMA buffer.

### Solution
1. **128 KB PSRAM Double-Buffering:** Decouple SD card reads from audio decoding using a dedicated `sd_reader_task` and a **128 KB PSRAM ringbuffer**.
2. **Multi-Block Sector Reads:** Read data from FatFS in clusters of $4096\text{ bytes}$ or $8192\text{ bytes}$ aligned to 4-byte boundaries.
3. **Card Bus Clock:** Set SDMMC host bus frequency to **40 MHz** (`SDMMC_FREQ_HIGHSPEED`).

---

## 15. Reference for AI Agents: Master Diagnostic Triage Decision Tree

When diagnosing runtime errors from serial console logs (`idf.py monitor`), automated agents should follow this triage table:

| Log Signature / Error Pattern | Root Cause | Mandatory Automated Fix |
| :--- | :--- | :--- |
| `wifi:state: init -> auth -> init (0x1c0)` in <20 ms with `reason 202` | Hotspot 802.11ax Wi-Fi 6 rejection or MAC filtering | Set `WIFI_PROTOCOL_11B \| WIFI_PROTOCOL_11G \| WIFI_PROTOCOL_11N`; advise user to disable "Wi-Fi 6 standard" or enable "Maximize Compatibility". |
| `***ERROR*** A stack overflow in task audio_stream_ta` | Task stack exhaustion (decoder structs on stack) | Increase task stack to `32768`; ensure `mp3dec_scratch_t` is `static` in BSS. |
| `Guru Meditation Error: Core 1 panic'ed (LoadProhibited) in ieee80211_search_node` | Stack smashing: local variable overflow overwrote Wi-Fi heap | Check `minimp3.h:1718` or decoder buffers; convert local struct to `static mp3dec_scratch_t scratch;`. |
| `Guru Meditation Error: Core 0 panic'ed (Cache error)` | Peripheral assigned to Octal PSRAM pins (GPIO 33–37) | Remap peripheral pins away from `GPIO 33, 34, 35, 36, 37`. |
| `Guru Meditation Error: Core 1 panic'ed (Cache access while memory is disabled)` | Realtime audio task executing from Flash during `nvs_commit()` | Add `IRAM_ATTR` to Core 1 audio & I2S functions; place biquad state in `DRAM_ATTR`; increase I2S DMA buffer to $\ge 64\text{ ms}$. |
| `Task watchdog got triggered on Core 1` during silence | Subnormal / Denormal floating-point operations in IIR biquad | Add anti-denormal DC bias ($10^{-25}\text{f}$) or flush states $<10^{-15}\text{f}$ to zero in biquad loop. |
| Loud white noise / `NaN` in biquad coefficients | Extreme Q ($>10$) or low frequency ($<20\text{ Hz}$) instability | Clamp $Q \le 10$, clamp $f_0 \ge 20\text{ Hz}$, add `isnan()` sanity check before applying filter coefficients. |
| Periodic click/tick every 30–90s in USB DAC mode | Asynchronous clock drift between USB host SOF and I2S crystal | Implement UAC Isochronous Feedback endpoint (`0x81 IN`) or zero-crossing adaptive ringbuffer watermarking. |
| WebHID `SecurityError: Failed to open the device` on Windows | Windows exclusive lock on Usage Page `0x01` or `0x0C` | Use Vendor-Defined Usage Page (`0xFF60`, Usage `0x61`) in TinyUSB HID report descriptor. |
| `assert failed: xQueueGenericSend` / ISR watchdog panic | Blocking call or non-ISR API executed in TinyUSB callback | Use `xQueueSendFromISR` only; defer biquad recalculation and NVS commits to Core 0 task context. |
| `Brownout detector was triggered` | High current transient during Wi-Fi burst on weak USB power | Add $100\ \mu\text{F}$ low-ESR capacitor across $5\text{V}_{in}$ and GND; shut down Wi-Fi radio when in USB DAC mode. |
| Audio stutter during high-res FLAC playback from MicroSD | SD card internal wear-leveling stall (50–120 ms) | Allocate 128 KB PSRAM ringbuffer for `sd_reader_task`; read in 4 KB multi-block clusters. |
| `i2s_common: i2s_channel_reconfig_std_clock(...) failed` | Attempted to reconfigure I2S clock while channel was active | Always call `i2s_channel_disable(tx_chan)` before reconfiguring clock, then call `i2s_channel_enable(tx_chan)`. |
| Audio silence / DAC unpowered / `5Vin` reads ~3.3V | `IN-OUT` solder jumper is open (Issue #3) | Solder-bridge the `IN-OUT` jumper pads to bypass diode D3; verify 5V with multimeter. |
| Complete audio silence but serial logs show streaming | PCM5102A hardware configuration pins floating | Verify `SCK -> GND` (PLL mode), `XMT -> 3.3V` or `GPIO 7 HIGH` (unmute), `FMT -> GND` (Philips format). |
| PEQ slider dragging causes clicks or flash wear | Committing to NVS on every slider move / no crossfade | Ensure live tuning updates volatile SRAM only; use 15ms crossfade and explicit "Save to Preset" with Delta check. |


