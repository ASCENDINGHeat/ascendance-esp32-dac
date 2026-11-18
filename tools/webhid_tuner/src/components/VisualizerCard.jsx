import React, { useEffect, useState } from 'react';
import { clamp } from '../dsp/biquad.js';
import { formatSlotOptionLabel } from '../hid/protocol.js';
import { EqCanvas } from './EqCanvas.jsx';

export function VisualizerCard({
  bands,
  preampDb,
  sampleRateHz,
  abBypass,
  showPhase,
  showGroupDelay,
  activeSlotIdx,
  slotNames,
  hoveredBandIdx,
  draggedBandIdx,
  onTogglePhase,
  onToggleGroupDelay,
  onSelectSlot,
  onRenameSlot,
  onResetSlot,
  onApplyTemplate,
  onChangePreamp,
  onHoverBand,
  onDragBand,
  onDragEnd,
  onUpdateBand,
  onAddBandAt,
}) {
  const [preampDraft, setPreampDraft] = useState(preampDb.toFixed(1));

  useEffect(() => {
    setPreampDraft(preampDb.toFixed(1));
  }, [preampDb]);

  const commitPreamp = () => {
    const val = Math.round(clamp(parseFloat(preampDraft), -12.0, 0.0) * 10) / 10;
    setPreampDraft(val.toFixed(1));
    onChangePreamp(val);
  };

  return (
    <div className="card">
      <div className="card-header">
        <span>Frequency Response Visualizer</span>
        <div style={{ display: 'flex', gap: '8px', alignItems: 'center', flexWrap: 'wrap' }}>
          <div className="trace-toggles">
            <label title="Overlay unwrapped phase response (-180° to +180°)">
              <input
                type="checkbox"
                id="chkPhase"
                checked={showPhase}
                onChange={(e) => onTogglePhase(e.target.checked)}
              />{' '}
              <span style={{ color: 'var(--amber)' }}>Phase (°)</span>
            </label>
            <label title="Overlay group delay in milliseconds (0 to 10 ms)">
              <input
                type="checkbox"
                id="chkGroupDelay"
                checked={showGroupDelay}
                onChange={(e) => onToggleGroupDelay(e.target.checked)}
              />{' '}
              <span style={{ color: 'var(--green)' }}>Group Delay (ms)</span>
            </label>
          </div>
          <div style={{ display: 'flex', alignItems: 'center', gap: '6px', flexWrap: 'wrap' }}>
            <select
              id="presetSelect"
              style={{ width: 'auto', padding: '6px 10px' }}
              value={activeSlotIdx}
              onChange={(e) => onSelectSlot(clamp(parseInt(e.target.value, 10), 0, 7))}
            >
              {slotNames.map((name, idx) => (
                <option key={idx} value={idx}>
                  {formatSlotOptionLabel(idx, name)}
                </option>
              ))}
            </select>
            <button
              id="btnRenameSlot"
              title="Rename active DAC preset slot"
              style={{ padding: '5px 8px', fontSize: '0.78rem' }}
              onClick={onRenameSlot}
            >
              ✏ Rename
            </button>
            <button
              id="btnResetSlot"
              title="Reset active preset slot to Reference Flat"
              style={{ padding: '5px 8px', fontSize: '0.78rem' }}
              onClick={onResetSlot}
            >
              ↺ Reset Slot
            </button>
            <select
              id="starterSelect"
              title="Load a preset template into the current slot"
              style={{ width: 'auto', padding: '5px 8px', fontSize: '0.78rem' }}
              value=""
              onChange={(e) => {
                if (e.target.value) onApplyTemplate(e.target.value);
              }}
            >
              <option value="">✨ Load Template...</option>
              <option value="flat">Reference Flat (0 dB)</option>
              <option value="bass">Warm Bass Boost (+4.5 dB)</option>
              <option value="harman">Harman OE2018 Target</option>
            </select>
          </div>
        </div>
      </div>

      <EqCanvas
        bands={bands}
        preampDb={preampDb}
        sampleRateHz={sampleRateHz}
        abBypass={abBypass}
        showPhase={showPhase}
        showGroupDelay={showGroupDelay}
        hoveredBandIdx={hoveredBandIdx}
        draggedBandIdx={draggedBandIdx}
        onHoverBand={onHoverBand}
        onDragBand={onDragBand}
        onDragEnd={onDragEnd}
        onUpdateBand={onUpdateBand}
        onAddBandAt={onAddBandAt}
      />

      <div
        style={{
          display: 'flex',
          justifyContent: 'space-between',
          fontSize: '0.74rem',
          color: 'var(--text-secondary)',
          marginBottom: '10px',
          flexWrap: 'wrap',
          gap: '8px',
        }}
      >
        <span>
          💡 <strong>Nodes:</strong> Drag (X=Freq, Y=Gain) • <strong>Wheel</strong> = Q •{' '}
          <strong>Dbl-Click</strong> = Add / Reset
        </span>
        <span style={{ color: 'var(--accent-color)' }}>
          Live Preview Tuning (Save to DAC to Store Permanently)
        </span>
      </div>

      <div
        style={{
          display: 'flex',
          alignItems: 'center',
          justifyContent: 'space-between',
          gap: '16px',
          flexWrap: 'wrap',
        }}
      >
        <div style={{ display: 'flex', alignItems: 'center', gap: '8px' }}>
          <label style={{ fontSize: '0.84rem', color: 'var(--text-secondary)' }} htmlFor="preampNum">
            Preamp Headroom:
          </label>
          <input
            type="number"
            className="num-input"
            id="preampNum"
            min={-12.0}
            max={0.0}
            step={0.1}
            style={{ width: '74px' }}
            value={preampDraft}
            onChange={(e) => {
              setPreampDraft(e.target.value);
              const val = parseFloat(e.target.value);
              if (!Number.isNaN(val) && val >= -12.0 && val <= 0.0) {
                onChangePreamp(val);
              }
            }}
            onBlur={commitPreamp}
            onKeyDown={(e) => {
              if (e.key === 'Enter') e.currentTarget.blur();
            }}
          />
          <span style={{ fontSize: '0.8rem', color: 'var(--text-secondary)' }}>dB</span>
        </div>
        <input
          type="range"
          id="preampSlider"
          min={-12}
          max={0}
          step={0.1}
          value={preampDb}
          style={{ flex: 1, minWidth: '200px' }}
          onChange={(e) => onChangePreamp(parseFloat(e.target.value))}
        />
      </div>
    </div>
  );
}
