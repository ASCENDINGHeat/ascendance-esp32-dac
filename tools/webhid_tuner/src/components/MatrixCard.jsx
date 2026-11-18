import React from 'react';
import { clamp } from '../dsp/biquad.js';

export function MatrixCard({ mixer, onChangeMixer }) {
  const balanceText =
    mixer.balance === 0
      ? 'Center (0)'
      : mixer.balance < 0
      ? `L ${Math.abs(mixer.balance)}%`
      : `R ${mixer.balance}%`;

  const updateMatrixCell = (idx, raw) => {
    const val = clamp(parseFloat(raw) || 0, -4, 4);
    const nextMat = [...mixer.customMatrix];
    nextMat[idx] = val;
    onChangeMixer({ ...mixer, mode: 5, customMatrix: nextMat });
  };

  return (
    <div className="card">
      <div className="card-header">
        <span>Channel Matrix &amp; Headphone Crossfeed</span>
        <select
          id="mixerModeSelect"
          style={{ width: 'auto', padding: '5px 10px' }}
          value={mixer.mode}
          onChange={(e) => {
            const nextMode = parseInt(e.target.value, 10);
            const nextCf = nextMode === 4 && mixer.crossfeedLevel === 0 ? 1 : mixer.crossfeedLevel;
            onChangeMixer({ ...mixer, mode: nextMode, crossfeedLevel: nextCf });
          }}
        >
          <option value={0}>Normal Stereo</option>
          <option value={1}>Mono Summed (Downmix)</option>
          <option value={2}>Channel Swap (L ↔ R)</option>
          <option value={3}>M/S Stereo Width</option>
          <option value={4}>Headphone Crossfeed (Chu Moy)</option>
          <option value={5}>Custom Channel Matrix</option>
        </select>
      </div>

      <div className="mixer-grid">
        <div>
          <div className="control-label-row">
            <span>Balance (L / R):</span>
            <span id="balanceLabel" style={{ fontFamily: 'monospace', color: 'var(--accent-color)' }}>
              {balanceText}
            </span>
          </div>
          <input
            type="range"
            id="balanceSlider"
            min={-100}
            max={100}
            step={1}
            value={mixer.balance}
            onChange={(e) => onChangeMixer({ ...mixer, balance: parseInt(e.target.value, 10) })}
          />
        </div>

        <div>
          <div className="control-label-row">
            <span>M/S Stereo Width:</span>
            <span id="widthLabel" style={{ fontFamily: 'monospace', color: 'var(--accent-color)' }}>
              {mixer.widthPct}%
            </span>
          </div>
          <input
            type="range"
            id="widthSlider"
            min={0}
            max={200}
            step={5}
            value={mixer.widthPct}
            onChange={(e) =>
              onChangeMixer({
                ...mixer,
                mode: mixer.mode !== 3 ? 3 : mixer.mode,
                widthPct: parseInt(e.target.value, 10),
              })
            }
          />
        </div>

        <div>
          <div className="control-label-row">
            <span>Headphone Crossfeed (700 Hz / 270 µs):</span>
          </div>
          <select
            id="crossfeedSelect"
            value={mixer.crossfeedLevel}
            onChange={(e) => {
              const nextCf = parseInt(e.target.value, 10);
              const nextMode = nextCf !== 0 && mixer.mode === 0 ? 4 : mixer.mode;
              onChangeMixer({ ...mixer, mode: nextMode, crossfeedLevel: nextCf });
            }}
          >
            <option value={0}>Off (Bypassed)</option>
            <option value={1}>Low (-4.5 dB blend)</option>
            <option value={2}>High (-6.0 dB blend)</option>
          </select>
        </div>

        <div className="matrix-box">
          {[
            ['mLL', 'L→L', 0],
            ['mRL', 'R→L', 1],
            ['mLR', 'L→R', 2],
            ['mRR', 'R→R', 3],
          ].map(([key, label, idx]) => (
            <div className="matrix-cell" key={key}>
              <span>{label}</span>
              <input
                type="number"
                className="num-input"
                id={key.replace('m', 'mat')}
                step={0.05}
                min={-4}
                max={4}
                style={{ width: '58px' }}
                value={Number(mixer.customMatrix[idx] ?? 0).toFixed(2)}
                onChange={(e) => updateMatrixCell(idx, e.target.value)}
              />
            </div>
          ))}
        </div>
      </div>
    </div>
  );
}
