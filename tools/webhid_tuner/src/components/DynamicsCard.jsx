import React, { useRef } from 'react';
import { clamp } from '../dsp/biquad.js';
import { DEFAULT_DSP_PRO, DEFAULT_HARMONICS, EXCITER_MODES } from '../hid/protocol.js';

export function DynamicsCard({
  dynamics,
  harmonics = DEFAULT_HARMONICS,
  dspPro = DEFAULT_DSP_PRO,
  sampleRateHz,
  onChangeDynamics,
  onChangeHarmonics,
  onChangeDspPro,
  onChangePower,
  onUploadFirFile,
  onClearFir,
}) {
  const firInputRef = useRef(null);
  const maxCap = sampleRateHz > 48000 ? 128 : 256;
  const harm = harmonics || DEFAULT_HARMONICS;
  const pro = dspPro || DEFAULT_DSP_PRO;
  const emitHarmonics = (patch) => {
    if (onChangeHarmonics) onChangeHarmonics({ ...harm, ...patch });
  };
  const emitDspPro = (patch) => {
    if (onChangeDspPro) onChangeDspPro({ ...pro, ...patch });
  };

  return (
    <div className="card">
      <div className="card-header">
        <span>Dynamics, Harmonics, FIR Impulse Filter &amp; Power Settings</span>
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
          <div style={{ fontSize: '0.75rem', color: 'var(--text-secondary)', marginBottom: '6px' }}>
            1.5 ms lookahead with -0.5 dBFS ceiling
          </div>
          <div className="control-label-row">
            <label>
              <input
                type="checkbox"
                id="chkAutoPreamp"
                checked={!!dynamics.autoPreampEnabled}
                onChange={(e) =>
                  onChangeDynamics({ ...dynamics, autoPreampEnabled: e.target.checked })
                }
              />{' '}
              Auto-Preamp Headroom Guard
              {dynamics.autoPreampEnabled && (
                <span
                  style={{
                    marginLeft: '6px',
                    fontFamily: 'monospace',
                    fontSize: '0.75rem',
                    color: 'var(--green)',
                  }}
                >
                  ({(dynamics.autoPreampDb || 0).toFixed(1)} dB)
                </span>
              )}
            </label>
          </div>
          <div className="control-label-row" style={{ marginTop: '6px' }}>
            <label>
              <input
                type="checkbox"
                id="chkSubsonicFilter"
                checked={!!pro.subsonicFilterEnabled}
                onChange={(e) => emitDspPro({ subsonicFilterEnabled: e.target.checked })}
              />{' '}
              Subsonic / DC Blocker (18 Hz)
            </label>
          </div>
          <div className="control-label-row" style={{ marginTop: '4px' }}>
            <label>
              <input
                type="checkbox"
                id="chkTpdfDither"
                checked={!!pro.tpdfDitherEnabled}
                onChange={(e) => emitDspPro({ tpdfDitherEnabled: e.target.checked })}
              />{' '}
              24-Bit TPDF Dither
            </label>
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
                oledView: clamp(parseInt(e.target.value, 10), 0, 4),
              })
            }
          >
            <option value={0}>Classic</option>
            <option value={1}>EQ Curve</option>
            <option value={2}>VU Meters</option>
            <option value={3}>Split</option>
            <option value={4}>Spectrum (32-Band FFT)</option>
          </select>
        </div>

        <div>
          <div className="control-label-row">
            <span>Volume Taper:</span>
          </div>
          <select
            id="volTaperSelect"
            value={pro.volTaperLogEnabled ? 1 : 0}
            onChange={(e) =>
              emitDspPro({ volTaperLogEnabled: parseInt(e.target.value, 10) === 1 })
            }
          >
            <option value={0}>Standard (Quadratic)</option>
            <option value={1}>Logarithmic dB (IEM)</option>
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
        className="mixer-grid"
        style={{
          marginBottom: '12px',
          borderTop: '1px solid var(--border-color)',
          paddingTop: '12px',
        }}
      >
        <div style={{ opacity: harm.bassEnabled ? 1 : 0.72 }}>
          <div className="control-label-row">
            <label>
              <input
                type="checkbox"
                id="chkBassEnhEnabled"
                checked={!!harm.bassEnabled}
                onChange={(e) => emitHarmonics({ bassEnabled: e.target.checked })}
              />{' '}
              Bass Enhancer
            </label>
            <span style={{ fontSize: '0.72rem', color: 'var(--text-secondary)' }}>
              Sub-bass harmonics
            </span>
          </div>
          <div className="control-group" style={{ marginBottom: '8px' }}>
            <div className="control-label-row">
              <label htmlFor="bassAmountSlider">Bass Drive %:</label>
              <span
                id="bassAmountLabel"
                style={{ fontFamily: 'monospace', color: 'var(--accent-color)' }}
              >
                {harm.bassAmount}%
              </span>
            </div>
            <input
              type="range"
              id="bassAmountSlider"
              min={0}
              max={100}
              step={1}
              value={harm.bassAmount}
              onChange={(e) =>
                emitHarmonics({ bassAmount: clamp(parseInt(e.target.value, 10), 0, 100) })
              }
            />
          </div>
          <div className="control-group" style={{ marginBottom: 0 }}>
            <div className="control-label-row">
              <label htmlFor="bassFreqSlider">Cutoff (50–150 Hz):</label>
              <span
                id="bassFreqLabel"
                style={{ fontFamily: 'monospace', color: 'var(--accent-color)' }}
              >
                {harm.bassFreq} Hz
              </span>
            </div>
            <input
              type="range"
              id="bassFreqSlider"
              min={50}
              max={150}
              step={1}
              value={harm.bassFreq}
              onChange={(e) =>
                emitHarmonics({ bassFreq: clamp(parseInt(e.target.value, 10), 50, 150) })
              }
            />
          </div>
          <div
            style={{
              marginTop: '10px',
              paddingTop: '8px',
              borderTop: '1px solid var(--border-color)',
              opacity: pro.dynamicBassEnabled ? 1 : 0.72,
            }}
          >
            <div className="control-label-row">
              <label>
                <input
                  type="checkbox"
                  id="chkDynBassEnabled"
                  checked={!!pro.dynamicBassEnabled}
                  onChange={(e) => emitDspPro({ dynamicBassEnabled: e.target.checked })}
                />{' '}
                Dynamic Bass
              </label>
              <span style={{ fontFamily: 'monospace', color: 'var(--accent-color)' }}>
                {pro.dynamicBassAmount}%
              </span>
            </div>
            <input
              type="range"
              id="dynBassAmountSlider"
              min={0}
              max={100}
              step={1}
              value={pro.dynamicBassAmount}
              onChange={(e) =>
                emitDspPro({ dynamicBassAmount: clamp(parseInt(e.target.value, 10), 0, 100) })
              }
            />
          </div>
        </div>

        <div style={{ opacity: harm.exciterEnabled ? 1 : 0.72 }}>
          <div className="control-label-row">
            <label>
              <input
                type="checkbox"
                id="chkExciterEnabled"
                checked={!!harm.exciterEnabled}
                onChange={(e) => emitHarmonics({ exciterEnabled: e.target.checked })}
              />{' '}
              Harmonic Exciter
            </label>
            <select
              id="exciterModeSelect"
              aria-label="Harmonic Exciter Mode"
              style={{ width: 'auto', padding: '3px 8px' }}
              value={harm.exciterMode}
              onChange={(e) =>
                emitHarmonics({ exciterMode: clamp(parseInt(e.target.value, 10), 0, 2) })
              }
            >
              {EXCITER_MODES.map((label, idx) => (
                <option key={label} value={idx}>
                  {label}
                </option>
              ))}
            </select>
          </div>
          <div className="control-group" style={{ marginBottom: '8px' }}>
            <div className="control-label-row">
              <label htmlFor="exciterAmountSlider">Excite %:</label>
              <span
                id="exciterAmountLabel"
                style={{ fontFamily: 'monospace', color: 'var(--accent-color)' }}
              >
                {harm.exciterAmount}%
              </span>
            </div>
            <input
              type="range"
              id="exciterAmountSlider"
              min={0}
              max={100}
              step={1}
              value={harm.exciterAmount}
              onChange={(e) =>
                emitHarmonics({ exciterAmount: clamp(parseInt(e.target.value, 10), 0, 100) })
              }
            />
          </div>
          <div className="control-group" style={{ marginBottom: 0 }}>
            <div className="control-label-row">
              <label htmlFor="exciterFreqSlider">Cutoff (1.5–8.0 kHz):</label>
              <span
                id="exciterFreqLabel"
                style={{ fontFamily: 'monospace', color: 'var(--accent-color)' }}
              >
                {(harm.exciterFreq / 1000).toFixed(2)} kHz
              </span>
            </div>
            <input
              type="range"
              id="exciterFreqSlider"
              min={1500}
              max={8000}
              step={50}
              value={harm.exciterFreq}
              onChange={(e) =>
                emitHarmonics({ exciterFreq: clamp(parseInt(e.target.value, 10), 1500, 8000) })
              }
            />
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
