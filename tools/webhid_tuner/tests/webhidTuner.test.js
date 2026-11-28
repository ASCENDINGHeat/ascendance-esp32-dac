import { describe, expect, it } from 'vitest';
import {
  createDefaultBands,
  evalCascadeAtFreq,
} from '../src/dsp/biquad.js';
import {
  CH_LINKED,
  CMD_GET_ACTIVE_PROFILE,
  CMD_SET_BAND_LIVE,
  CMD_SET_DYNAMICS,
  CMD_SET_HARMONICS,
  CMD_SET_DSP_PRO,
  WEBHID_MAGIC_16,
  decodeInputReport,
  encodeDynamicsConfig,
  encodeFirChunkPackets,
  encodeRenameSlot,
  encodeSetBandLive,
  encodeSetDspPro,
  encodeSetHarmonics,
  formatSlotOptionLabel,
} from '../src/hid/protocol.js';
import {
  computeRecommendedPreampDb,
  exportEqualizerApoText,
  parseEqualizerApoText,
  parseFirFileBuffer,
  suggestSlotNameFromSource,
} from '../src/io/fileParsers.js';

describe('WebHID Tuner DSP, Protocol & Parsers', () => {
  it('formats default and custom slot labels cleanly', () => {
    expect(formatSlotOptionLabel(0, 'Slot 1')).toBe('Slot 1');
    expect(formatSlotOptionLabel(1, '')).toBe('Slot 2');
    expect(formatSlotOptionLabel(0, 'HD600')).toBe('1: HD600');
    expect(formatSlotOptionLabel(2, ' Bass ')).toBe('3: Bass');
  });

  it('evaluates RBJ peaking boost accurately at center frequency', () => {
    const bands = [{ type: 1, freq: 1000, gain: 6.0, q: 1.41 }];
    const atCenter = evalCascadeAtFreq(bands, 1000, 48000);
    expect(atCenter.magDb).toBeCloseTo(6.0, 1);
  });

  it('round-trips Equalizer APO text import and export', () => {
    const raw = [
      'Preamp: -4.5 dB',
      'Filter 1: ON LSC Fc 80 Hz Gain 4.5 dB Q 0.71',
      'Filter 2: ON PK Fc 1000 Hz Gain -3.0 dB Q 1.50',
      'Filter 3: ON HPQ Fc 30 Hz Q 0.71',
    ].join('\n');

    const parsed = parseEqualizerApoText(raw, 0);
    expect(parsed.preamp).toBe(-4.5);
    expect(parsed.bands).toHaveLength(3);
    expect(parsed.bands[2].type).toBe(4);

    const exported = exportEqualizerApoText(parsed.bands, parsed.preamp);
    expect(exported).toContain('Preamp: -4.5 dB');
    expect(exported).toContain('Filter 1: ON LSC Fc 80 Hz Gain 4.5 dB Q 0.71');
    expect(exported).toContain('Filter 3: ON HPQ Fc 30 Hz Q 0.71');
  });

  it('encodes 64-byte live band, rename slot, and chunked FIR packets', () => {
    const bands = createDefaultBands(5);
    const pkt = encodeSetBandLive(2, CH_LINKED, bands[2]);
    const dv = new DataView(pkt.buffer);
    expect(pkt).toHaveLength(64);
    expect(dv.getUint16(0, true)).toBe(WEBHID_MAGIC_16);
    expect(dv.getUint8(2)).toBe(CMD_SET_BAND_LIVE);
    expect(dv.getUint8(3)).toBe(2);
    expect(dv.getUint16(6, true)).toBe(1000);

    const rename = encodeRenameSlot(3, 'HD650 Warm');
    const decodedName = new TextDecoder().decode(rename.subarray(12, 22));
    expect(decodedName).toBe('HD650 Warm');

    const firPkts = encodeFirChunkPackets(
      1,
      Array.from({ length: 14 }, (_, i) => i * 0.1),
      true
    );
    expect(firPkts).toHaveLength(2);
    expect(firPkts[0][4]).toBe(12);
    expect(firPkts[1][4]).toBe(2);
    expect(firPkts[1][5]).toBe(1);
  });

  it('encodes CMD_SET_HARMONICS (0x0E) matching the golden vector', () => {
    const pkt = encodeSetHarmonics({
      bassEnabled: true,
      exciterEnabled: true,
      exciterMode: 1,
      bassAmount: 60,
      exciterAmount: 35,
      bassFreq: 90,
      exciterFreq: 4500,
    });

    expect(pkt).toHaveLength(64);
    expect(CMD_SET_HARMONICS).toBe(0x0e);
    expect(Array.from(pkt.slice(0, 12))).toEqual([
      0x51, 0x45, 0x0e, 0x07, 0x3c, 0x23, 0x5a, 0x00, 0x00, 0x00, 0x94, 0x11,
    ]);
    expect(Array.from(pkt.slice(12)).every((b) => b === 0)).toBe(true);
  });

  it('clamps out-of-range inputs in encodeSetHarmonics()', () => {
    const lowPkt = encodeSetHarmonics({
      bassEnabled: false,
      exciterEnabled: true,
      exciterMode: -5,
      bassAmount: -10,
      exciterAmount: -1,
      bassFreq: 20,
      exciterFreq: 500,
    });
    const lowDv = new DataView(lowPkt.buffer);
    expect(lowDv.getUint8(2)).toBe(CMD_SET_HARMONICS);
    expect(lowDv.getUint8(3)).toBe(0x02);
    expect(lowDv.getUint8(4)).toBe(0);
    expect(lowDv.getUint8(5)).toBe(0);
    expect(lowDv.getUint16(6, true)).toBe(50);
    expect(lowDv.getUint16(10, true)).toBe(1500);

    const highPkt = encodeSetHarmonics({
      bassEnabled: true,
      exciterEnabled: false,
      exciterMode: 7,
      bassAmount: 180,
      exciterAmount: 255,
      bassFreq: 400,
      exciterFreq: 16000,
    });
    const highDv = new DataView(highPkt.buffer);
    expect(highDv.getUint8(3)).toBe(0x01 | (2 << 2));
    expect(highDv.getUint8(4)).toBe(100);
    expect(highDv.getUint8(5)).toBe(100);
    expect(highDv.getUint16(6, true)).toBe(150);
    expect(highDv.getUint16(10, true)).toBe(8000);
  });

  it('encodes CMD_SET_DYNAMICS (0x0B) with autoPreampEnabled bit 0x04', () => {
    const pkt = encodeDynamicsConfig({
      limiterEnabled: true,
      firEnabled: true,
      autoPreampEnabled: true,
      loudnessMode: 2,
      oledView: 3,
    });
    const dv = new DataView(pkt.buffer);
    expect(pkt).toHaveLength(64);
    expect(dv.getUint16(0, true)).toBe(WEBHID_MAGIC_16);
    expect(dv.getUint8(2)).toBe(CMD_SET_DYNAMICS);
    expect(dv.getUint8(3)).toBe(3);
    expect(dv.getUint8(4)).toBe(2);
    expect(dv.getUint8(5)).toBe(0x07);
  });

  it('decodes extended dynamics (0xFD) including harmonics reserved[20..26], autoPreamp byte 39, and profile (0x01)', () => {
    const buf = new ArrayBuffer(64);
    const dv = new DataView(buf);
    dv.setUint16(0, WEBHID_MAGIC_16, true);
    dv.setUint8(2, CMD_SET_DYNAMICS);
    dv.setUint8(3, 0xfd);
    dv.setUint8(4, 2);
    dv.setUint8(5, 0x01 | (2 << 1) | 0x08 | (3 << 4) | 0x40);
    dv.setUint16(6, 30, true);
    dv.setUint16(8, 120, true);
    dv.setUint16(10, 256, true);
    new Uint8Array(buf, 12, 16).set(new TextEncoder().encode('Planar Eq'));
    dv.setUint16(28, 128, true);
    dv.setUint16(30, 256, true);

    const encodedHarm = encodeSetHarmonics({
      bassEnabled: true,
      exciterEnabled: true,
      exciterMode: 2,
      bassAmount: 75,
      exciterAmount: 42,
      bassFreq: 110,
      exciterFreq: 6200,
    });
    const harmView = new DataView(encodedHarm.buffer);
    dv.setUint8(32, harmView.getUint8(3));
    dv.setUint8(33, harmView.getUint8(4));
    dv.setUint8(34, harmView.getUint8(5));
    dv.setUint16(35, harmView.getUint16(6, true), true);
    dv.setUint16(37, harmView.getUint16(10, true), true);
    dv.setUint8(39, 42);

    const ext = decodeInputReport(dv);
    expect(ext.kind).toBe('extState');
    expect(ext.slotIdx).toBe(2);
    expect(ext.slotName).toBe('Planar Eq');
    expect(ext.dynamics.loudnessMode).toBe(2);
    expect(ext.dynamics.oledView).toBe(3);
    expect(ext.dynamics.autoPreampEnabled).toBe(true);
    expect(ext.dynamics.autoPreampDb).toBeCloseTo(-4.2, 2);
    expect(ext.dynamics.firTapsL).toBe(256);
    expect(ext.harmonics).toEqual({
      bassEnabled: true,
      bassAmount: 75,
      bassFreq: 110,
      exciterEnabled: true,
      exciterMode: 2,
      exciterAmount: 42,
      exciterFreq: 6200,
    });

    const pBuf = new ArrayBuffer(64);
    const pdv = new DataView(pBuf);
    pdv.setUint16(0, WEBHID_MAGIC_16, true);
    pdv.setUint8(2, CMD_GET_ACTIVE_PROFILE);
    pdv.setUint8(3, 1);
    pdv.setUint8(4, 1);
    pdv.setUint16(6, 2000, true);
    pdv.setInt16(8, -25, true);
    pdv.setUint16(10, 141, true);
    pdv.setUint8(12, 5);
    pdv.setUint32(22, 96000, true);

    const prof = decodeInputReport(pdv);
    expect(prof.kind).toBe('profile');
    expect(prof.sampleRateHz).toBe(96000);
    expect(prof.band.gain).toBeCloseTo(-2.5, 2);
  });

  it('parses FIR .txt and PCM16 .wav impulse buffers', () => {
    const txtBuf = new TextEncoder().encode('1.0, -0.5\n0.25').buffer;
    expect(parseFirFileBuffer('room.txt', txtBuf)).toEqual([1.0, -0.5, 0.25]);

    const wavBuf = new ArrayBuffer(44 + 4);
    const dv = new DataView(wavBuf);
    [0x52, 0x49, 0x46, 0x46].forEach((b, i) => dv.setUint8(i, b));
    dv.setUint32(4, 36 + 4, true);
    [0x57, 0x41, 0x56, 0x45, 0x66, 0x6d, 0x74, 0x20].forEach((b, i) => dv.setUint8(8 + i, b));
    dv.setUint32(16, 16, true);
    dv.setUint16(20, 1, true);
    dv.setUint16(22, 1, true);
    dv.setUint16(34, 16, true);
    [0x64, 0x61, 0x74, 0x61].forEach((b, i) => dv.setUint8(36 + i, b));
    dv.setUint32(40, 4, true);
    dv.setInt16(44, 16384, true);
    dv.setInt16(46, -16384, true);

    const wavTaps = parseFirFileBuffer('impulse.wav', wavBuf);
    expect(wavTaps[0]).toBeCloseTo(0.5, 4);
    expect(wavTaps[1]).toBeCloseTo(-0.5, 4);
  });

  it('encodes CMD_SET_DSP_PRO (0x0F) and decodes 0xFD[40..43] with v0.4.x backward compat', () => {
    const pkt = encodeSetDspPro(
      {
        tpdfDitherEnabled: true,
        subsonicFilterEnabled: true,
        volTaperLogEnabled: true,
        dynamicBassEnabled: true,
        dynamicBassAmount: 65,
        autoPreampEnabled: true,
      },
      4
    );
    expect(pkt).toHaveLength(64);
    expect(CMD_SET_DSP_PRO).toBe(0x0f);
    expect(Array.from(pkt.slice(0, 6))).toEqual([0x51, 0x45, 0x0f, 0x1f, 65, 4]);

    const buf = new ArrayBuffer(64);
    const dv = new DataView(buf);
    dv.setUint16(0, WEBHID_MAGIC_16, true);
    dv.setUint8(2, CMD_SET_DYNAMICS);
    dv.setUint8(3, 0xfd);
    dv.setUint8(4, 1);
    dv.setUint8(5, 0x41);
    dv.setUint8(40, 1);
    dv.setUint8(41, 0x1d); // tpdf(1) | log(4) | dynBass(8) | autoPreamp(16)
    dv.setUint8(42, 72);
    dv.setUint8(43, 4);

    const ext = decodeInputReport(dv);
    expect(ext.dynamics.oledView).toBe(4);
    expect(ext.dspPro).toEqual({
      tpdfDitherEnabled: true,
      subsonicFilterEnabled: false,
      volTaperLogEnabled: true,
      dynamicBassEnabled: true,
      dynamicBassAmount: 72,
      autoPreampEnabled: true,
    });
  });

  it('suggests 16-char AutoEQ slot names and computes Auto-Preamp headroom', () => {
    expect(suggestSlotNameFromSource('Sennheiser HD 600 ParametricEQ.txt')).toBe('Sennheiser HD 60');
    expect(suggestSlotNameFromSource('Moondrop_Aria_AutoEQ.txt')).toBe('Moondrop Aria');
    expect(suggestSlotNameFromSource('', '# Sony IER-M9\nFilter 1: ON PK Fc 100 Hz Gain 5.0 dB Q 1.0')).toBe('Sony IER-M9');

    const parsed = parseEqualizerApoText('Filter 1: ON PK Fc 100 Hz Gain 5.5 dB Q 1.0\n');
    expect(parsed.hasExplicitPreamp).toBe(false);
    expect(computeRecommendedPreampDb(parsed.bands, 48000)).toBeLessThanOrEqual(-5.4);
    expect(computeRecommendedPreampDb(parsed.bands, 48000)).toBeGreaterThanOrEqual(-6.0);
  });
});
