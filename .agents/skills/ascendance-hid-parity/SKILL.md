---
name: ascendance-hid-parity
description: >-
  Use this skill whenever adding, modifying, or debugging DSP parameters, preset
  slots, OLED menu items, or 64-byte USB HID commands (`0x4551`) across the
  ESP32-S3 firmware, React WebHID tuner, Android Companion app, and documentation.
---

# ASCENDANCE Cross-Layer HID & UI Parity

All DAC settings live across three runtimes connected by a single 64-byte raw USB HID contract (`WEBHID_MAGIC_16 = 0x4551`). Never update one layer without checking parity across the others.

## Steps

1. **Inspect Protocol Definitions First**
   - Firmware protocol structs & command IDs: [main/usb/usb_hid.h](../../../main/usb/usb_hid.h) & [main/usb/usb_hid.c](../../../main/usb/usb_hid.c)
   - Firmware DSP & NVS v3 slots: [main/dsp/dsp_peq.h](../../../main/dsp/dsp_peq.h) & [main/dsp/dsp_core.c](../../../main/dsp/dsp_core.c)
   - WebHID React codec & constants: [tools/webhid_tuner/src/hid/protocol.js](../../../tools/webhid_tuner/src/hid/protocol.js)
   - Android Kotlin codec & models: [PeqPacketCodec.kt](../../../tools/android_companion/app/src/main/java/com/ascendance/dac/usb/PeqPacketCodec.kt) & [DspModels.kt](../../../tools/android_companion/app/src/main/java/com/ascendance/dac/model/DspModels.kt)

2. **Verify Command, Byte-Offset & Clamp Bounds**
   - `0x01`: Get active band / metadata (`reserved[0]` = `num_bands`), `0xFD` = extended dynamics + 16-byte UTF-8 slot name + harmonics mirror (`reserved[20..27]`) + DSP Pro (`reserved[28..31]`), `0xFE` = `2×2` channel matrix floats.
   - `0x02`..`0x09`, `0x13`: Live PEQ band (`20..20000 Hz`, `-24.0..+12.0 dB`, `0.10..10.00 Q`), Preamp (`-12.0..0.0 dB`), Slot (`0..7`), Save (`0x05`), Reset (`0x06`), Band Count (`1..10`), A/B (`0x08`), Dual-Mono (`0x09`), Mixer (`0x13`).
   - `0x0A`..`0x0F`, `0x14`: Rename slot (`16` UTF-8 bytes), Set Dynamics (`view`, `loudness`, `limiter | (fir << 1) | (auto_preamp << 2)`), Set Power (`auto_mute_sec`, `oled_dim_sec`), Factory Reset slot (`0x0D`), Set Harmonics (`0x0E`: flags, bass `0..100%`, exciter `0..100%`, bass `50..150 Hz`, exciter `1500..8000 Hz`), Set DSP Pro (`0x0F`: flags, dynamic bass amount, expanded OLED view `0..4`), Chunked FIR float32 upload (`0x14`, `12` floats/packet, `256` taps max @ `44.1 / 48 kHz`).

3. **Enforce User-Friendly UI Copy & OLED Fit**
   - Use accessible audiophile terms (`ASCENDANCE Hi-Fi` / `ASCENDANCE Hi-Fi DAC`, `Slot 1..8`, `Preview Live`, `Save to DAC`, `Bass Enhancer`, `Dynamic Bass`, `Harmonic Exciter`, `FIR Impulse Filter`, `Equal Loudness (ISO 226)`, `OLED Screen View`, `Auto-Mute on Silence`, `OLED Auto-Dim & Sleep`) in user-facing UI strings across OLED ([main/ui/ui_oled.c](../../../main/ui/ui_oled.c)), WebHID (`src/components/*.jsx`), and Android (`ui/screens/*.kt`).
   - Check SSD1306 128×64 menu/hint strings do not exceed 20 characters at `x = 2` (`len * 6 + 2 <= 128`).

4. **Update Unit Tests & Protocol Docs Together**
   - Add or update test vectors in [webhidTuner.test.js](../../../tools/webhid_tuner/tests/webhidTuner.test.js) and [PeqPacketCodecTest.kt](../../../tools/android_companion/app/src/test/java/com/ascendance/dac/PeqPacketCodecTest.kt).
   - Sync documentation in [docs/guides/usb_dac_guide.md](../../../docs/guides/usb_dac_guide.md), [docs/firmware/current_firmware_implementation.md](../../../docs/firmware/current_firmware_implementation.md), and [docs/firmware/software.md](../../../docs/firmware/software.md).
