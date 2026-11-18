import React, { useRef } from 'react';
import { clamp } from '../dsp/biquad.js';

export function DynamicsCard({
  dynamics,
  sampleRateHz,
  onChangeDynamics,
  onChangePower,
  onUploadFirFile,
  onClearFir,
}) {
  const firInputRef = useRef(null);
  const maxCap = sampleRateHz > 48000 ? 128 : 256;

  return (
    <div className="card">
      <div className="card-header">
        <span>Dynamics, FIR Impulse Filter &amp; Power Settings</span>
        <span
          id="firStatusBadge"
          style={{
            fontFamily: 'monospace',
            fontSize: '0.78rem',
            color: dynamics.firEnabled ? 'var(--green)' : 'var(--text-secondary)',
          }}
        >
          {dynamics.firEnabled
            ? `FIR: ON (L:${dynamics.firTapsL} / R:${dynamics.firTapsR} taps, cap ${maxCap})`
            : `FIR: Off (L:${dynamics.firTapsL} / R:${dynamics.firTapsR} loaded, cap ${maxCap})`}
        </span>
      </div>

      <div className="mixer-grid" style={{ marginBottom: '12px' }}>
        <div>
          <div className="control-label-row">
            <label>
              <input
                type="checkbox"
                id="chkLimiter"
                checked={!!dynamics.limiterEnabled}
                onChange={(e) =>
                  onChangeDynamics({ ...dynamics, limiterEnabled: e.target.checked })
                }
              />{' '}
              True-Peak Limiter (-0.5 dBFS)
            </label>
          </div>
          <div style={{ fontSize: '0.75rem', color: 'var(--text-secondary)' }}>
            1.5 ms lookahead with -0.5 dBFS ceiling
          </div>
        </div>

        <div>
          <div className="control-label-row">
            <span>Equal Loudness (ISO 226):</span>
          </div>
          <select
            id="loudnessSelect"
            value={dynamics.loudnessMode}
            onChange={(e) =>
              onChangeDynamics({
                ...dynamics,
                loudnessMode: clamp(parseInt(e.target.value, 10), 0, 2),
              })
            }
          >
            <option value={0}>Off (Linear Reference)</option>
            <option value={1}>ISO 226 Mild (+4.0 dB Bass / +1.5 dB Treble)</option>
            <option value={2}>ISO 226 Full (+6.5 dB Bass / +2.5 dB Treble)</option>
          </select>
        </div>

        <div>
          <div className="control-label-row">
            <span>OLED Screen View:</span>
          </div>
          <select
            id="oledViewSelect"
            value={dynamics.oledView}
            onChange={(e) =>
              onChangeDynamics({
                ...dynamics,
                oledView: clamp(parseInt(e.target.value, 10), 0, 3),
              })
            }
          >
            <option value={0}>Classic</option>
            <option value={1}>EQ Curve</option>
            <option value={2}>VU Meters</option>
            <option value={3}>Split</option>
          </select>
        </div>

        <div>
          <div className="control-label-row">
            <span>Auto-Mute on Silence / OLED Auto-Dim:</span>
          </div>
          <div style={{ display: 'flex', gap: '8px' }}>
            <select
              id="autoMuteSelect"
              style={{ flex: 1 }}
              value={dynamics.autoMuteSec}
              onChange={(e) =>
                onChangePower(
                  clamp(parseInt(e.target.value, 10), 0, 3600),
                  dynamics.oledDimSec
                )
              }
            >
              <option value={0}>Auto-Mute: Off</option>
              <option value={10}>Auto-Mute: 10s</option>
              <option value={30}>Auto-Mute: 30s</option>
              <option value={60}>Auto-Mute: 60s</option>
            </select>
            <select
              id="oledDimSelect"
              style={{ flex: 1 }}
              value={dynamics.oledDimSec}
              onChange={(e) =>
                onChangePower(
                  dynamics.autoMuteSec,
                  clamp(parseInt(e.target.value, 10), 0, 3600)
                )
              }
            >
              <option value={0}>Auto-Dim: Off</option>
              <option value={60}>Dim: 60s / Sleep 180s</option>
              <option value={120}>Dim: 120s / Sleep 180s</option>
            </select>
          </div>
        </div>
      </div>

      <div
        style={{
          display: 'flex',
          alignItems: 'center',
          justifyContent: 'space-between',
          gap: '10px',
          flexWrap: 'wrap',
          borderTop: '1px solid var(--border-color)',
          paddingTop: '10px',
        }}
      >
        <div style={{ display: 'flex', alignItems: 'center', gap: '10px', flexWrap: 'wrap' }}>
          <label>
            <input
              type="checkbox"
              id="chkFirEnabled"
              checked={!!dynamics.firEnabled}
              onChange={(e) =>
                onChangeDynamics({ ...dynamics, firEnabled: e.target.checked })
              }
            />{' '}
            Enable FIR Impulse Filter
          </label>
          <input
            ref={firInputRef}
            type="file"
            id="firFileInput"
            accept=".txt,.csv,.wav"
            style={{ display: 'none' }}
            onChange={(e) => {
              const file = e.target.files?.[0];
              e.target.value = '';
              if (file) onUploadFirFile(file);
            }}
          />
          <button
            id="btnUploadFir"
            title="Load Impulse Response (.txt float taps or PCM .wav, capped 256@48k / 128@96k)"
            onClick={() => firInputRef.current?.click()}
          >
            📐 Load FIR (.txt / .wav)
          </button>
          <button id="btnClearFir" onClick={onClearFir}>
            ✖ Clear FIR
          </button>
        </div>
        <span style={{ fontSize: '0.74rem', color: 'var(--text-secondary)' }}>
          Active target follows Dual-Mono channel selector (Linked / L / R)
        </span>
      </div>
    </div>
  );
}
