import React, { useEffect, useState } from 'react';
import { BAND_COLORS, FILTER_NAMES, clamp, filterUsesGain } from '../dsp/biquad.js';
import {
  computeRecommendedPreampDb,
  parseEqualizerApoText,
  suggestSlotNameFromSource,
} from '../io/fileParsers.js';

function BandCard({
  index,
  band,
  totalBands,
  isActive,
  onHover,
  onUpdateBand,
  onRemoveBand,
}) {
  const [freqDraft, setFreqDraft] = useState(String(Math.round(band.freq)));
  const [gainDraft, setGainDraft] = useState(band.gain.toFixed(1));
  const [qDraft, setQDraft] = useState(band.q.toFixed(2));

  useEffect(() => {
    setFreqDraft(String(Math.round(band.freq)));
  }, [band.freq]);

  useEffect(() => {
    setGainDraft(band.gain.toFixed(1));
  }, [band.gain]);

  useEffect(() => {
    setQDraft(band.q.toFixed(2));
  }, [band.q]);

  const bandColor = BAND_COLORS[index % BAND_COLORS.length];
  const gainDisabled = !filterUsesGain(band.type);

  const commitFreq = () => {
    const val = Math.round(clamp(parseFloat(freqDraft), 20, 20000));
    setFreqDraft(String(val));
    onUpdateBand(index, { ...band, freq: val });
  };

  const commitGain = () => {
    const val = Math.round(clamp(parseFloat(gainDraft), -24.0, 12.0) * 10) / 10;
    setGainDraft(val.toFixed(1));
    onUpdateBand(index, { ...band, gain: val });
  };

  const commitQ = () => {
    const val = Math.round(clamp(parseFloat(qDraft), 0.1, 10.0) * 100) / 100;
    setQDraft(val.toFixed(2));
    onUpdateBand(index, { ...band, q: val });
  };

  return (
    <div
      className={`band-card ${isActive ? 'active-band' : ''}`}
      onMouseEnter={() => onHover(index)}
      onMouseLeave={() => onHover(-1)}
    >
      <div className="band-title">
        <span style={{ display: 'flex', alignItems: 'center', gap: '6px' }}>
          <span className="band-color-dot" style={{ backgroundColor: bandColor }} />
          B{index + 1}
        </span>
        <div style={{ display: 'flex', alignItems: 'center', gap: '4px' }}>
          <select
            id={`type_${index}`}
            style={{ width: 'auto' }}
            value={band.type}
            onChange={(e) => onUpdateBand(index, { ...band, type: parseInt(e.target.value, 10) }, true)}
          >
            <option value={1}>{FILTER_NAMES[1]}</option>
            <option value={2}>{FILTER_NAMES[2]}</option>
            <option value={3}>{FILTER_NAMES[3]}</option>
            <option value={4}>{FILTER_NAMES[4]}</option>
            <option value={5}>{FILTER_NAMES[5]}</option>
            <option value={6}>{FILTER_NAMES[6]}</option>
            <option value={7}>{FILTER_NAMES[7]}</option>
            <option value={0}>{FILTER_NAMES[0]}</option>
          </select>
          <button
            className="btn-del"
            id={`del_${index}`}
            title={`Delete Band ${index + 1}`}
            disabled={totalBands <= 1}
            style={totalBands <= 1 ? { opacity: 0.3, cursor: 'not-allowed' } : undefined}
            onClick={(e) => {
              e.stopPropagation();
              onRemoveBand(index);
            }}
          >
            ✕
          </button>
        </div>
      </div>

      <div className="control-group">
        <div className="control-label-row">
          <label htmlFor={`num_f_${index}`}>Freq (Hz):</label>
          <input
            type="number"
            className="num-input"
            id={`num_f_${index}`}
            min={20}
            max={20000}
            step={1}
            value={freqDraft}
            onChange={(e) => {
              setFreqDraft(e.target.value);
              const val = parseFloat(e.target.value);
              if (!Number.isNaN(val) && val >= 20 && val <= 20000) {
                onUpdateBand(index, { ...band, freq: val });
              }
            }}
            onBlur={commitFreq}
            onKeyDown={(e) => {
              if (e.key === 'Enter') e.currentTarget.blur();
            }}
          />
        </div>
        <input
          type="range"
          id={`freq_${index}`}
          min={20}
          max={20000}
          step={1}
          value={Math.round(band.freq)}
          onChange={(e) => onUpdateBand(index, { ...band, freq: parseFloat(e.target.value) })}
        />
      </div>

      <div className="control-group" style={{ opacity: gainDisabled ? 0.45 : 1 }}>
        <div className="control-label-row">
          <label htmlFor={`num_g_${index}`}>Gain (dB):</label>
          <input
            type="number"
            className="num-input"
            id={`num_g_${index}`}
            min={-24.0}
            max={12.0}
            step={0.1}
            disabled={gainDisabled}
            value={gainDraft}
            onChange={(e) => {
              setGainDraft(e.target.value);
              const val = parseFloat(e.target.value);
              if (!Number.isNaN(val) && val >= -24.0 && val <= 12.0) {
                onUpdateBand(index, { ...band, gain: val });
              }
            }}
            onBlur={commitGain}
            onKeyDown={(e) => {
              if (e.key === 'Enter') e.currentTarget.blur();
            }}
          />
        </div>
        <input
          type="range"
          id={`gain_${index}`}
          min={-24.0}
          max={12.0}
          step={0.1}
          disabled={gainDisabled}
          value={band.gain}
          onChange={(e) => onUpdateBand(index, { ...band, gain: parseFloat(e.target.value) })}
        />
      </div>

      <div className="control-group">
        <div className="control-label-row">
          <label htmlFor={`num_q_${index}`}>Q Factor:</label>
          <input
            type="number"
            className="num-input"
            id={`num_q_${index}`}
            min={0.1}
            max={10.0}
            step={0.01}
            value={qDraft}
            onChange={(e) => {
              setQDraft(e.target.value);
              const val = parseFloat(e.target.value);
              if (!Number.isNaN(val) && val >= 0.1 && val <= 10.0) {
                onUpdateBand(index, { ...band, q: val });
              }
            }}
            onBlur={commitQ}
            onKeyDown={(e) => {
              if (e.key === 'Enter') e.currentTarget.blur();
            }}
          />
        </div>
        <input
          type="range"
          id={`q_${index}`}
          min={0.1}
          max={10.0}
          step={0.05}
          value={band.q}
          onChange={(e) => onUpdateBand(index, { ...band, q: parseFloat(e.target.value) })}
        />
      </div>
    </div>
  );
}

export function BandGrid({
  bands,
  selectedChannel,
  hoveredBandIdx,
  draggedBandIdx,
  onSelectChannel,
  onBandCountPreset,
  onSetCustomBandCount,
  onAddBand,
  onUpdateBand,
  onRemoveBand,
  onHoverBand,
  onImportClick,
  onQuickImportAutoEq,
  sampleRateHz = 48000,
  onExportApo,
  onExportJson,
  onResetBands,
  onSaveToDac,
}) {
  const [countDraft, setCountDraft] = useState(String(bands.length));
  const [quickOpen, setQuickOpen] = useState(false);
  const [quickText, setQuickText] = useState('');
  const [quickSlotName, setQuickSlotName] = useState('');
  const [quickAutoPreamp, setQuickAutoPreamp] = useState(true);

  useEffect(() => {
    setCountDraft(String(bands.length));
  }, [bands.length]);

  const quickParsed = quickText.trim() ? parseEqualizerApoText(quickText, 0.0) : null;
  const quickCalcPreamp =
    quickParsed && quickParsed.bands.length > 0
      ? computeRecommendedPreampDb(quickParsed.bands, sampleRateHz)
      : 0.0;
  const quickEffectivePreamp = quickAutoPreamp
    ? Math.min(quickCalcPreamp, quickParsed?.hasExplicitPreamp ? quickParsed.preamp : 0.0)
    : quickParsed?.preamp ?? 0.0;

  const presetVal = [3, 5, 7, 10].includes(bands.length) ? String(bands.length) : 'custom';

  return (
    <div className="card">
      <div className="card-header">
        <div style={{ display: 'flex', alignItems: 'center', gap: '10px', flexWrap: 'wrap' }}>
          <span>
            Parametric EQ (<span id="bandCountLabel">{bands.length}</span> Bands)
          </span>
          <div className="pill-group" role="group" aria-label="Dual-Mono Channel Selector">
            <button
              id="btnChLinked"
              className={selectedChannel === 0 ? 'seg-active' : ''}
              title="Edit Left & Right channels identically"
              onClick={() => onSelectChannel(0)}
            >
              Stereo Linked
            </button>
            <button
              id="btnChLeft"
              className={selectedChannel === 1 ? 'seg-active' : ''}
              title="Edit Left channel independently (Dual-Mono)"
              onClick={() => onSelectChannel(1)}
            >
              Left (L)
            </button>
            <button
              id="btnChRight"
              className={selectedChannel === 2 ? 'seg-active' : ''}
              title="Edit Right channel independently (Dual-Mono)"
              onClick={() => onSelectChannel(2)}
            >
              Right (R)
            </button>
          </div>
        </div>

        <div style={{ display: 'flex', gap: '8px', alignItems: 'center', flexWrap: 'wrap' }}>
          <label style={{ fontSize: '0.75rem', color: 'var(--text-secondary)' }}>Bands:</label>
          <input
            type="number"
            id="customBandCountInput"
            min={1}
            max={10}
            step={1}
            className="num-input"
            style={{ width: '50px' }}
            value={countDraft}
            onChange={(e) => setCountDraft(e.target.value)}
            onBlur={() => {
              const val = clamp(parseInt(countDraft, 10) || bands.length, 1, 10);
              setCountDraft(String(val));
              onSetCustomBandCount(val);
            }}
            onKeyDown={(e) => {
              if (e.key === 'Enter') e.currentTarget.blur();
            }}
          />
          <select
            id="bandCountSelect"
            style={{ width: 'auto', padding: '3px 8px' }}
            value={presetVal}
            onChange={(e) => {
              if (e.target.value !== 'custom') onBandCountPreset(parseInt(e.target.value, 10));
            }}
          >
            <option value="custom" disabled hidden>
              Custom
            </option>
            <option value="3">3 Bands (Tone)</option>
            <option value="5">5 Bands (Standard)</option>
            <option value="7">7 Bands (Extended)</option>
            <option value="10">10 Bands (Mastering)</option>
          </select>
          <button
            id="btnAddBand"
            className="primary"
            style={{ padding: '4px 10px', fontSize: '0.75rem' }}
            disabled={bands.length >= 10}
            onClick={onAddBand}
          >
            + Add Band
          </button>
        </div>
      </div>

      <div className="controls-grid" id="bandsContainer">
        {bands.map((band, i) => (
          <BandCard
            key={i}
            index={i}
            band={band}
            totalBands={bands.length}
            isActive={i === hoveredBandIdx || i === draggedBandIdx}
            onHover={(idx) => {
              if (draggedBandIdx === -1) onHoverBand(idx);
            }}
            onUpdateBand={onUpdateBand}
            onRemoveBand={onRemoveBand}
          />
        ))}
      </div>

      <div className="footer-actions">
        <div style={{ display: 'flex', gap: '8px', flexWrap: 'wrap' }}>
          <button
            id="btnImportFile"
            title="Import Equalizer APO / AutoEQ .txt or .json backup"
            onClick={onImportClick}
          >
            📂 Import APO / JSON
          </button>
          <button
            id="btnAutoEqQuick"
            className={quickOpen ? 'seg-active' : ''}
            title="Paste or drop AutoEQ ParametricEQ.txt with 1-click Auto-Preamp and Slot Name"
            onClick={() => setQuickOpen((v) => !v)}
          >
            ⚡ AutoEQ Quick-Import
          </button>
          <button
            id="btnExportApo"
            title="Export active channel as Equalizer APO .txt"
            onClick={onExportApo}
          >
            ⬇ Export APO .txt
          </button>
          <button
            id="btnExportJson"
            title="Export full Dual-Mono + Matrix JSON backup"
            onClick={onExportJson}
          >
            ⬇ Export JSON
          </button>
        </div>
        <div style={{ display: 'flex', gap: '8px', flexWrap: 'wrap' }}>
          <button id="btnReset" onClick={onResetBands}>
            ↺ Reset to Flat
          </button>
          <button
            id="btnSave"
            className="primary"
            style={{ backgroundColor: '#1f6feb' }}
            onClick={onSaveToDac}
          >
            💾 Save to DAC
          </button>
        </div>
      </div>

      {quickOpen && (
        <div
          style={{
            marginTop: '12px',
            padding: '12px',
            borderRadius: '8px',
            border: '1px dashed var(--border)',
            background: 'rgba(13, 17, 23, 0.55)',
            display: 'flex',
            flexDirection: 'column',
            gap: '8px',
          }}
          onDragOver={(e) => e.preventDefault()}
          onDrop={async (e) => {
            e.preventDefault();
            const f = e.dataTransfer?.files?.[0];
            if (!f) return;
            const txt = await f.text();
            setQuickText(txt);
            const suggested = suggestSlotNameFromSource(f.name, txt);
            if (suggested) setQuickSlotName(suggested);
          }}
        >
          <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', flexWrap: 'wrap', gap: '8px' }}>
            <strong style={{ fontSize: '0.82rem' }}>AutoEQ Quick-Import (Paste or Drop ParametricEQ.txt)</strong>
            <span style={{ fontSize: '0.75rem', color: 'var(--text-muted)' }}>
              {quickParsed && quickParsed.bands.length > 0
                ? `${quickParsed.bands.length} bands · Headroom Preamp: ${quickEffectivePreamp.toFixed(1)} dB`
                : 'Paste Filter lines or drop a .txt file'}
            </span>
          </div>
          <textarea
            id="txtAutoEqQuick"
            rows={4}
            placeholder={'# Sennheiser HD 600\nPreamp: -6.2 dB\nFilter 1: ON PK Fc 105 Hz Gain 5.5 dB Q 0.70'}
            value={quickText}
            onChange={(e) => {
              const v = e.target.value;
              setQuickText(v);
              if (!quickSlotName.trim()) {
                const suggested = suggestSlotNameFromSource('', v);
                if (suggested) setQuickSlotName(suggested);
              }
            }}
            style={{
              width: '100%',
              boxSizing: 'border-box',
              fontFamily: 'monospace',
              fontSize: '0.76rem',
              padding: '8px',
              borderRadius: '6px',
              background: '#0d1117',
              color: 'var(--text-main)',
              border: '1px solid var(--border)',
            }}
          />
          <div style={{ display: 'flex', alignItems: 'center', gap: '10px', flexWrap: 'wrap' }}>
            <label style={{ display: 'flex', alignItems: 'center', gap: '6px', fontSize: '0.78rem', cursor: 'pointer' }}>
              <input
                type="checkbox"
                checked={quickAutoPreamp}
                onChange={(e) => setQuickAutoPreamp(e.target.checked)}
              />
              Auto-Preamp Headroom
            </label>
            <input
              type="text"
              maxLength={16}
              placeholder="Optional Slot Name (16 chars)"
              value={quickSlotName}
              onChange={(e) => setQuickSlotName(e.target.value.slice(0, 16))}
              style={{
                width: '190px',
                padding: '4px 8px',
                fontSize: '0.78rem',
                borderRadius: '6px',
                background: '#0d1117',
                color: 'var(--text-main)',
                border: '1px solid var(--border)',
              }}
            />
            <button
              id="btnApplyAutoEqQuick"
              className="primary"
              disabled={!quickParsed || quickParsed.bands.length === 0}
              onClick={() => {
                if (!quickParsed || quickParsed.bands.length === 0) return;
                onQuickImportAutoEq?.({
                  bands: quickParsed.bands,
                  preamp: quickEffectivePreamp,
                  slotName: quickSlotName.trim(),
                });
                setQuickOpen(false);
              }}
            >
              ⚡ Preview Live
            </button>
          </div>
        </div>
      )}
    </div>
  );
}
