import React, { useCallback, useRef, useState } from 'react';
import {
  STARTER_CURVES,
  clamp,
  cloneBands,
  createDefaultBands,
  resizeBandList,
  suggestNewBand,
} from './dsp/biquad.js';
import {
  CH_LEFT,
  CH_LINKED,
  CH_RIGHT,
  DEFAULT_DYNAMICS,
  DEFAULT_MIXER,
  SUB_EXT_STATE,
  SUB_MIXER,
  decodeInputReport,
  defaultSlotNames,
  encodeAbBypass,
  encodeDualMonoMode,
  encodeDynamicsConfig,
  encodeFactoryResetSlot,
  encodeFirChunkPackets,
  encodeGetBand,
  encodeMixerConfig,
  encodePowerConfig,
  encodePreampLive,
  encodeRenameSlot,
  encodeSaveToPreset,
  encodeSelectProfile,
  encodeSetBandCount,
  encodeSetBandLive,
} from './hid/protocol.js';
import {
  downloadTextFile,
  exportEqualizerApoText,
  parseEqualizerApoText,
  parseFirFileBuffer,
  sanitizeImportedBands,
} from './io/fileParsers.js';
import { HeaderBar } from './components/HeaderBar.jsx';
import { VisualizerCard } from './components/VisualizerCard.jsx';
import { BandGrid } from './components/BandGrid.jsx';
import { MatrixCard } from './components/MatrixCard.jsx';
import { DynamicsCard } from './components/DynamicsCard.jsx';
import { LogPanel } from './components/LogPanel.jsx';

export default function App() {
  const [linkedStereo, setLinkedStereo] = useState(true);
  const [selectedChannel, setSelectedChannel] = useState(CH_LINKED);
  const [leftBands, setLeftBands] = useState(() => createDefaultBands(5));
  const [rightBands, setRightBands] = useState(() => createDefaultBands(5));
  const [preampDb, setPreampDb] = useState(0.0);
  const [abBypass, setAbBypass] = useState(false);
  const [sampleRateHz, setSampleRateHz] = useState(48000);
  const [connected, setConnected] = useState(false);
  const [activeSlotIdx, setActiveSlotIdx] = useState(0);
  const [slotNames, setSlotNames] = useState(() => defaultSlotNames());
  const [mixer, setMixer] = useState(() => ({ ...DEFAULT_MIXER }));
  const [dynamics, setDynamics] = useState(() => ({ ...DEFAULT_DYNAMICS }));
  const [showPhase, setShowPhase] = useState(false);
  const [showGroupDelay, setShowGroupDelay] = useState(false);
  const [hoveredBandIdx, setHoveredBandIdx] = useState(-1);
  const [draggedBandIdx, setDraggedBandIdx] = useState(-1);
  const [logs, setLogs] = useState([]);

  const hidDeviceRef = useRef(null);
  const fileImportRef = useRef(null);
  const selectedChannelRef = useRef(selectedChannel);
  selectedChannelRef.current = selectedChannel;

  const activeBands = selectedChannel === CH_RIGHT ? rightBands : leftBands;

  const appendLog = useCallback((msg) => {
    const time = new Date().toLocaleTimeString();
    setLogs((prev) => [`[${time}] ${msg}`, ...prev].slice(0, 100));
  }, []);

  const sendReport = useCallback(
    async (bytes) => {
      const dev = hidDeviceRef.current;
      if (!dev || !dev.opened) return;
      try {
        await dev.sendReport(0x00, bytes);
      } catch (e) {
        appendLog(`HID send error: ${e.message}`);
      }
    },
    [appendLog]
  );

  const fetchAllSlotState = useCallback(async () => {
    for (let b = 0; b < 10; b++) await sendReport(encodeGetBand(b, CH_LEFT));
    for (let b = 0; b < 10; b++) await sendReport(encodeGetBand(b, CH_RIGHT));
    await sendReport(encodeGetBand(SUB_MIXER, CH_LINKED));
    await sendReport(encodeGetBand(SUB_EXT_STATE, CH_LINKED));
  }, [sendReport]);

  const handleInputReport = useCallback((evt) => {
    const decoded = decodeInputReport(evt.data);
    if (!decoded) return;

    if (decoded.kind === 'mixer') {
      setMixer(decoded.mixer);
      return;
    }

    if (decoded.kind === 'extState') {
      if (decoded.slotIdx < 8) {
        setActiveSlotIdx(decoded.slotIdx);
        if (decoded.slotName) {
          setSlotNames((prev) => {
            const next = [...prev];
            next[decoded.slotIdx] = decoded.slotName;
            return next;
          });
        }
      }
      setDynamics(decoded.dynamics);
      return;
    }

    if (decoded.kind === 'profile') {
      setLinkedStereo(decoded.linkedStereo);
      setAbBypass(decoded.abBypass);
      if (
        decoded.sampleRateHz === 44100 ||
        decoded.sampleRateHz === 48000 ||
        decoded.sampleRateHz === 96000
      ) {
        setSampleRateHz(decoded.sampleRateHz);
      }
      setPreampDb(decoded.preampDb);
      if (decoded.activeSlot < 8) setActiveSlotIdx(decoded.activeSlot);
      setMixer((prev) => ({
        ...prev,
        ...decoded.mixerPartial,
      }));

      const isRightTarget = decoded.channel === CH_RIGHT && !decoded.linkedStereo;
      const updateTarget = isRightTarget ? setRightBands : setLeftBands;

      updateTarget((prev) => {
        let next =
          decoded.numBands >= 1 && decoded.numBands <= 10 && prev.length !== decoded.numBands
            ? createDefaultBands(decoded.numBands)
            : cloneBands(prev);
        if (decoded.bandIdx < next.length) {
          next[decoded.bandIdx] = { ...decoded.band };
        }
        if (decoded.linkedStereo) {
          setRightBands(cloneBands(next));
        }
        return next;
      });

      if (decoded.linkedStereo) {
        setSelectedChannel(CH_LINKED);
      } else if (selectedChannelRef.current === CH_LINKED) {
        setSelectedChannel(CH_LEFT);
      }
    }
  }, []);

  const handleConnectToggle = useCallback(async () => {
    const curDev = hidDeviceRef.current;
    if (curDev) {
      await curDev.close();
      hidDeviceRef.current = null;
      setConnected(false);
      appendLog('Disconnected from device.');
      return;
    }
    if (!navigator.hid) {
      alert('WebHID is not supported in this browser. Please use Google Chrome or Microsoft Edge.');
      return;
    }
    try {
      const paired = await navigator.hid.getDevices();
      let device = paired.find((d) => d.vendorId === 0x303a && d.productId === 0x4002);
      if (!device) {
        const devices = await navigator.hid.requestDevice({
          filters: [{ vendorId: 0x303a, productId: 0x4002 }],
        });
        if (devices.length === 0) return;
        device = devices[0];
      }
      await device.open();
      device.oninputreport = handleInputReport;
      hidDeviceRef.current = device;
      setConnected(true);
      appendLog(`Connected to ${device.productName || 'ASCENDANCE Hi-Fi DAC'}`);
      await fetchAllSlotState();
    } catch (err) {
      appendLog(`Connection error: ${err.message}`);
    }
  }, [appendLog, fetchAllSlotState, handleInputReport]);

  const replaceActiveBands = useCallback(
    (nextBands) => {
      if (selectedChannel === CH_LINKED) {
        setLeftBands(cloneBands(nextBands));
        setRightBands(cloneBands(nextBands));
      } else if (selectedChannel === CH_LEFT) {
        setLeftBands(cloneBands(nextBands));
      } else {
        setRightBands(cloneBands(nextBands));
      }
    },
    [selectedChannel]
  );

  const handleUpdateBand = useCallback(
    (idx, nextBand) => {
      const targetSetter =
        selectedChannel === CH_RIGHT
          ? setRightBands
          : selectedChannel === CH_LEFT
          ? setLeftBands
          : null;
      if (targetSetter) {
        targetSetter((prev) => prev.map((b, i) => (i === idx ? nextBand : b)));
      } else {
        setLeftBands((prev) => prev.map((b, i) => (i === idx ? nextBand : b)));
        setRightBands((prev) => prev.map((b, i) => (i === idx ? nextBand : b)));
      }
      sendReport(encodeSetBandLive(idx, selectedChannel, nextBand));
    },
    [selectedChannel, sendReport]
  );

  const handleAddBand = useCallback(
    async (freq, gain) => {
      if (activeBands.length >= 10) {
        appendLog('Maximum of 10 bands supported by hardware DSP.');
        return;
      }
      const added = suggestNewBand(activeBands, freq, gain);
      const next = [...activeBands, added];
      replaceActiveBands(next);
      await sendReport(encodeSetBandCount(next.length, selectedChannel));
      for (let i = 0; i < next.length; i++) {
        await sendReport(encodeSetBandLive(i, selectedChannel, next[i]));
      }
    },
    [activeBands, appendLog, replaceActiveBands, selectedChannel, sendReport]
  );

  const handleRemoveBand = useCallback(
    async (idx) => {
      if (activeBands.length <= 1) {
        alert('Minimum 1 active EQ band required.');
        return;
      }
      const next = activeBands.filter((_, i) => i !== idx);
      setHoveredBandIdx(-1);
      setDraggedBandIdx(-1);
      replaceActiveBands(next);
      await sendReport(encodeSetBandCount(next.length, selectedChannel));
      for (let i = 0; i < next.length; i++) {
        await sendReport(encodeSetBandLive(i, selectedChannel, next[i]));
      }
    },
    [activeBands, replaceActiveBands, selectedChannel, sendReport]
  );

  const handleResizeBandCount = useCallback(
    async (targetCount, resetToDefaults = false) => {
      const count = clamp(targetCount, 1, 10);
      const next = resetToDefaults
        ? createDefaultBands(count)
        : resizeBandList(activeBands, count);
      replaceActiveBands(next);
      await sendReport(encodeSetBandCount(count, selectedChannel));
      for (let i = 0; i < count; i++) {
        await sendReport(encodeSetBandLive(i, selectedChannel, next[i]));
      }
    },
    [activeBands, replaceActiveBands, selectedChannel, sendReport]
  );

  const handleSelectChannel = useCallback(
    async (ch) => {
      const wasLinked = linkedStereo;
      setHoveredBandIdx(-1);
      setDraggedBandIdx(-1);
      if (ch === CH_LINKED) {
        const synced = cloneBands(leftBands);
        setRightBands(synced);
        setLinkedStereo(true);
        setSelectedChannel(CH_LINKED);
        await sendReport(encodeDualMonoMode(true));
        for (let i = 0; i < synced.length; i++) {
          await sendReport(encodeSetBandLive(i, CH_LINKED, synced[i]));
        }
        appendLog('Switched to Stereo Linked PEQ (L mirrored to R).');
      } else {
        setLinkedStereo(false);
        setSelectedChannel(ch);
        if (wasLinked) await sendReport(encodeDualMonoMode(false));
        appendLog(`Editing ${ch === CH_LEFT ? 'Left' : 'Right'} Channel (Dual-Mono).`);
      }
    },
    [appendLog, leftBands, linkedStereo, sendReport]
  );

  const handleToggleAb = useCallback(async () => {
    const next = !abBypass;
    setAbBypass(next);
    await sendReport(encodeAbBypass(next));
    appendLog(next ? 'A/B Compare: EQ BYPASSED (Preamp-normalized flat)' : 'A/B Compare: EQ ACTIVE');
  }, [abBypass, appendLog, sendReport]);

  const handleChangePreamp = useCallback(
    (val) => {
      setPreampDb(val);
      sendReport(encodePreampLive(val));
    },
    [sendReport]
  );

  const handleSelectSlot = useCallback(
    async (slot) => {
      setActiveSlotIdx(slot);
      await sendReport(encodeSelectProfile(slot));
      if (hidDeviceRef.current?.opened) {
        await fetchAllSlotState();
      }
      appendLog(`Switched to Slot ${slot + 1}: ${slotNames[slot]}`);
    },
    [appendLog, fetchAllSlotState, sendReport, slotNames]
  );

  const handleRenameSlot = useCallback(async () => {
    const current = slotNames[activeSlotIdx] || `Slot ${activeSlotIdx + 1}`;
    const entered = prompt(`Rename Slot ${activeSlotIdx + 1} (max 16 chars):`, current);
    if (entered === null) return;
    const trimmed = entered.trim().slice(0, 16) || `Slot ${activeSlotIdx + 1}`;
    setSlotNames((prev) => prev.map((n, idx) => (idx === activeSlotIdx ? trimmed : n)));
    await sendReport(encodeRenameSlot(activeSlotIdx, trimmed));
    appendLog(`Renamed Slot ${activeSlotIdx + 1} to "${trimmed}" and saved to DAC.`);
  }, [activeSlotIdx, appendLog, sendReport, slotNames]);

  const handleResetSlot = useCallback(async () => {
    if (!confirm(`Reset Slot ${activeSlotIdx + 1} (${slotNames[activeSlotIdx]}) to Reference Flat?`)) {
      return;
    }
    const flat = STARTER_CURVES.flat;
    const flatBandsL = cloneBands(flat.bands);
    const flatBandsR = cloneBands(flat.bands);
    setSlotNames((prev) =>
      prev.map((n, idx) => (idx === activeSlotIdx ? `Slot ${activeSlotIdx + 1}` : n))
    );
    setPreampDb(0.0);
    setLeftBands(flatBandsL);
    setRightBands(flatBandsR);
    setLinkedStereo(true);
    setSelectedChannel(CH_LINKED);
    await sendReport(encodeFactoryResetSlot(activeSlotIdx));
    appendLog(`Reset Slot ${activeSlotIdx + 1} to Reference Flat.`);
  }, [activeSlotIdx, appendLog, sendReport, slotNames]);

  const handleApplyTemplate = useCallback(
    async (key) => {
      const p = STARTER_CURVES[key];
      if (!p) return;
      const bandsL = cloneBands(p.bands);
      const bandsR = cloneBands(p.bands);
      setPreampDb(p.preamp);
      setLeftBands(bandsL);
      setRightBands(bandsR);
      setLinkedStereo(true);
      setSelectedChannel(CH_LINKED);
      if (hidDeviceRef.current?.opened) {
        await sendReport(encodeDualMonoMode(true));
        await sendReport(encodeSetBandCount(bandsL.length, CH_LINKED));
        await sendReport(encodePreampLive(p.preamp));
        for (let i = 0; i < bandsL.length; i++) {
          await sendReport(encodeSetBandLive(i, CH_LINKED, bandsL[i]));
        }
      }
      appendLog(`Loaded "${key}" preset template into active Slot ${activeSlotIdx + 1}.`);
    },
    [activeSlotIdx, appendLog, sendReport]
  );

  const handleResetBands = useCallback(async () => {
    const count = activeBands.length;
    const fresh = createDefaultBands(count);
    replaceActiveBands(fresh);
    setPreampDb(0.0);
    for (let i = 0; i < count; i++) {
      await sendReport(encodeSetBandLive(i, selectedChannel, fresh[i]));
    }
    await sendReport(encodePreampLive(0.0));
    appendLog(`Reset ${count} EQ bands to flat (0.0 dB).`);
  }, [activeBands.length, appendLog, replaceActiveBands, selectedChannel, sendReport]);

  const handleSaveToDac = useCallback(async () => {
    if (!hidDeviceRef.current?.opened) {
      alert('Please connect to ASCENDANCE DAC via USB first.');
      return;
    }
    await sendReport(encodeSaveToPreset());
    appendLog(`Saved Slot ${activeSlotIdx + 1} (${slotNames[activeSlotIdx]}) & Dynamics to DAC!`);
    alert('Settings permanently saved to DAC!');
  }, [activeSlotIdx, appendLog, sendReport, slotNames]);

  const handleImportFile = useCallback(
    async (e) => {
      const file = e.target.files?.[0];
      if (!file) return;
      const text = await file.text();
      e.target.value = '';

      if (file.name.toLowerCase().endsWith('.json')) {
        try {
          const obj = JSON.parse(text);
          const nextPreamp =
            typeof obj.preampDb === 'number' ? clamp(obj.preampDb, -12.0, 0.0) : preampDb;
          const nextL = sanitizeImportedBands(obj.leftBands) || leftBands;
          const nextR = sanitizeImportedBands(obj.rightBands) || cloneBands(nextL);
          const nextMixer = obj.mixer ? { ...mixer, ...obj.mixer } : mixer;
          const wantLinked = obj.linkedStereo !== false;

          setPreampDb(nextPreamp);
          setLeftBands(nextL);
          setRightBands(nextR);
          setMixer(nextMixer);
          setLinkedStereo(wantLinked);
          setSelectedChannel(wantLinked ? CH_LINKED : CH_LEFT);

          if (obj.mixer) await sendReport(encodeMixerConfig(nextMixer));
          await sendReport(encodeDualMonoMode(wantLinked));
          await sendReport(encodePreampLive(nextPreamp));
          await sendReport(encodeSetBandCount(nextL.length, CH_LEFT));
          for (let i = 0; i < nextL.length; i++) {
            await sendReport(encodeSetBandLive(i, CH_LEFT, nextL[i]));
          }
          if (!wantLinked) {
            await sendReport(encodeSetBandCount(nextR.length, CH_RIGHT));
            for (let i = 0; i < nextR.length; i++) {
              await sendReport(encodeSetBandLive(i, CH_RIGHT, nextR[i]));
            }
          }
          appendLog(`Imported JSON profile: ${file.name}`);
        } catch (err) {
          alert(`Invalid JSON file: ${err.message}`);
        }
        return;
      }

      const parsed = parseEqualizerApoText(text, preampDb);
      if (parsed.bands.length === 0) {
        alert('No valid Equalizer APO / AutoEQ Filter lines found.');
        return;
      }
      setPreampDb(parsed.preamp);
      replaceActiveBands(parsed.bands);
      await sendReport(encodePreampLive(parsed.preamp));
      await sendReport(encodeSetBandCount(parsed.bands.length, selectedChannel));
      for (let i = 0; i < parsed.bands.length; i++) {
        await sendReport(encodeSetBandLive(i, selectedChannel, parsed.bands[i]));
      }
      appendLog(
        `Imported ${parsed.bands.length} bands from ${file.name} (Preamp ${parsed.preamp.toFixed(1)} dB).`
      );
    },
    [appendLog, leftBands, mixer, preampDb, replaceActiveBands, selectedChannel, sendReport]
  );

  const handleChangeMixer = useCallback(
    (nextMixer) => {
      setMixer(nextMixer);
      sendReport(encodeMixerConfig(nextMixer));
    },
    [sendReport]
  );

  const handleChangeDynamics = useCallback(
    (nextDyn) => {
      setDynamics(nextDyn);
      sendReport(encodeDynamicsConfig(nextDyn));
    },
    [sendReport]
  );

  const handleChangePower = useCallback(
    (autoMuteSec, oledDimSec) => {
      setDynamics((prev) => ({ ...prev, autoMuteSec, oledDimSec }));
      sendReport(encodePowerConfig(autoMuteSec, oledDimSec));
    },
    [sendReport]
  );

  const handleUploadFirFile = useCallback(
    async (file) => {
      const buf = await file.arrayBuffer();
      let taps = parseFirFileBuffer(file.name, buf);
      if (taps.length === 0) {
        alert('No valid FIR impulse samples found in file.');
        return;
      }
      const maxCap = sampleRateHz > 48000 ? 128 : 256;
      if (taps.length > maxCap) {
        appendLog(`FIR notice: truncated ${taps.length} taps to ${maxCap} for ${sampleRateHz} Hz.`);
        taps = taps.slice(0, maxCap);
      }
      setDynamics((prev) => ({
        ...prev,
        firTapsL: selectedChannel === CH_LINKED || selectedChannel === CH_LEFT ? taps.length : prev.firTapsL,
        firTapsR:
          selectedChannel === CH_LINKED || selectedChannel === CH_RIGHT ? taps.length : prev.firTapsR,
        firEnabled: true,
      }));
      const packets = encodeFirChunkPackets(selectedChannel, taps, true);
      for (const pkt of packets) await sendReport(pkt);
      appendLog(`Uploaded ${taps.length}-tap FIR impulse from ${file.name} to Ch${selectedChannel}.`);
    },
    [appendLog, sampleRateHz, selectedChannel, sendReport]
  );

  const handleClearFir = useCallback(async () => {
    setDynamics((prev) => {
      const nextL = selectedChannel === CH_LINKED || selectedChannel === CH_LEFT ? 0 : prev.firTapsL;
      const nextR = selectedChannel === CH_LINKED || selectedChannel === CH_RIGHT ? 0 : prev.firTapsR;
      return {
        ...prev,
        firTapsL: nextL,
        firTapsR: nextR,
        firEnabled: nextL === 0 && nextR === 0 ? false : prev.firEnabled,
      };
    });
    for (const pkt of encodeFirChunkPackets(selectedChannel, [], true)) {
      await sendReport(pkt);
    }
    appendLog(`Cleared FIR impulse on Ch${selectedChannel}.`);
  }, [appendLog, selectedChannel, sendReport]);

  return (
    <div className="container">
      <input
        ref={fileImportRef}
        type="file"
        id="fileImportInput"
        accept=".txt,.json"
        style={{ display: 'none' }}
        onChange={handleImportFile}
      />

      <HeaderBar
        sampleRateHz={sampleRateHz}
        abBypass={abBypass}
        connected={connected}
        onToggleAb={handleToggleAb}
        onConnectToggle={handleConnectToggle}
      />

      <VisualizerCard
        bands={activeBands}
        preampDb={preampDb}
        sampleRateHz={sampleRateHz}
        abBypass={abBypass}
        showPhase={showPhase}
        showGroupDelay={showGroupDelay}
        activeSlotIdx={activeSlotIdx}
        slotNames={slotNames}
        hoveredBandIdx={hoveredBandIdx}
        draggedBandIdx={draggedBandIdx}
        onTogglePhase={setShowPhase}
        onToggleGroupDelay={setShowGroupDelay}
        onSelectSlot={handleSelectSlot}
        onRenameSlot={handleRenameSlot}
        onResetSlot={handleResetSlot}
        onApplyTemplate={handleApplyTemplate}
        onChangePreamp={handleChangePreamp}
        onHoverBand={setHoveredBandIdx}
        onDragBand={setDraggedBandIdx}
        onDragEnd={() => setDraggedBandIdx(-1)}
        onUpdateBand={handleUpdateBand}
        onAddBandAt={handleAddBand}
      />

      <BandGrid
        bands={activeBands}
        selectedChannel={selectedChannel}
        hoveredBandIdx={hoveredBandIdx}
        draggedBandIdx={draggedBandIdx}
        onSelectChannel={handleSelectChannel}
        onBandCountPreset={(count) => handleResizeBandCount(count, true)}
        onSetCustomBandCount={(count) => handleResizeBandCount(count, false)}
        onAddBand={() => handleAddBand()}
        onUpdateBand={handleUpdateBand}
        onRemoveBand={handleRemoveBand}
        onHoverBand={setHoveredBandIdx}
        onImportClick={() => fileImportRef.current?.click()}
        onExportApo={() => {
          downloadTextFile(
            'ascendance_peq_apo.txt',
            exportEqualizerApoText(activeBands, preampDb),
            'text/plain'
          );
          appendLog('Exported Equalizer APO .txt profile.');
        }}
        onExportJson={() => {
          const payload = {
            version: 2,
            linkedStereo,
            preampDb,
            leftBands,
            rightBands,
            mixer,
          };
          downloadTextFile(
            'ascendance_dsp_backup.json',
            JSON.stringify(payload, null, 2),
            'application/json'
          );
          appendLog('Exported full Dual-Mono + Matrix JSON backup.');
        }}
        onResetBands={handleResetBands}
        onSaveToDac={handleSaveToDac}
      />

      <MatrixCard mixer={mixer} onChangeMixer={handleChangeMixer} />

      <DynamicsCard
        dynamics={dynamics}
        sampleRateHz={sampleRateHz}
        onChangeDynamics={handleChangeDynamics}
        onChangePower={handleChangePower}
        onUploadFirFile={handleUploadFirFile}
        onClearFir={handleClearFir}
      />

      <LogPanel logs={logs} />
    </div>
  );
}
