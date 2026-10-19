# USB DAC Implementation Guide & Agentic AI Reference

This guide provides the complete technical specifications, software architecture, and step-by-step implementation instructions for converting the **YD-ESP32-S3 (N16R8)** into a driverless, high-resolution **USB Audio Class (UAC) DAC** outputting to the **GY-PCM5102A** and **TDA1308**.

> **Note for Agentic AI / AI Assistants:**  
> This document contains structured architectural patterns, function prototypes, descriptor tables, and an actionable implementation checklist designed for automated code generation and refactoring.

---

## 1. System Overview & Dual-Port Topology

The YD-ESP32-S3 board features a distinctive advantage for USB audio development: **Two independent USB-C ports**.

```
                           YD-ESP32-S3 Development Board
                   ┌──────────────────────────────────────────────┐
  [Host PC / Mac]──►[ "USB" Port (Native USB OTG)               ]│
 (Digital Audio)   │  • GPIO 19: D- (USB D-)                      ││
                   │  • GPIO 20: D+ (USB D+)                      ││
                   │  • Class: USB Audio Class 1.0/2.0            ││
                   │                                              ││
                   │  [ESP32-S3 Core 1: TinyUSB Audio Task]       ││
                   │  [ESP32-S3 Core 0: Background / Web / PEQ]   ││
                   │                                              ││
                   │  • I2S Bus Output:                           ││
                   │    GPIO 4: LCK (WS) ─────────────────────────┼┼──► [GY-PCM5102A DAC]
                   │    GPIO 5: DIN (Data) ───────────────────────┼┼──► [TDA1308 Amp]
                   │    GPIO 6: BCK (Bit Clock) ──────────────────┼┼──► [Headphones]
                   │                                              ││
  [Developer PC]───►[ "COM" Port (CH343 / CP2102 Serial UART)   ]│
(Logs & Flashing)  │  • Real-time ESP_LOG monitoring             ││
                   │  • Flashing without interrupting USB Audio   ││
                   └──────────────────────────────────────────────┘
```

* **"USB" Port (Native USB OTG):** Connected directly to ESP32-S3 internal PHY (`GPIO 19: USB_D-`, `GPIO 20: USB_D+`). Receives isochronous audio stream packets from the host OS.
* **"COM" Port (UART-to-USB Bridge):** Remains connected to your development terminal. Allows simultaneous debugging, flashing, and monitoring while the "USB" port is acting as an active sound card for your PC, Mac, Android, or iOS device.

---

## 2. USB Audio Class (UAC) Standards Comparison

| Parameter | USB Audio Class 1.0 (UAC 1.0) *(Recommended)* | USB Audio Class 2.0 (UAC 2.0) |
| :--- | :--- | :--- |
| **Max Bit Depth / Rate** | Up to 24-bit / 96 kHz (16-bit / 44.1 & 48 kHz standard) | Up to 32-bit / 384 kHz |
| **Host Driver Requirements** | **100% Driverless Plug-and-Play** on Windows 10/11, macOS, Linux, Android, iOS | Driverless on macOS/Linux/iOS/modern Win10+, but can experience descriptor negotiation issues on older Windows |
| **USB Endpoint Speed** | Full Speed (12 Mbps Isochronous) | High Speed (Requires external USB PHY on ESP32-S3) |
| **Complexity** | Low / Extremely stable on TinyUSB | High / Sensitive to jitter |
| **Recommendation** | **Select UAC 1.0 (16-bit / 48 kHz stereo)** for maximum reliability and universal compatibility | Optional upgrade path |

---

## 3. TinyUSB Audio Stack Integration in ESP-IDF

ESP-IDF integrates **TinyUSB** via the `esp_tinyusb` component.

### 3.1 Component Dependency (`main/idf_component.yml`)
To equip the project with TinyUSB support, declare the dependency:
```yaml
dependencies:
  espressif/esp_tinyusb: "^1.4.0"
```

### 3.2 Kconfig Requirements (`sdkconfig.defaults`)
```ini
CONFIG_TINYUSB=y
CONFIG_TINYUSB_AUDIO_ENABLED=y
CONFIG_TINYUSB_AUDIO_SAMPLE_RATE=48000
CONFIG_TINYUSB_AUDIO_CHANNELS=2
CONFIG_TINYUSB_AUDIO_BIT_DEPTH=16
```

---

## 4. Software Architecture & Buffer Synchronization

### 4.1 Clock Domain Decoupling
USB audio packets arrive every **1 ms** (USB Full-Speed Frame Interval). The ESP32 I2S hardware transmits audio clocked by the ESP32's internal PLL clock. Because the host PC clock and the ESP32 crystal clock drift slightly over time:
* An intermediate **FreeRTOS Ringbuffer** (8 KB to 16 KB) acts as an elastic buffer between TinyUSB's isochronous reception callback and the I2S DMA transmitter.
* If buffer level exceeds 75%, drop 1 frame; if buffer level falls below 25%, repeat/pad 1 frame (or implement an asynchronous feedback endpoint).

```
   [USB Host]
       │ Isochronous OUT Packets (1 ms interval)
       ▼
   tud_audio_rx_done_cb()
       │
       ▼ xRingbufferSend()
 ┌────────────────────────────────────────┐
 │ FreeRTOS Byte Ringbuffer (16 KB)       │  <── Decouples USB and I2S clocks
 └────────────────────────────────────────┘
       │
       ▼ xRingbufferReceiveUpTo()
   audio_output_task()
       │
       ▼ i2s_channel_write()
   [I2S DMA Engine] ──► GPIO 4/5/6 ──► [PCM5102A] ──► [TDA1308]
```

### 4.2 Core TinyUSB Audio Callbacks
```c
// Invoked when an Isochronous audio packet is received from the host
bool tud_audio_rx_done_pre_read_cb(uint8_t rhport, uint16_t n_bytes_received, uint8_t func_id, uint8_t ep_out, uint8_t cur_alt_setting)
{
    uint8_t buffer[256];
    uint16_t bytes_read = tud_audio_read(buffer, n_bytes_received);
    if (bytes_read > 0 && s_usb_ringbuf) {
        xRingbufferSend(s_usb_ringbuf, buffer, bytes_read, 0);
    }
    return true;
}

// Invoked when host adjusts mute or volume
bool tud_audio_set_req_channel_cb(uint8_t rhport, tusb_control_request_t const *p_request)
{
    // Handle Volume (Feature Unit) and Mute
    return true;
}
```

---

## 5. FiiO JA11 / Moondrop Style Live PEQ Tuning over USB (Composite UAC + WebHID Architecture)

Commercial audiophile dongle DACs with built-in DSP (such as the **FiiO JA11**, **Moondrop FreeDSP / Dawn Pro**, and **Qudelix 5K**) do **not** rely on Wi-Fi for parameter tuning. Instead, they stream audio and receive real-time equalization adjustments **over the exact same single USB-C cable**.

```
                     FIIO JA11 / MOONDROP STYLE USB ARCHITECTURE
  ┌─────────────────────────────────────────────────────────────────────────────┐
  │ HOST PC / MAC / ANDROID PHONE                                               │
  │   • Operating System Audio Engine (Spotify, YouTube, Foobar2000, Tidal)     │
  │   • Google Chrome / Edge Browser (WebHID API: navigator.hid)                │
  └──────────────────────────────────────┬──────────────────────────────────────┘
                                         │ Single USB Type-C Cable
                                         ▼
  ┌─────────────────────────────────────────────────────────────────────────────┐
  │ YD-ESP32-S3 NATIVE USB OTG PORT (GPIO 19: D-, GPIO 20: D+)                  │
  │                                                                             │
  │   ┌───────────────────────────────────┬─────────────────────────────────┐   │
  │   │ INTERFACE 0 & 1: USB AUDIO (UAC)  │ INTERFACE 2: VENDOR USB HID     │   │
  │   │ • Standard UAC 1.0 Isochronous    │ • Custom 64-byte Raw Reports    │   │
  │   │ • 16-bit / 48 kHz Stereo Stream   │ • WebHID Compatible (Driverless)│   │
  │   │ • Bit-Perfect PCM Playback        │ • Live PEQ Tuning & Curve Read  │   │
  │   └─────────────────┬─────────────────┴─────────────────┬───────────────┘   │
  │                     │                                   │                   │
  │                     │ Audio Frames (1 ms)               │ Tuning Packets    │
  │                     ▼                                   ▼                   │
  │         ┌───────────────────────┐           ┌───────────────────────┐       │
  │         │ FreeRTOS Audio Buffer │           │ TinyUSB tud_hid_cb    │       │
  │         │ Elastic Ringbuffer    │           │ • Updates SRAM Biquads│       │
  │         └───────────┬───────────┘           │ • Resets 3s NVS Timer │       │
  │                     │                       └───────────┬───────────┘       │
  │                     ▼                                   ▼                   │
  │         ┌───────────────────────────────────────────────────────┐           │
  │         │ CORE 1: DSP PARAMETRIC EQUALIZER ENGINE (esp-dsp SIMD)│           │
  │         │   15 ms Linear Crossfade -> I2S Output -> PCM5102A    │           │
  │         └───────────────────────────────────────────────────────┘           │
  └─────────────────────────────────────────────────────────────────────────────┘
```

### 5.1 Why USB HID + WebHID?
* **100% Driverless:** USB Audio Class and USB Human Interface Device (HID) are natively supported by Windows 10/11, macOS, Linux, and Android out of the box without installing proprietary drivers or kernel extensions.
* **Zero Desktop Software Installation:** Modern Chromium-based browsers (Chrome, Edge, Brave, Opera, and Android Chrome) feature the **WebHID API** (`navigator.hid`). A static HTML page can open the HID channel, read current curves, and stream slider updates at 60 FPS directly over USB.
* **Completely Offline:** Works on airplanes, trains, or in locations with zero Wi-Fi or cellular connectivity.

---

### 5.2 TinyUSB Composite Descriptor Configuration

To enable both Audio Streaming and HID tuning concurrently, configure TinyUSB with a composite descriptor:

```c
#include "tusb.h"

// Report descriptor defining a 64-byte bi-directional raw data channel
static const uint8_t desc_hid_report[] = {
    TUD_HID_REPORT_DESC_GENERIC_INOUT(64)
};

// Composite Configuration Descriptor
enum {
    ITF_NUM_AUDIO_CONTROL = 0,
    ITF_NUM_AUDIO_STREAMING,
    ITF_NUM_HID,
    ITF_NUM_TOTAL
};

#define CONFIG_TOTAL_LEN  (TUD_CONFIG_DESC_LEN + TUD_AUDIO_HEADPHONE_STEREO_DESC_LEN + TUD_HID_INOUT_DESC_LEN)
```

---

### 5.3 64-Byte Raw HID Tuning Packet Schema

Tuning commands and profile queries travel in fixed 64-byte packets:

```c
#define PEQ_HID_REPORT_SIZE  64
#define PEQ_HID_MAGIC        0x4551  // "EQ" in ASCII

typedef enum {
    CMD_GET_ACTIVE_PROFILE = 0x01,  // Request current settings
    CMD_SET_BAND_LIVE      = 0x02,  // Real-time slider move (volatile in SRAM only, 0 flash writes)
    CMD_SET_PREAMP_LIVE    = 0x03,  // Preamp headroom change (volatile in SRAM only)
    CMD_SELECT_PROFILE     = 0x04,  // Switch active slot (0=User, 1..3=Presets)
    CMD_SAVE_TO_PRESET     = 0x05,  // Explicit flash commit to target preset slot (Delta-checked)
    CMD_RESET_TO_PRESET    = 0x06,  // Reset user slot from factory preset
    CMD_SET_BAND_COUNT     = 0x07,  // Dynamically configure active band count (1 to 10 bands)
} peq_hid_cmd_t;

typedef struct __attribute__((packed)) {
    uint16_t magic;         // PEQ_HID_MAGIC (0x4551)
    uint8_t  cmd;           // peq_hid_cmd_t
    uint8_t  band_idx;      // 0 to 9 (or target count for CMD_SET_BAND_COUNT, or slot for CMD_SAVE_TO_PRESET)
    uint8_t  filter_type;   // 1=Peaking, 2=LowShelf, 3=HighShelf, 4=HPF, 5=LPF
    uint8_t  enabled;       // 1=Active, 0=Bypassed
    uint16_t freq_hz;       // 20 to 20,000 Hz
    int16_t  gain_db_x10;   // e.g. +35 = +3.5 dB, -45 = -4.5 dB
    uint16_t q_x100;        // e.g. 141 = 1.41, 71 = 0.71
    uint8_t  reserved[52];  // Padded to 64 bytes (reserved[0] = active dynamic num_bands)
} peq_hid_packet_t;
```

---

### 5.4 TinyUSB HID Event Callbacks & Flash Protection

```c
// Invoked when host sends a 64-byte HID feature report over USB
void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                           hid_report_type_t report_type, uint8_t const* buffer, uint16_t bufsize)
{
    if (bufsize == sizeof(peq_hid_packet_t)) {
        peq_hid_packet_t *pkt = (peq_hid_packet_t *)buffer;
        if (pkt->magic != PEQ_HID_MAGIC) return;

        if (pkt->cmd == CMD_SET_BAND_LIVE) {
            // 1. Update active Biquad coefficients in volatile SRAM (Auditioning Mode)
            peq_update_band_live(pkt->band_idx, pkt->filter_type, 
                                pkt->freq_hz, pkt->gain_db_x10 / 10.0f, pkt->q_x100 / 100.0f);
            
            // 2. Trigger 15ms double-buffered crossfade on Core 1
            peq_trigger_crossfade();
            
            // 3. Mark dirty flag; ZERO bytes written to flash while dragging!
            s_peq_dirty = true;
        } 
        else if (pkt->cmd == CMD_SET_BAND_COUNT) {
            // Dynamically scale active band count (1 to 10 bands) with 15ms crossfade
            peq_dsp_set_band_count(pkt->band_idx);
        }
        else if (pkt->cmd == CMD_SAVE_TO_PRESET) {
            // Explicit user save to preset: Delta check + commit to NVS
            const char *label = (const char *)&pkt->reserved[0];
            peq_storage_save_active_to_preset(pkt->band_idx, label);
        }
        else if (pkt->cmd == CMD_SELECT_PROFILE) {
            peq_select_profile_slot(pkt->band_idx);
        }
    }
}
```

---

### 5.5 Frontend WebHID Implementation (Browser JavaScript)

Users can tune the PEQ by visiting a web application in Chrome, Edge, or Android Chrome:

```javascript
class UsbDacTuner {
    constructor() {
        this.device = null;
    }

    async connect() {
        // Filter for Espressif Vendor ID (0x303A)
        const filters = [{ vendorId: 0x303A, productId: 0x4002 }];
        const [selectedDevice] = await navigator.hid.requestDevice({ filters });
        
        if (!selectedDevice) return;
        this.device = selectedDevice;
        await this.device.open();
        console.log("Connected to ESP32-S3 DSP DAC via WebHID!");

        // Listen for profile feedback from the DAC
        this.device.oninputreport = (event) => {
            this.handleInputReport(event.data);
        };
    }

    // Called on slider drag (60 FPS smooth tuning over USB - VOLATILE ONLY)
    sendBandLive(bandIdx, type, freqHz, gainDb, qVal) {
        if (!this.device || !this.device.opened) return;

        const buffer = new ArrayBuffer(64);
        const view = new DataView(buffer);

        view.setUint16(0, 0x4551, true);        // Magic "EQ"
        view.setUint8(2, 0x02);                 // CMD_SET_BAND_LIVE
        view.setUint8(3, bandIdx);              // Band (0-9)
        view.setUint8(4, type);                 // Filter Type
        view.setUint8(5, 1);                    // Enabled = true
        view.setUint16(6, freqHz, true);        // Frequency in Hz
        view.setInt16(8, Math.round(gainDb * 10), true); // Gain x10
        view.setUint16(10, Math.round(qVal * 100), true); // Q x100

        // Transmitted over USB-C in <1 millisecond!
        this.device.sendReport(0x00, new Uint8Array(buffer));
    }

    // Explicit Save to Preset (Called when user clicks "💾 Save to Hardware")
    saveCurrentToPreset(slot = 0, name = "User Preset") {
        if (!this.device || !this.device.opened) return;

        const buffer = new ArrayBuffer(64);
        const view = new DataView(buffer);

        view.setUint16(0, 0x4551, true);        // Magic "EQ"
        view.setUint8(2, 0x05);                 // CMD_SAVE_TO_PRESET
        view.setUint8(3, slot);                 // Target Preset Slot (0 = User Slot)

        // Encode preset name in reserved padding bytes
        const encoder = new TextEncoder();
        const nameBytes = encoder.encode(name.slice(0, 31));
        new Uint8Array(buffer, 12, nameBytes.length).set(nameBytes);

        this.device.sendReport(0x00, new Uint8Array(buffer));
        console.log(`Explicitly committed active EQ to hardware preset slot ${slot}`);
    }

    // Dynamic Band Count Selector (3, 5, 7, 10 bands)
    setBandCount(numBands) {
        if (!this.device || !this.device.opened) return;

        const buffer = new ArrayBuffer(64);
        const view = new DataView(buffer);

        view.setUint16(0, 0x4551, true);        // Magic "EQ"
        view.setUint8(2, 0x07);                 // CMD_SET_BAND_COUNT
        view.setUint8(3, numBands);             // Desired active band count (1-10)

        this.device.sendReport(0x00, new Uint8Array(buffer));
        console.log(`Requested active band count change to ${numBands}`);
    }

    // Bidirectional sync handler reading hardware reports
    handleInputReport(data) {
        if (data.byteLength < 64) return;
        const magic = data.getUint16(0, true);
        if (magic !== 0x4551) return;

        const bandIdx = data.getUint8(3);
        const filterType = data.getUint8(4);
        const freqHz = data.getUint16(6, true);
        const gainDb = data.getInt16(8, true) / 10.0;
        const qFactor = data.getUint16(10, true) / 100.0;

        // Byte offset 16 (reserved[0]) carries the active dynamic band count
        const activeBandCount = data.getUint8(16);
        if (activeBandCount >= 1 && activeBandCount <= 10) {
            this.syncBandCountUI(activeBandCount);
        }

        this.updateBandUI(bandIdx, filterType, freqHz, gainDb, qFactor);
    }
}
```

> [!TIP]
> **Complete WebHID Tuner Web App:**  
> A production, standalone single-page WebHID tuner is provided in [`tools/webhid_tuner/index.html`](../../tools/webhid_tuner/index.html). It features:
> * **Interactive HTML5 Canvas Frequency Response Visualizer:**
>   * **Draggable Control Nodes:** Direct manipulation of band center points on the response graph ($X$-axis: logarithmic frequency 20 Hz – 20 kHz; $Y$-axis: gain -24 dB to +12 dB).
>   * **Mouse Wheel / Scroll Q Adjustment:** Hovering over any control node and scrolling adjusts filter Q Factor in real time ($0.10$ to $10.00$) with $-3\text{ dB}$ bandwidth guides.
>   * **Live Tooltips & Dynamic Bandwidth Guides:** Hovering or dragging displays sleek HUD tooltips with band metrics, center frequency guide lines, and shaded $-3\text{ dB}$ passband visualizer.
>   * **Double-Click Actions:** Double-clicking an existing node resets its gain to 0.0 dB; double-clicking empty canvas space instantiates a new band at that frequency and gain.
> * **Custom & Dynamic Band Count Configuration (1 to 10 Bands):**
>   * **Custom Number Input Stepper:** Type any number of bands between 1 and 10 with instant DSP hardware synchronization.
>   * **`+ Add Band` Button:** Appends a new band placed at the optimal octave gap among existing filters.
>   * **Per-Card Delete Button (`✕`):** Remove individual bands down to a minimum of 1 band.
>   * **Preset Selectors:** Quick selection between 3-Band (Tone), 5-Band (Standard), 7-Band (Extended), and 10-Band (Pro Mastering) with automatic ISO standard frequency allocation.
> * **Custom Value (Within Bound) Numeric Inputs:** Direct numeric input fields for every band parameter alongside range sliders with live two-way binding:
>   * **Frequency:** `20 Hz` to `20,000 Hz` (Step: `1 Hz`)
>   * **Gain:** `-24.0 dB` to `+12.0 dB` (Step: `0.1 dB`)
>   * **Q Factor:** `0.10` to `10.00` (Step: `0.01` precision)
>   * **Preamp Headroom:** `-12.0 dB` to `0.0 dB` (Step: `0.1 dB`)
> * **Input Validation & Bound Clamping:** Typing an out-of-range value triggers CSS `:invalid` feedback; on blur or Enter, the value is clamped strictly within safe mathematical bounds (`clamp(val, min, max)`).
> * **60 FPS Live Dragging & Auditioning:** Volatile SRAM mode guarantees **0 flash wear** while dragging nodes or sliders.
> * **Bidirectional State Synchronization:** Reads hardware state on connection and reflects hardware reports in real time.
> * **Hardware NVS Flash Persistence:** Commits active settings via explicit "Save to Hardware NVS" with CRC32 and flash delta checks.

---

## 6. Reference for AI Agents: Implementation Checklist & Invariants

### 6.1 Critical Invariants
1. **Never Reconfigure GPIO 19 & 20:** When USB is enabled, GPIO 19 (`USB_D-`) and GPIO 20 (`USB_D+`) are locked to the internal USB-OTG PHY. Never assign I2S, I2C, or buttons to these pins.
2. **Interrupt Context Rules:** The TinyUSB callback `tud_audio_rx_done_cb` executes in interrupt context. Never perform blocking operations (`vTaskDelay`, blocking `i2s_channel_write`) inside this callback; write directly into an intermediate ringbuffer using ISR-safe primitives (`xRingbufferSendFromISR`).
3. **Volatile Auditioning & Explicit Save Rule:** `tud_hid_set_report_cb` MUST update volatile SRAM coefficients only on `CMD_SET_BAND_LIVE`. Committing to NVS flash occurs ONLY on explicit `CMD_SAVE_TO_PRESET` commands (accompanied by Delta checking) to guarantee zero unneeded flash wear during slider dragging.

### 6.2 Implementation Checklist
Future AI agents tasked with generating or refactoring this project into a USB DAC should follow this sequential execution plan:

- [x] **Step 1:** Add `espressif/esp_tinyusb: "^1.4.0"` to `main/idf_component.yml`.
- [x] **Step 2:** Configure root `CMakeLists.txt` & `sdkconfig`:
  - Enable `CFG_TUD_AUDIO=1`, `CFG_TUD_AUDIO_FUNC_1_SAMPLE_RATE=48000`, and `CFG_TUD_HID=1`.
  - Set USB Device Vendor ID (`0x303A`), Product ID (`0x4002`), and Device Strings (`"Ascension Audio ESP32-S3 Hi-Fi DSP DAC"`).
- [x] **Step 3:** Implement the Composite USB descriptor:
  - 1 AC (Audio Control) Interface (with Volume and Mute feature units).
  - 1 AS (Audio Streaming) Interface (16-bit stereo, 48 kHz Adaptive Isochronous OUT).
  - 1 HID Interface (64-byte Generic IN/OUT raw report descriptor for WebHID).
- [x] **Step 4:** Implement TinyUSB Audio & HID callbacks:
  - Audio: `tud_audio_set_itf_cb`, `tud_audio_set_req_entity_cb`, `tud_audio_get_req_entity_cb`, `tud_audio_set_req_ep_cb`, `tud_audio_get_req_ep_cb`.
  - HID: `tud_hid_set_report_cb`, `tud_hid_get_report_cb`, `tud_hid_descriptor_report_cb`.
- [x] **Step 5:** Create the audio pipeline routing on Core 1:
  - In `AUDIO_SOURCE_STANDALONE`, stream from `tud_audio_read()` into `peq_dsp_process_interleaved_s16()` and feed `audio_pipeline_write()`.
  - Route through the dynamic PEQ engine with 15ms click-free crossfade.
- [x] **Step 6:** Verify compilation with `idf.py build`, flash via UART, and verify driverless UAC 1.0 soundcard + WebHID live tuning.

---

## 7. Future Enhancements & Architectural Roadmap

For the detailed mathematical formulations, descriptor configurations, and implementation plans for upcoming capabilities, refer to:
* **[Future Enhancements & Architectural Roadmap](../firmware/future_enhancements.md)**
  * **DSP:** Extended biquads (HPF, LPF, Notch, BPF), 24-bit/96kHz pipeline, 44.1k/48k dual-rate, dual-mono L/R PEQ, A/B toggle.
  * **Hardware & OLED:** Live mini response curve graph on SSD1306, stereo VU peak meters, rotary encoder menu.
  * **Tooling:** AutoEQ / Equalizer APO file import/export, PWA offline app.
  * **Power:** USB Suspend sleep mode and 10s zero-signal auto soft-mute.
