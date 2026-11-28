import { clamp, evalCascadeAtFreq, filterUsesGain } from '../dsp/biquad.js';

export function suggestSlotNameFromSource(fileName = '', text = '') {
  let raw = (fileName || '').trim();
  if (raw) {
    raw = raw
      .replace(/^.*[\\/]/, '')
      .replace(/\.(txt|json|csv)$/i, '')
      .replace(/[_-]+/g, ' ')
      .replace(/\b(ParametricEQ|FixedBandEQ|GraphicEQ|AutoEQ|oratory1990|crinacle|rtings|innerfidelity|headphonecom)\b/gi, ' ')
      .replace(/\s+/g, ' ')
      .trim();
  }
  if (!raw && text) {
    const firstComment = text
      .split(/\r?\n/)
      .map((l) => l.trim())
      .find((l) => l.startsWith('#') && l.replace(/^#+\s*/, '').length >= 2);
    if (firstComment) {
      raw = firstComment
        .replace(/^#+\s*/, '')
        .replace(/\b(ParametricEQ|FixedBandEQ|AutoEQ|EQ)\b/gi, ' ')
        .replace(/\s+/g, ' ')
        .trim();
    }
  }
  if (!raw) return '';
  return raw.slice(0, 16).trim();
}

export function computeRecommendedPreampDb(bands, sampleRate = 48000) {
  if (!Array.isArray(bands) || bands.length === 0) return 0.0;
  let maxBoostDb = 0.0;
  const steps = 64;
  const fMin = 20.0;
  const fMax = Math.min(20000.0, sampleRate * 0.45);
  const ratio = Math.pow(fMax / fMin, 1.0 / (steps - 1));
  let freq = fMin;
  for (let i = 0; i < steps; i++) {
    const { magDb } = evalCascadeAtFreq(bands, freq, sampleRate);
    if (Number.isFinite(magDb) && magDb > maxBoostDb) {
      maxBoostDb = magDb;
    }
    freq *= ratio;
  }
  if (maxBoostDb <= 0.05) return 0.0;
  const needed = -Math.ceil(maxBoostDb * 10) / 10;
  return clamp(Number(needed.toFixed(1)), -12.0, 0.0);
}

export function parseEqualizerApoText(text, fallbackPreamp = 0.0) {
  const lines = text.split(/\r?\n/);
  let parsedPreamp = fallbackPreamp;
  let hasExplicitPreamp = false;
  const parsedBands = [];
  const typeMap = {
    PK: 1,
    PEQ: 1,
    LS: 2,
    LSC: 2,
    HS: 3,
    HSC: 3,
    HP: 4,
    HPQ: 4,
    LP: 5,
    LPQ: 5,
    NO: 6,
    NOTCH: 6,
    BP: 7,
  };

  for (const rawLine of lines) {
    const line = rawLine.trim();
    if (!line || line.startsWith('#')) continue;

    const preampMatch = line.match(/^Preamp:\s*([+-]?\d+(?:\.\d+)?)\s*dB/i);
    if (preampMatch) {
      parsedPreamp = clamp(parseFloat(preampMatch[1]), -12.0, 0.0);
      hasExplicitPreamp = true;
      continue;
    }

    const filterMatch = line.match(
      /^Filter\s*\d*\s*:\s*(ON|OFF)\s+([A-Z]+)\s+Fc\s+([0-9.]+)\s*Hz(?:\s+Gain\s+([+-]?[0-9.]+)\s*dB)?(?:\s+Q\s+([0-9.]+))?/i
    );
    if (filterMatch && parsedBands.length < 10) {
      const isOn = filterMatch[1].toUpperCase() === 'ON';
      const code = filterMatch[2].toUpperCase();
      const mappedType = isOn ? typeMap[code] || 1 : 0;
      const freq = clamp(Math.round(parseFloat(filterMatch[3])), 20, 20000);
      const gain = filterMatch[4] !== undefined ? clamp(parseFloat(filterMatch[4]), -24.0, 12.0) : 0.0;
      const q = filterMatch[5] !== undefined ? clamp(parseFloat(filterMatch[5]), 0.1, 10.0) : 0.71;
      parsedBands.push({ type: mappedType, freq, gain, q });
    }
  }
  return { preamp: parsedPreamp, hasExplicitPreamp, bands: parsedBands };
}

export function exportEqualizerApoText(bands, preampDb) {
  const codeMap = { 0: 'PK', 1: 'PK', 2: 'LSC', 3: 'HSC', 4: 'HPQ', 5: 'LPQ', 6: 'NO', 7: 'BP' };
  const lines = [`Preamp: ${preampDb.toFixed(1)} dB`];
  bands.forEach((b, idx) => {
    const state = b.type === 0 ? 'OFF' : 'ON';
    const code = codeMap[b.type] || 'PK';
    if (filterUsesGain(b.type) || b.type === 0) {
      lines.push(
        `Filter ${idx + 1}: ${state} ${code} Fc ${Math.round(b.freq)} Hz Gain ${b.gain.toFixed(1)} dB Q ${b.q.toFixed(2)}`
      );
    } else {
      lines.push(`Filter ${idx + 1}: ${state} ${code} Fc ${Math.round(b.freq)} Hz Q ${b.q.toFixed(2)}`);
    }
  });
  return lines.join('\n') + '\n';
}

export function sanitizeImportedBands(rawBands) {
  if (!Array.isArray(rawBands) || rawBands.length === 0) return null;
  return rawBands.slice(0, 10).map((b) => ({
    type: clamp(parseInt(b.type, 10) || 0, 0, 7),
    freq: clamp(Math.round(b.freq || 1000), 20, 20000),
    gain: clamp(Number(b.gain || 0), -24, 12),
    q: clamp(Number(b.q || 0.71), 0.1, 10),
  }));
}

export function parseFirFileBuffer(fileName, arrayBuf) {
  const lower = fileName.toLowerCase();
  if (lower.endsWith('.wav') && arrayBuf.byteLength >= 44) {
    const dv = new DataView(arrayBuf);
    const riff = String.fromCharCode(dv.getUint8(0), dv.getUint8(1), dv.getUint8(2), dv.getUint8(3));
    const wave = String.fromCharCode(dv.getUint8(8), dv.getUint8(9), dv.getUint8(10), dv.getUint8(11));
    if (riff === 'RIFF' && wave === 'WAVE') {
      let pos = 12;
      let audioFmt = 1;
      let channels = 1;
      let bits = 16;
      const taps = [];
      while (pos + 8 <= arrayBuf.byteLength) {
        const id = String.fromCharCode(
          dv.getUint8(pos),
          dv.getUint8(pos + 1),
          dv.getUint8(pos + 2),
          dv.getUint8(pos + 3)
        );
        const sz = dv.getUint32(pos + 4, true);
        const dataOff = pos + 8;
        if (id === 'fmt ' && sz >= 16) {
          audioFmt = dv.getUint16(dataOff, true);
          channels = dv.getUint16(dataOff + 2, true);
          bits = dv.getUint16(dataOff + 14, true);
        } else if (id === 'data') {
          const bytesPerSample = Math.max(1, bits >> 3);
          const frameBytes = bytesPerSample * Math.max(1, channels);
          const numFrames = Math.floor(Math.min(sz, arrayBuf.byteLength - dataOff) / frameBytes);
          for (let f = 0; f < numFrames; f++) {
            const off = dataOff + f * frameBytes;
            let val = 0.0;
            if (audioFmt === 3 && bits === 32) {
              val = dv.getFloat32(off, true);
            } else if (bits === 16) {
              val = dv.getInt16(off, true) / 32768.0;
            } else if (bits === 24) {
              const b0 = dv.getUint8(off);
              const b1 = dv.getUint8(off + 1);
              const b2 = dv.getInt8(off + 2);
              val = (b0 | (b1 << 8) | (b2 << 16)) / 8388608.0;
            } else if (bits === 32) {
              val = dv.getInt32(off, true) / 2147483648.0;
            }
            taps.push(val);
          }
          break;
        }
        pos = dataOff + sz + (sz & 1);
      }
      return taps;
    }
  }
  const text = new TextDecoder().decode(arrayBuf);
  return text
    .split(/[\s,;]+/)
    .map((s) => parseFloat(s.trim()))
    .filter((n) => Number.isFinite(n));
}

export function downloadTextFile(filename, content, mime) {
  const blob = new Blob([content], { type: mime });
  const url = URL.createObjectURL(blob);
  const a = document.createElement('a');
  a.href = url;
  a.download = filename;
  document.body.appendChild(a);
  a.click();
  document.body.removeChild(a);
  URL.revokeObjectURL(url);
}
