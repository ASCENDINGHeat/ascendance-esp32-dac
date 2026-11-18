import React from 'react';

export function LogPanel({ logs }) {
  return (
    <div className="card" style={{ padding: '12px' }}>
      <div className="card-header" style={{ marginBottom: '8px', fontSize: '0.85rem' }}>
        Activity Log
      </div>
      <div className="log-box" id="logBox">
        {logs.length === 0 ? (
          'Awaiting WebHID connection via Chrome/Edge...'
        ) : (
          logs.map((entry, idx) => <div key={idx}>{entry}</div>)
        )}
      </div>
    </div>
  );
}
