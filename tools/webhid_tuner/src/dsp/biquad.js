export const FILTER_NAMES = [
  'Bypass',
  'Peaking',
  'Low Shelf',
  'High Shelf',
  'High Pass',
  'Low Pass',
  'Notch',
  'Band Pass',
];

export const ISO_FREQS = {
  1: [1000],
  2: [150, 4000],
  3: [100, 1000, 10000],
  4: [100, 500, 2500, 10000],
  5: [80, 250, 1000, 4000, 12000],
  6: [80, 200, 800, 2500, 6300, 14000],
  7: [63, 160, 400, 1000, 2500, 6300, 16000],
  8: [40, 100, 250, 630, 1600, 4000, 10000, 16000],
  9: [31, 63, 160, 400, 1000, 2500, 5000, 10000, 16000],
  10: [31, 63, 125, 250, 500, 1000, 2000, 4000, 8000, 16000],
};

export const BAND_COLORS = [
  '#58a6ff',
  '#3fb950',
  '#d29922',
  '#f85149',
  '#bc8cff',
  '#39c5bb',
  '#f0883e',
  '#79c0ff',
  '#56d364',
  '#e3b341',
];

export const STARTER_CURVES = {
  flat: {
    preamp: 0.0,
    bands: [
      { type: 2, freq: 80, gain: 0.0, q: 0.71 },
      { type: 1, freq: 250, gain: 0.0, q: 1.0 },
      { type: 1, freq: 1000, gain: 0.0, q: 1.0 },
      { type: 1, freq: 4000, gain: 0.0, q: 1.0 },
      { type: 3, freq: 10000, gain: 0.0, q: 0.71 },
    ],
  },
  bass: {
    preamp: -2.5,
    bands: [
      { type: 2, freq: 80, gain: 4.5, q: 0.71 },
      { type: 1, freq: 250, gain: 1.5, q: 1.0 },
      { type: 1, freq: 1000, gain: 0.0, q: 1.0 },
      { type: 1, freq: 4000, gain: -1.0, q: 1.0 },
      { type: 3, freq: 10000, gain: 0.5, q: 0.71 },
    ],
  },
  harman: {
    preamp: -4.0,
    bands: [
      { type: 2, freq: 105, gain: 5.0, q: 0.71 },
      { type: 1, freq: 200, gain: -1.5, q: 1.2 },
      { type: 1, freq: 1200, gain: 1.0, q: 1.4 },
      { type: 1, freq: 3000, gain: 2.5, q: 1.8 },
      { type: 3, freq: 8000, gain: -1.0, q: 0.71 },
    ],
  },
};

export function clamp(val, min, max) {
  if (Number.isNaN(val)) return min;
  return Math.max(min, Math.min(max, val));
}

export function filterUsesGain(type) {
  return type === 1 || type === 2 || type === 3;
}

export function cloneBands(bands) {
  return bands.map((b) => ({
    type: b.type,
    freq: b.freq,
    gain: b.gain,
    q: b.q,
  }));
}

export function createDefaultBands(count) {
  const freqs = ISO_FREQS[count] || ISO_FREQS[5];
  const bands = [];
  for (let i = 0; i < count; i++) {
    let type = 1;
    if (i === 0) type = 2;
    else if (i === count - 1) type = 3;
    bands.push({
      type,
      freq: freqs[i] || Math.round(100 * Math.pow(2, i)),
      gain: 0.0,
      q: type === 1 ? 1.0 : 0.71,
    });
  }
  return bands;
}

export function suggestNewBand(activeBands, freq, gain) {
  let newFreq = freq;
  const newGain = gain !== undefined ? gain : 0.0;
  if (newFreq === undefined) {
    if (activeBands.length === 0) {
      newFreq = 1000;
    } else {
      const sorted = [...activeBands].sort((a, b) => a.freq - b.freq);
      let maxRatio = 0;
      let bestIdx = 0;
      for (let i = 0; i < sorted.length - 1; i++) {
        const r = sorted[i + 1].freq / sorted[i].freq;
        if (r > maxRatio) {
          maxRatio = r;
          bestIdx = i;
        }
      }
      if (maxRatio > 1.35) {
        newFreq = Math.round(Math.sqrt(sorted[bestIdx].freq * sorted[bestIdx + 1].freq));
      } else if (sorted[sorted.length - 1].freq < 14000) {
        newFreq = Math.min(20000, Math.round(sorted[sorted.length - 1].freq * 1.4));
      } else if (sorted[0].freq > 30) {
        newFreq = Math.max(20, Math.round(sorted[0].freq / 1.4));
      } else {
        newFreq = 1000;
      }
    }
  }
  const filterType = activeBands.length === 0 ? 2 : 1;
  return {
    type: filterType,
    freq: clamp(Math.round(newFreq), 20, 20000),
    gain: clamp(Math.round(newGain * 10.0) / 10.0, -24.0, 12.0),
    q: 1.0,
  };
}

export function resizeBandList(bands, targetCount) {
  const count = clamp(targetCount, 1, 10);
  const next = cloneBands(bands);
  if (count < next.length) {
    next.length = count;
    return next;
  }
  const defaultList = ISO_FREQS[count];
  while (next.length < count) {
    const idx = next.length;
    const freq =
      defaultList && defaultList[idx]
        ? defaultList[idx]
        : Math.min(20000, 80 * Math.pow(2, idx));
    next.push({
      type: idx === count - 1 ? 3 : 1,
      freq: clamp(Math.round(freq), 20, 20000),
      gain: 0.0,
      q: 1.0,
    });
  }
  return next;
}

export function calcBiquadCoeffs(band, fs) {
  if (!band || band.type === 0) {
    return { b0: 1, b1: 0, b2: 0, a1: 0, a2: 0 };
  }
  if (filterUsesGain(band.type) && Math.abs(band.gain) < 0.05) {
    return { b0: 1, b1: 0, b2: 0, a1: 0, a2: 0 };
  }
  const f0 = clamp(band.freq, 10, fs * 0.49);
  const q = clamp(band.q, 0.05, 10.0);
  const g = clamp(band.gain, -24.0, 12.0);
  const omega = (2 * Math.PI * f0) / fs;
  const cosW = Math.cos(omega);
  const sinW = Math.sin(omega);
  const alpha = sinW / (2 * q);
  const A = Math.pow(10, g / 40.0);
  const sqrtA = Math.sqrt(A);

  let b0 = 1;
  let b1 = 0;
  let b2 = 0;
  let a0 = 1;
  let a1 = 0;
  let a2 = 0;

  switch (band.type) {
    case 1:
      b0 = 1 + alpha * A;
      b1 = -2 * cosW;
      b2 = 1 - alpha * A;
      a0 = 1 + alpha / A;
      a1 = -2 * cosW;
      a2 = 1 - alpha / A;
      break;
    case 2:
      b0 = A * (A + 1 - (A - 1) * cosW + 2 * sqrtA * alpha);
      b1 = 2 * A * (A - 1 - (A + 1) * cosW);
      b2 = A * (A + 1 - (A - 1) * cosW - 2 * sqrtA * alpha);
      a0 = A + 1 + (A - 1) * cosW + 2 * sqrtA * alpha;
      a1 = -2 * (A - 1 + (A + 1) * cosW);
      a2 = A + 1 + (A - 1) * cosW - 2 * sqrtA * alpha;
      break;
    case 3:
      b0 = A * (A + 1 + (A - 1) * cosW + 2 * sqrtA * alpha);
      b1 = -2 * A * (A - 1 + (A + 1) * cosW);
      b2 = A * (A + 1 + (A - 1) * cosW - 2 * sqrtA * alpha);
      a0 = A + 1 - (A - 1) * cosW + 2 * sqrtA * alpha;
      a1 = 2 * (A - 1 - (A + 1) * cosW);
      a2 = A + 1 - (A - 1) * cosW - 2 * sqrtA * alpha;
      break;
    case 4:
      b0 = (1 + cosW) * 0.5;
      b1 = -(1 + cosW);
      b2 = (1 + cosW) * 0.5;
      a0 = 1 + alpha;
      a1 = -2 * cosW;
      a2 = 1 - alpha;
      break;
    case 5:
      b0 = (1 - cosW) * 0.5;
      b1 = 1 - cosW;
      b2 = (1 - cosW) * 0.5;
      a0 = 1 + alpha;
      a1 = -2 * cosW;
      a2 = 1 - alpha;
      break;
    case 6:
      b0 = 1;
      b1 = -2 * cosW;
      b2 = 1;
      a0 = 1 + alpha;
      a1 = -2 * cosW;
      a2 = 1 - alpha;
      break;
    case 7:
      b0 = alpha;
      b1 = 0;
      b2 = -alpha;
      a0 = 1 + alpha;
      a1 = -2 * cosW;
      a2 = 1 - alpha;
      break;
  }
  if (!Number.isFinite(a0) || Math.abs(a0) < 1e-12) {
    return { b0: 1, b1: 0, b2: 0, a1: 0, a2: 0 };
  }
  return { b0: b0 / a0, b1: b1 / a0, b2: b2 / a0, a1: a1 / a0, a2: a2 / a0 };
}

export function evalCascadeAtFreq(bands, freq, fs) {
  const w = (2 * Math.PI * freq) / fs;
  const c1 = Math.cos(w);
  const s1 = -Math.sin(w);
  const c2 = Math.cos(2 * w);
  const s2 = -Math.sin(2 * w);

  let re = 1.0;
  let im = 0.0;
  let phaseSum = 0.0;

  for (let i = 0; i < bands.length; i++) {
    const c = calcBiquadCoeffs(bands[i], fs);
    const nr = c.b0 + c.b1 * c1 + c.b2 * c2;
    const ni = c.b1 * s1 + c.b2 * s2;
    const dr = 1.0 + c.a1 * c1 + c.a2 * c2;
    const di = c.a1 * s1 + c.a2 * s2;
    const den = dr * dr + di * di;
    if (den < 1e-24) continue;

    const hr = (nr * dr + ni * di) / den;
    const hi = (ni * dr - nr * di) / den;
    const newRe = re * hr - im * hi;
    const newIm = re * hi + im * hr;
    re = newRe;
    im = newIm;
    phaseSum += Math.atan2(hi, hr);
  }
  const mag = Math.sqrt(re * re + im * im);
  return {
    magDb: 20 * Math.log10(Math.max(mag, 1e-7)),
    phaseRad: phaseSum,
  };
}
