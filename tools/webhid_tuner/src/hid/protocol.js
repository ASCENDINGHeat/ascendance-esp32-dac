import { clamp } from '../dsp/biquad.js';

export const WEBHID_MAGIC_16 = 0x4551; // "EQ"
export const CMD_GET_ACTIVE_PROFILE = 0x01;
export const CMD_SET_BAND_LIVE = 0x02;
export const CMD_SET_PREAMP_LIVE = 0x03;
export const CMD_SELECT_PROFILE = 0x04;
export const CMD_SAVE_TO_PRESET = 0x05;
export const CMD_RESET_TO_PRESET = 0x06;
export const CMD_SET_BAND_COUNT = 0x07;
export const CMD_SET_AB_BYPASS = 0x08;
export const CMD_SET_DUAL_MONO = 0x09;
export const CMD_RENAME_SLOT = 0x0a;
export const CMD_SET_DYNAMICS = 0x0b;
export const CMD_SET_POWER_CFG = 0x0c;
export const CMD_FACTORY_RESET_SLOT = 0x0d;
export const CMD_SET_HARMONICS = 0x0e;
export const CMD_SET_DSP_PRO = 0x0f;
export const CMD_SET_MIXER = 0x13;
export const CMD_UPLOAD_FIR_CHUNK = 0x14;

export const SUB_EXT_STATE = 0xfd;
export const SUB_MIXER = 0xfe;

export const CH_LINKED = 0;
export const CH_LEFT = 1;
export const CH_RIGHT = 2;

export const EXCITER_MODES = ['Tube Warmth', 'Tape', 'Air Sheen'];

export const DEFAULT_MIXER = {
  mode: 0,
  balance: 0,
  widthPct: 100,
  crossfeedLevel: 0,
  customMatrix: [1.0, 0.0, 0.0, 1.0],
};

export const DEFAULT_DYNAMICS = {
  limiterEnabled: true,
  autoPreampEnabled: false,
  autoPreampDb: 0.0,
  loudnessMode: 0,
  firEnabled: false,
  oledView: 0,
  autoMuteSec: 10,
  oledDimSec: 60,
  firTapsL: 0,
  firTapsR: 0,
  firMaxTaps: 256,
};

export const DEFAULT_HARMONICS = {
  bassEnabled: false,
  bassAmount: 40,
  bassFreq: 90,
  exciterEnabled: false,
  exciterMode: 0,
  exciterAmount: 30,
  exciterFreq: 4500,
};

export const DEFAULT_DSP_PRO = {
  tpdfDitherEnabled: true,
  subsonicFilterEnabled: false,
  volTaperLogEnabled: false,
  dynamicBassEnabled: false,
  dynamicBassAmount: 40,
  autoPreampEnabled: false,
};

export function defaultSlotNames() {
  return Array.from({ length: 8 }, (_, i) => `Slot ${i + 1}`);
}

export function formatSlotOptionLabel(i, rawName) {
  const defName = `Slot ${i + 1}`;
  const name = (rawName || '').trim() || defName;
  return name === defName ? name : `${i + 1}: ${name}`;
}

function makeReport() {
  const buffer = new ArrayBuffer(64);
  const view = new DataView(buffer);
  view.setUint16(0, WEBHID_MAGIC_16, true);
  return { buffer, view };
}

export function encodeGetBand(bandIdx, ch) {
  const { buffer, view } = makeReport();
  view.setUint8(2, CMD_GET_ACTIVE_PROFILE);
  view.setUint8(3, bandIdx);
  view.setUint8(13, ch);
  return new Uint8Array(buffer);
}

export function encodeSetBandCount(count, ch) {
  const { buffer, view } = makeReport();
  view.setUint8(2, CMD_SET_BAND_COUNT);
  view.setUint8(3, count);
  view.setUint8(13, ch);
  return new Uint8Array(buffer);
}

export function encodeSetBandLive(bandIdx, ch, band) {
  const { buffer, view } = makeReport();
  view.setUint8(2, CMD_SET_BAND_LIVE);
  view.setUint8(3, bandIdx);
  view.setUint8(4, band.type);
  view.setUint8(5, band.type !== 0 ? 1 : 0);
  view.setUint16(6, Math.round(band.freq), true);
  view.setInt16(8, Math.round(band.gain * 10), true);
  view.setUint16(10, Math.round(band.q * 100), true);
  view.setUint8(13, ch);
  return new Uint8Array(buffer);
}

export function encodePreampLive(gainDb) {
  const { buffer, view } = makeReport();
  view.setUint8(2, CMD_SET_PREAMP_LIVE);
  view.setInt16(8, Math.round(gainDb * 10), true);
  return new Uint8Array(buffer);
}

export function encodeSelectProfile(slot) {
  const { buffer, view } = makeReport();
  view.setUint8(2, CMD_SELECT_PROFILE);
  view.setUint8(3, clamp(slot, 0, 7));
  return new Uint8Array(buffer);
}

export function encodeSaveToPreset() {
  const { buffer, view } = makeReport();
  view.setUint8(2, CMD_SAVE_TO_PRESET);
  return new Uint8Array(buffer);
}

export function encodeAbBypass(bypass) {
  const { buffer, view } = makeReport();
  view.setUint8(2, CMD_SET_AB_BYPASS);
  view.setUint8(3, bypass ? 1 : 0);
  return new Uint8Array(buffer);
}

export function encodeDualMonoMode(linked) {
  const { buffer, view } = makeReport();
  view.setUint8(2, CMD_SET_DUAL_MONO);
  view.setUint8(3, linked ? 1 : 0);
  return new Uint8Array(buffer);
}

export function encodeMixerConfig(mixer) {
  const { buffer, view } = makeReport();
  view.setUint8(2, CMD_SET_MIXER);
  view.setUint8(3, mixer.mode);
  view.setInt8(4, mixer.balance);
  view.setUint8(5, clamp(mixer.widthPct, 0, 200));
  view.setUint16(6, clamp(mixer.crossfeedLevel, 0, 3), true);
  for (let i = 0; i < 4; i++) {
    view.setFloat32(12 + i * 4, mixer.customMatrix[i], true);
  }
  return new Uint8Array(buffer);
}

export function encodeRenameSlot(slot, nameStr) {
  const { buffer, view } = makeReport();
  view.setUint8(2, CMD_RENAME_SLOT);
  view.setUint8(3, clamp(slot, 0, 7));
  const bytes = new TextEncoder().encode(String(nameStr || '').slice(0, 16));
  new Uint8Array(buffer, 12, 16).set(bytes.subarray(0, 16));
  return new Uint8Array(buffer);
}

export function encodeDynamicsConfig(dynamics) {
  const { buffer, view } = makeReport();
  view.setUint8(2, CMD_SET_DYNAMICS);
  view.setUint8(3, clamp(dynamics.oledView, 0, 4));
  view.setUint8(4, clamp(dynamics.loudnessMode, 0, 2));
  const flags =
    (dynamics.limiterEnabled ? 0x01 : 0x00) |
    (dynamics.firEnabled ? 0x02 : 0x00) |
    (dynamics.autoPreampEnabled ? 0x04 : 0x00);
  view.setUint8(5, flags);
  return new Uint8Array(buffer);
}

export function encodePowerConfig(autoMuteSec, oledDimSec) {
  const { buffer, view } = makeReport();
  view.setUint8(2, CMD_SET_POWER_CFG);
  view.setUint16(6, clamp(autoMuteSec, 0, 3600), true);
  view.setUint16(10, clamp(oledDimSec, 0, 3600), true);
  return new Uint8Array(buffer);
}

export function encodeFactoryResetSlot(slot) {
  const { buffer, view } = makeReport();
  view.setUint8(2, CMD_FACTORY_RESET_SLOT);
  view.setUint8(3, clamp(slot, 0, 7));
  return new Uint8Array(buffer);
}

export function encodeFirChunkPackets(ch, floatArray, commitNvs = true) {
  const packets = [];
  const total = floatArray.length;
  if (total === 0) {
    const { buffer, view } = makeReport();
    view.setUint8(2, CMD_UPLOAD_FIR_CHUNK);
    view.setUint8(3, ch);
    view.setUint8(4, 0);
    view.setUint8(5, commitNvs ? 1 : 0);
    view.setUint16(6, 0, true);
    view.setUint16(10, 0, true);
    packets.push(new Uint8Array(buffer));
    return packets;
  }
  for (let offset = 0; offset < total; offset += 12) {
    const count = Math.min(12, total - offset);
    const isLast = offset + count >= total;
    const { buffer, view } = makeReport();
    view.setUint8(2, CMD_UPLOAD_FIR_CHUNK);
    view.setUint8(3, ch);
    view.setUint8(4, count);
    view.setUint8(5, commitNvs && isLast ? 1 : 0);
    view.setUint16(6, total, true);
    view.setUint16(10, offset, true);
    for (let k = 0; k < count; k++) {
      view.setFloat32(12 + k * 4, floatArray[offset + k], true);
    }
    packets.push(new Uint8Array(buffer));
  }
  return packets;
}

export function clampHarmonicsConfig(cfg = {}) {
  const merged = { ...DEFAULT_HARMONICS, ...(cfg || {}) };
  const exciterModeRaw = Number(merged.exciterMode);
  const bassAmountRaw = Number(merged.bassAmount);
  const exciterAmountRaw = Number(merged.exciterAmount);
  const bassFreqRaw = Number(merged.bassFreq);
  const exciterFreqRaw = Number(merged.exciterFreq);
  return {
    bassEnabled: Boolean(merged.bassEnabled),
    bassAmount: clamp(Math.round(Number.isFinite(bassAmountRaw) ? bassAmountRaw : 0), 0, 100),
    bassFreq: clamp(Math.round(Number.isFinite(bassFreqRaw) ? bassFreqRaw : 50), 50, 150),
    exciterEnabled: Boolean(merged.exciterEnabled),
    exciterMode: clamp(Math.round(Number.isFinite(exciterModeRaw) ? exciterModeRaw : 0), 0, 2),
    exciterAmount: clamp(Math.round(Number.isFinite(exciterAmountRaw) ? exciterAmountRaw : 0), 0, 100),
    exciterFreq: clamp(Math.round(Number.isFinite(exciterFreqRaw) ? exciterFreqRaw : 1500), 1500, 8000),
  };
}

export function encodeSetHarmonics(cfg) {
  const safe = clampHarmonicsConfig(cfg);
  const { buffer, view } = makeReport();
  view.setUint8(2, CMD_SET_HARMONICS);
  const flags =
    (safe.bassEnabled ? 0x01 : 0x00) |
    (safe.exciterEnabled ? 0x02 : 0x00) |
    ((safe.exciterMode & 0x03) << 2);
  view.setUint8(3, flags);
  view.setUint8(4, safe.bassAmount);
  view.setUint8(5, safe.exciterAmount);
  view.setUint16(6, safe.bassFreq, true);
  view.setUint16(10, safe.exciterFreq, true);
  return new Uint8Array(buffer);
}

export function encodeSetDspPro(cfg = {}, oledViewOverride = 0xff) {
  const merged = { ...DEFAULT_DSP_PRO, ...(cfg || {}) };
  const { buffer, view } = makeReport();
  view.setUint8(2, CMD_SET_DSP_PRO);
  const flags =
    (merged.tpdfDitherEnabled ? 0x01 : 0x00) |
    (merged.subsonicFilterEnabled ? 0x02 : 0x00) |
    (merged.volTaperLogEnabled ? 0x04 : 0x00) |
    (merged.dynamicBassEnabled ? 0x08 : 0x00) |
    (merged.autoPreampEnabled ? 0x10 : 0x00);
  view.setUint8(3, flags);
  view.setUint8(4, Math.round(clamp(Number(merged.dynamicBassAmount) || 0, 0, 100)));
  const viewByte = oledViewOverride === 0xff ? 0xff : clamp(Number(oledViewOverride) || 0, 0, 4);
  view.setUint8(5, viewByte);
  return new Uint8Array(buffer);
}

export function decodeInputReport(dataView) {
  if (!dataView || dataView.byteLength < 26) return null;
  if (dataView.getUint16(0, true) !== WEBHID_MAGIC_16) return null;

  const cmd = dataView.getUint8(2);
  if (cmd === CMD_SET_MIXER) {
    const customMatrix = [];
    for (let i = 0; i < 4; i++) {
      customMatrix.push(dataView.getFloat32(12 + i * 4, true));
    }
    return {
      kind: 'mixer',
      mixer: {
        mode: dataView.getUint8(3),
        balance: dataView.getInt8(4),
        widthPct: dataView.getUint8(5),
        crossfeedLevel: dataView.getUint16(6, true),
        customMatrix,
      },
    };
  }

  if (cmd === CMD_SET_DYNAMICS && dataView.byteLength >= 32) {
    const slotIdx = dataView.getUint8(4);
    const dynFlags = dataView.getUint8(5);
    const nameBytes = new Uint8Array(dataView.buffer, dataView.byteOffset + 12, 16);
    const nul = nameBytes.indexOf(0);
    const slotName = new TextDecoder().decode(nul >= 0 ? nameBytes.subarray(0, nul) : nameBytes);

    let harmonics = { ...DEFAULT_HARMONICS };
    if (dataView.byteLength >= 40) {
      const harmFlags = dataView.getUint8(32);
      harmonics = {
        bassEnabled: (harmFlags & 0x01) !== 0,
        bassAmount: clamp(dataView.getUint8(33), 0, 100),
        bassFreq: clamp(dataView.getUint16(35, true), 50, 150),
        exciterEnabled: (harmFlags & 0x02) !== 0,
        exciterMode: clamp((harmFlags >> 2) & 0x03, 0, 2),
        exciterAmount: clamp(dataView.getUint8(34), 0, 100),
        exciterFreq: clamp(dataView.getUint16(37, true), 1500, 8000),
      };
    }

    const autoPreampDb =
      dataView.byteLength >= 40 ? -(clamp(dataView.getUint8(39), 0, 120) / 10.0) : 0.0;

    let oledView = (dynFlags >> 4) & 0x03;
    let dspPro = { ...DEFAULT_DSP_PRO, autoPreampEnabled: (dynFlags & 0x40) !== 0 };
    if (dataView.byteLength >= 44 && dataView.getUint8(40) >= 1) {
      const proFlags = dataView.getUint8(41);
      dspPro = {
        tpdfDitherEnabled: (proFlags & 0x01) !== 0,
        subsonicFilterEnabled: (proFlags & 0x02) !== 0,
        volTaperLogEnabled: (proFlags & 0x04) !== 0,
        dynamicBassEnabled: (proFlags & 0x08) !== 0,
        dynamicBassAmount: clamp(dataView.getUint8(42), 0, 100),
        autoPreampEnabled: (proFlags & 0x10) !== 0,
      };
      oledView = clamp(dataView.getUint8(43), 0, 4);
    }

    return {
      kind: 'extState',
      slotIdx,
      slotName,
      dynamics: {
        limiterEnabled: (dynFlags & 0x01) !== 0,
        autoPreampEnabled: (dynFlags & 0x40) !== 0,
        autoPreampDb,
        loudnessMode: (dynFlags >> 1) & 0x03,
        firEnabled: (dynFlags & 0x08) !== 0,
        oledView,
        autoMuteSec: dataView.getUint16(6, true),
        oledDimSec: dataView.getUint16(8, true),
        firTapsL: dataView.getUint16(10, true),
        firTapsR: dataView.getUint16(28, true),
        firMaxTaps: dataView.getUint16(30, true),
      },
      harmonics,
      dspPro,
    };
  }

  if (cmd !== CMD_GET_ACTIVE_PROFILE) return null;

  const flags = dataView.getUint8(14);
  return {
    kind: 'profile',
    bandIdx: dataView.getUint8(3),
    band: {
      type: dataView.getUint8(4),
      freq: clamp(dataView.getUint16(6, true), 20, 20000),
      gain: clamp(dataView.getInt16(8, true) / 10.0, -24.0, 12.0),
      q: clamp(dataView.getUint16(10, true) / 100.0, 0.1, 10.0),
    },
    numBands: dataView.getUint8(12),
    channel: dataView.getUint8(13),
    linkedStereo: (flags & 0x01) !== 0,
    abBypass: (flags & 0x02) !== 0,
    activeSlot: dataView.getUint8(15),
    preampDb: dataView.getInt16(16, true) / 10.0,
    mixerPartial: {
      mode: dataView.getUint8(18),
      balance: dataView.getInt8(19),
      widthPct: dataView.getUint8(20),
      crossfeedLevel: dataView.getUint8(21),
    },
    sampleRateHz: dataView.getUint32(22, true),
  };
}
