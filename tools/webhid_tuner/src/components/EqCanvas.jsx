import React, { useEffect, useRef } from 'react';
import {
  BAND_COLORS,
  FILTER_NAMES,
  clamp,
  evalCascadeAtFreq,
  filterUsesGain,
} from '../dsp/biquad.js';

export function EqCanvas({
  bands,
  preampDb,
  sampleRateHz,
  abBypass,
  showPhase,
  showGroupDelay,
  hoveredBandIdx,
  draggedBandIdx,
  onHoverBand,
  onDragBand,
  onDragEnd,
  onUpdateBand,
  onAddBandAt,
}) {
  const canvasRef = useRef(null);
  const stateRef = useRef({
    bands,
    preampDb,
    hoveredBandIdx,
    draggedBandIdx,
  });

  useEffect(() => {
    stateRef.current = { bands, preampDb, hoveredBandIdx, draggedBandIdx };
  }, [bands, preampDb, hoveredBandIdx, draggedBandIdx]);

  useEffect(() => {
    const canvas = canvasRef.current;
    if (!canvas) return;
    const ctx = canvas.getContext('2d');
    const w = canvas.width;
    const h = canvas.height;

    const freqToX = (freq) => (w * Math.log10(clamp(freq, 20, 20000) / 20.0)) / 3.0;
    const gainToY = (gain) => h / 2.0 - ((gain + preampDb) / 24.0) * (h / 2.0);

    ctx.clearRect(0, 0, w, h);

    const fGrid = [50, 100, 250, 500, 1000, 2000, 4000, 8000, 16000];
    ctx.strokeStyle = '#161b22';
    ctx.lineWidth = 1;
    fGrid.forEach((f) => {
      const x = freqToX(f);
      ctx.beginPath();
      ctx.moveTo(x, 0);
      ctx.lineTo(x, h);
      ctx.stroke();
      ctx.fillStyle = '#30363d';
      ctx.font = '9px monospace';
      ctx.fillText(f >= 1000 ? `${f / 1000}k` : `${f}`, x + 2, h - 4);
    });

    const dbLevels = [12, 6, 0, -6, -12, -18];
    dbLevels.forEach((db) => {
      const y = h / 2 - (db / 24) * (h / 2);
      ctx.beginPath();
      ctx.strokeStyle = db === 0 ? '#30363d' : '#21262d';
      ctx.lineWidth = db === 0 ? 1.5 : 1;
      ctx.moveTo(0, y);
      ctx.lineTo(w, y);
      ctx.stroke();
      ctx.fillStyle = db === 0 ? '#8b949e' : '#484f58';
      ctx.font = '10px monospace';
      ctx.fillText(`${db > 0 ? '+' : ''}${db}dB`, 8, y - 2);
    });

    const activeGuideIdx = draggedBandIdx !== -1 ? draggedBandIdx : hoveredBandIdx;
    if (activeGuideIdx >= 0 && activeGuideIdx < bands.length) {
      const b = bands[activeGuideIdx];
      const nx = freqToX(b.freq);
      const guideColor = BAND_COLORS[activeGuideIdx % BAND_COLORS.length];
      ctx.save();
      ctx.strokeStyle = guideColor;
      ctx.lineWidth = 1;
      ctx.setLineDash([3, 3]);
      ctx.beginPath();
      ctx.moveTo(nx, 0);
      ctx.lineTo(nx, h);
      ctx.stroke();
      if ((b.type === 1 || b.type === 6 || b.type === 7) && b.q > 0.05) {
        const bwOctaves = 1.0 / b.q;
        const xLow = freqToX(b.freq * Math.pow(2, -bwOctaves / 2));
        const xHigh = freqToX(b.freq * Math.pow(2, bwOctaves / 2));
        ctx.fillStyle = 'rgba(88, 166, 255, 0.05)';
        ctx.fillRect(xLow, 0, Math.max(2, xHigh - xLow), h);
      }
      ctx.restore();
    }

    const samples = [];
    const evalBands = abBypass ? [] : bands;
    let prevPhase = 0;
    let unwrappedOffset = 0;

    for (let x = 0; x <= w; x += 2) {
      const freq = 20 * Math.pow(1000, x / w);
      const res = evalCascadeAtFreq(evalBands, freq, sampleRateHz);
      const totalGain = preampDb + res.magDb;

      const rawPhase = res.phaseRad;
      if (samples.length > 0) {
        let diff = rawPhase - prevPhase;
        while (diff > Math.PI) {
          unwrappedOffset -= 2 * Math.PI;
          diff -= 2 * Math.PI;
        }
        while (diff < -Math.PI) {
          unwrappedOffset += 2 * Math.PI;
          diff += 2 * Math.PI;
        }
      }
      prevPhase = rawPhase;

      samples.push({
        x,
        freq,
        yMag: h / 2 - (totalGain / 24.0) * (h / 2),
        phaseUnwrapped: rawPhase + unwrappedOffset,
        phaseWrappedDeg: (rawPhase * 180) / Math.PI,
      });
    }

    if (showGroupDelay && samples.length > 2) {
      ctx.save();
      ctx.strokeStyle = '#3fb950';
      ctx.lineWidth = 1.4;
      ctx.beginPath();
      for (let i = 1; i < samples.length; i++) {
        const dPhi = samples[i].phaseUnwrapped - samples[i - 1].phaseUnwrapped;
        const dW = 2 * Math.PI * (samples[i].freq - samples[i - 1].freq);
        const gdMs = dW > 0 ? clamp((-dPhi / dW) * 1000.0, -2.0, 10.0) : 0.0;
        const yGd = h - (gdMs / 10.0) * (h * 0.85) - 8;
        if (i === 1) ctx.moveTo(samples[i].x, yGd);
        else ctx.lineTo(samples[i].x, yGd);
      }
      ctx.stroke();
      ctx.fillStyle = '#3fb950';
      ctx.font = '9px monospace';
      ctx.fillText('10ms GD', w - 46, 14);
      ctx.restore();
    }

    if (showPhase) {
      ctx.save();
      ctx.strokeStyle = '#d29922';
      ctx.lineWidth = 1.4;
      ctx.beginPath();
      samples.forEach((s, idx) => {
        const yPh = h / 2 - (s.phaseWrappedDeg / 180.0) * (h / 2 - 8);
        if (idx === 0) ctx.moveTo(s.x, yPh);
        else ctx.lineTo(s.x, yPh);
      });
      ctx.stroke();
      ctx.fillStyle = '#d29922';
      ctx.font = '9px monospace';
      ctx.fillText('+180°', w - 34, 26);
      ctx.fillText('-180°', w - 34, h - 14);
      ctx.restore();
    }

    const zeroY = h / 2 - (preampDb / 24.0) * (h / 2);
    const grad = ctx.createLinearGradient(0, 0, 0, h);
    grad.addColorStop(0, abBypass ? 'rgba(210, 153, 34, 0.16)' : 'rgba(88, 166, 255, 0.22)');
    grad.addColorStop(1, 'rgba(88, 166, 255, 0.02)');
    ctx.fillStyle = grad;
    ctx.beginPath();
    ctx.moveTo(0, zeroY);
    samples.forEach((p) => ctx.lineTo(p.x, p.yMag));
    ctx.lineTo(w, zeroY);
    ctx.closePath();
    ctx.fill();

    ctx.strokeStyle = abBypass ? '#d29922' : '#58a6ff';
    ctx.lineWidth = 2.5;
    ctx.beginPath();
    samples.forEach((p, idx) => {
      if (idx === 0) ctx.moveTo(p.x, p.yMag);
      else ctx.lineTo(p.x, p.yMag);
    });
    ctx.stroke();

    bands.forEach((b, i) => {
      const nx = freqToX(b.freq);
      const ny = gainToY(filterUsesGain(b.type) ? b.gain : 0.0);
      const color = BAND_COLORS[i % BAND_COLORS.length];
      const isHovered = i === hoveredBandIdx;
      const isDragged = i === draggedBandIdx;
      const isBypassed = b.type === 0 || abBypass;
      const radius = isHovered || isDragged ? 10 : 8;

      ctx.save();
      if (isHovered || isDragged) {
        ctx.shadowColor = color;
        ctx.shadowBlur = 12;
      }
      ctx.beginPath();
      ctx.arc(nx, ny, radius, 0, 2 * Math.PI);
      if (isBypassed) {
        ctx.fillStyle = '#161b22';
        ctx.fill();
        ctx.strokeStyle = color;
        ctx.lineWidth = 2;
        ctx.setLineDash([2, 2]);
        ctx.stroke();
      } else {
        ctx.fillStyle = color;
        ctx.fill();
        ctx.strokeStyle = '#ffffff';
        ctx.lineWidth = 1.5;
        ctx.stroke();
      }
      ctx.restore();

      ctx.fillStyle = isBypassed ? color : '#0b0e14';
      ctx.font = 'bold 9px -apple-system, BlinkMacSystemFont, sans-serif';
      ctx.textAlign = 'center';
      ctx.textBaseline = 'middle';
      ctx.fillText(`${i + 1}`, nx, ny);
    });

    if (activeGuideIdx >= 0 && activeGuideIdx < bands.length) {
      const b = bands[activeGuideIdx];
      const nx = freqToX(b.freq);
      const ny = gainToY(filterUsesGain(b.type) ? b.gain : 0.0);
      const tName = FILTER_NAMES[b.type] || 'Filter';
      const gTxt = filterUsesGain(b.type)
        ? ` | ${b.gain >= 0 ? '+' : ''}${b.gain.toFixed(1)} dB`
        : '';
      const text = `B${activeGuideIdx + 1} (${tName}): ${Math.round(b.freq)} Hz${gTxt} | Q: ${b.q.toFixed(2)}`;

      ctx.font = '11px -apple-system, BlinkMacSystemFont, sans-serif';
      const tipW = ctx.measureText(text).width + 18;
      const tipH = 22;
      const tipX = clamp(nx - tipW / 2, 8, w - tipW - 8);
      let tipY = ny - 24;
      if (tipY < 6) tipY = ny + 14;

      ctx.save();
      ctx.fillStyle = 'rgba(13, 17, 23, 0.94)';
      ctx.strokeStyle = BAND_COLORS[activeGuideIdx % BAND_COLORS.length];
      ctx.lineWidth = 1.5;
      ctx.beginPath();
      if (ctx.roundRect) ctx.roundRect(tipX, tipY, tipW, tipH, 4);
      else ctx.rect(tipX, tipY, tipW, tipH);
      ctx.fill();
      ctx.stroke();

      ctx.fillStyle = '#f0f6fc';
      ctx.textAlign = 'center';
      ctx.textBaseline = 'middle';
      ctx.fillText(text, tipX + tipW / 2, tipY + tipH / 2);
      ctx.restore();
    }
  }, [bands, preampDb, sampleRateHz, abBypass, showPhase, showGroupDelay, hoveredBandIdx, draggedBandIdx]);

  useEffect(() => {
    const canvas = canvasRef.current;
    if (!canvas) return;

    const getCoords = (evt) => {
      const rect = canvas.getBoundingClientRect();
      const scaleX = canvas.width / rect.width;
      const scaleY = canvas.height / rect.height;
      const clientX = evt.touches && evt.touches.length > 0 ? evt.touches[0].clientX : evt.clientX;
      const clientY = evt.touches && evt.touches.length > 0 ? evt.touches[0].clientY : evt.clientY;
      return { x: (clientX - rect.left) * scaleX, y: (clientY - rect.top) * scaleY };
    };

    const freqToX = (freq) => (canvas.width * Math.log10(clamp(freq, 20, 20000) / 20.0)) / 3.0;
    const xToFreq = (x) => clamp(Math.round(20.0 * Math.pow(1000.0, clamp(x, 0, canvas.width) / canvas.width)), 20, 20000);
    const gainToY = (gain, pre) => canvas.height / 2.0 - ((gain + pre) / 24.0) * (canvas.height / 2.0);
    const yToGain = (y, pre) => {
      const clampedY = clamp(y, 0, canvas.height);
      const totalG = 24.0 * (1.0 - (2.0 * clampedY) / canvas.height);
      return clamp(Math.round((totalG - pre) * 10.0) / 10.0, -24.0, 12.0);
    };

    const findHit = (cx, cy, radius = 14) => {
      const { bands: curBands, preampDb: pre } = stateRef.current;
      let closestIdx = -1;
      let minDistSq = radius * radius;
      for (let i = 0; i < curBands.length; i++) {
        const b = curBands[i];
        const bx = freqToX(b.freq);
        const by = gainToY(filterUsesGain(b.type) ? b.gain : 0.0, pre);
        const distSq = (cx - bx) * (cx - bx) + (cy - by) * (cy - by);
        if (distSq <= minDistSq) {
          minDistSq = distSq;
          closestIdx = i;
        }
      }
      return closestIdx;
    };

    const onDown = (e) => {
      const coords = getCoords(e);
      const hitIdx = findHit(coords.x, coords.y, 14);
      if (hitIdx !== -1) {
        if (e.cancelable) e.preventDefault();
        onDragBand(hitIdx);
        onHoverBand(hitIdx);
        canvas.style.cursor = 'grabbing';
      }
    };

    const onMove = (e) => {
      const { bands: curBands, preampDb: pre, hoveredBandIdx: hov, draggedBandIdx: drag } = stateRef.current;
      const coords = getCoords(e);
      if (drag !== -1 && curBands[drag]) {
        if (e.cancelable) e.preventDefault();
        const cur = curBands[drag];
        const nextFreq = xToFreq(coords.x);
        const nextGain = filterUsesGain(cur.type) ? yToGain(coords.y, pre) : cur.gain;
        onUpdateBand(drag, { ...cur, freq: nextFreq, gain: nextGain });
        return;
      }
      const rect = canvas.getBoundingClientRect();
      const cx = e.touches && e.touches.length > 0 ? e.touches[0].clientX : e.clientX;
      const cy = e.touches && e.touches.length > 0 ? e.touches[0].clientY : e.clientY;
      const isInside = cx >= rect.left && cx <= rect.right && cy >= rect.top && cy <= rect.bottom;
      if (isInside) {
        const hitIdx = findHit(coords.x, coords.y, 14);
        if (hitIdx !== hov) onHoverBand(hitIdx);
        canvas.style.cursor = hitIdx !== -1 ? 'grab' : 'crosshair';
      } else if (hov !== -1) {
        onHoverBand(-1);
      }
    };

    const onUp = () => {
      if (stateRef.current.draggedBandIdx !== -1) {
        onDragEnd();
        canvas.style.cursor = stateRef.current.hoveredBandIdx !== -1 ? 'grab' : 'crosshair';
      }
    };

    const onWheel = (e) => {
      const coords = getCoords(e);
      const hitIdx = findHit(coords.x, coords.y, 18);
      const cur = stateRef.current.bands[hitIdx];
      if (hitIdx !== -1 && cur) {
        e.preventDefault();
        const delta = e.deltaY < 0 ? 0.05 : -0.05;
        const nextQ = clamp(Math.round((cur.q + delta) * 100) / 100, 0.1, 10.0);
        onUpdateBand(hitIdx, { ...cur, q: nextQ });
      }
    };

    const onDblClick = (e) => {
      const coords = getCoords(e);
      const hitIdx = findHit(coords.x, coords.y, 14);
      const { bands: curBands, preampDb: pre } = stateRef.current;
      if (hitIdx !== -1 && curBands[hitIdx]) {
        onUpdateBand(hitIdx, { ...curBands[hitIdx], gain: 0.0 });
      } else if (curBands.length < 10) {
        onAddBandAt(xToFreq(coords.x), yToGain(coords.y, pre));
      }
    };

    canvas.addEventListener('mousedown', onDown);
    window.addEventListener('mousemove', onMove);
    window.addEventListener('mouseup', onUp);
    canvas.addEventListener('touchstart', onDown, { passive: false });
    window.addEventListener('touchmove', onMove, { passive: false });
    window.addEventListener('touchend', onUp);
    canvas.addEventListener('wheel', onWheel, { passive: false });
    canvas.addEventListener('dblclick', onDblClick);

    return () => {
      canvas.removeEventListener('mousedown', onDown);
      window.removeEventListener('mousemove', onMove);
      window.removeEventListener('mouseup', onUp);
      canvas.removeEventListener('touchstart', onDown);
      window.removeEventListener('touchmove', onMove);
      window.removeEventListener('touchend', onUp);
      canvas.removeEventListener('wheel', onWheel);
      canvas.removeEventListener('dblclick', onDblClick);
    };
  }, [onHoverBand, onDragBand, onDragEnd, onUpdateBand, onAddBandAt]);

  return (
    <canvas
      ref={canvasRef}
      id="eqCanvas"
      width={880}
      height={210}
      style={{ cursor: 'crosshair' }}
    />
  );
}
