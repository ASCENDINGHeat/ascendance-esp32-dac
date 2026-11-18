import React from 'react';

export function HeaderBar({
  sampleRateHz,
  abBypass,
  connected,
  onToggleAb,
  onConnectToggle,
}) {
  return (
    <header>
      <div>
        <h1>
          ASCENDANCE Hi-Fi DAC
          <span className="device-badge">24-Bit Tri-Rate</span>
          <span className="rate-badge" id="rateBadge">
            {(sampleRateHz / 1000).toFixed(1)} kHz / 24b
          </span>
        </h1>
        <p style={{ fontSize: '0.8rem', color: 'var(--text-secondary)', marginTop: '4px' }}>
          Driverless USB Dual-Mono Parametric EQ, Crossfeed &amp; Channel Matrix Tuner
        </p>
      </div>
      <div className="status-panel">
        <button
          id="btnAbToggle"
          className={abBypass ? 'ab-active' : ''}
          title="Instant preamp-normalized flat bypass comparison"
          onClick={onToggleAb}
        >
          {abBypass ? 'A/B: BYPASS (Flat)' : 'A/B: EQ ON'}
        </button>
        <span
          id="connStatus"
          style={{ fontSize: '0.82rem', color: connected ? 'var(--green)' : 'var(--red)' }}
        >
          {connected ? '● Connected (USB OTG)' : '● Disconnected'}
        </span>
        <button id="btnConnect" className="primary" onClick={onConnectToggle}>
          {connected ? 'Disconnect' : 'Connect USB DAC'}
        </button>
      </div>
    </header>
  );
}
