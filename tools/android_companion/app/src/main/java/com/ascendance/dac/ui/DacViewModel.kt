package com.ascendance.dac.ui

import android.app.Application
import android.content.Intent
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.ascendance.dac.data.LocalPresetRepository
import com.ascendance.dac.dsp.EqualizerApoCodec
import com.ascendance.dac.dsp.WebHidSnapshotJson
import com.ascendance.dac.model.DspDefaults
import com.ascendance.dac.model.DynamicsConfig
import com.ascendance.dac.model.FilterType
import com.ascendance.dac.model.LocalPreset
import com.ascendance.dac.model.MixerConfig
import com.ascendance.dac.model.PeqBand
import com.ascendance.dac.model.PeqChannel
import com.ascendance.dac.usb.AscendanceUsbTransport
import com.ascendance.dac.usb.HidIncomingMessage
import com.ascendance.dac.usb.PeqPacketCodec
import com.ascendance.dac.usb.UsbConnectionInfo
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.UUID

data class DacUiState(
    val usbInfo: UsbConnectionInfo = UsbConnectionInfo(),
    val offlineDemoMode: Boolean = true,
    val linkedStereo: Boolean = true,
    val selectedChannel: PeqChannel = PeqChannel.LINKED,
    val activePresetSlot: Int = 0,
    val slotNames: List<String> = DspDefaults.defaultSlotNames(),
    val sampleRateHz: Int = 48000,
    val preampDb: Float = 0.0f,
    val abBypass: Boolean = false,
    val leftBands: List<PeqBand> = DspDefaults.createDefaultBands(5),
    val rightBands: List<PeqBand> = DspDefaults.createDefaultBands(5),
    val selectedBandIdx: Int = 0,
    val showPhase: Boolean = false,
    val showGroupDelay: Boolean = false,
    val mixer: MixerConfig = MixerConfig(),
    val dynamics: DynamicsConfig = DynamicsConfig(),
    val localPresets: List<LocalPreset> = emptyList(),
    val logs: List<String> = emptyList(),
    val statusBanner: String? = null,
    val pendingOfflineDraft: Boolean = false
) {
    val activeBands: List<PeqBand>
        get() = if (selectedChannel == PeqChannel.RIGHT && !linkedStereo) rightBands else leftBands
}

class DacViewModel(application: Application) : AndroidViewModel(application) {

    private val transport = AscendanceUsbTransport(application)
    private val presetRepo = LocalPresetRepository(application)

    private val _uiState = MutableStateFlow(DacUiState())
    val uiState: StateFlow<DacUiState> = _uiState.asStateFlow()

    private val bandThrottleJobs = HashMap<Pair<PeqChannel, Int>, Job>()
    private var preampThrottleJob: Job? = null
    private var mixerThrottleJob: Job? = null
    private var suppressOfflineDraftTracking = false

    private val timeFmt = SimpleDateFormat("HH:mm:ss", Locale.US)

    init {
        appendLog("ASCENDANCE Android Companion ready. Plug USB-C DAC or edit offline.")

        viewModelScope.launch {
            val saved = presetRepo.loadAll()
            _uiState.update { it.copy(localPresets = saved) }
        }

        viewModelScope.launch {
            transport.connectionInfo.collect { info ->
                val wasConnected = _uiState.value.usbInfo.connected
                _uiState.update { state ->
                    state.copy(
                        usbInfo = info,
                        offlineDemoMode = if (info.connected) false else state.offlineDemoMode
                    )
                }
                if (!wasConnected && info.connected) {
                    syncFromHardware()
                }
            }
        }

        viewModelScope.launch {
            transport.incomingMessages.collect { msg ->
                handleIncomingMessage(msg)
            }
        }

        viewModelScope.launch {
            transport.logs.collect { line ->
                appendLog(line)
            }
        }
    }

    fun handleNewIntent(intent: Intent?) {
        transport.handleNewIntent(intent)
    }

    fun connectUsb() {
        transport.scanAndConnect()
    }

    fun disconnectUsb() {
        transport.disconnect()
        _uiState.update { it.copy(offlineDemoMode = true) }
    }

    fun setOfflineDemoMode(enabled: Boolean) {
        if (!enabled && !_uiState.value.usbInfo.connected) {
            connectUsb()
        } else {
            _uiState.update { it.copy(offlineDemoMode = enabled) }
        }
    }

    fun dismissBanner() {
        _uiState.update { it.copy(statusBanner = null) }
    }

    fun syncFromHardware() {
        if (!_uiState.value.usbInfo.connected) return
        viewModelScope.launch {
            suppressOfflineDraftTracking = true
            appendLog("Syncing EQ, Matrix & Dynamics from DAC...")
            for (b in 0 until DspDefaults.MAX_BANDS) {
                transport.sendPacket(PeqPacketCodec.buildGetBand(b, PeqChannel.LEFT))
            }
            for (b in 0 until DspDefaults.MAX_BANDS) {
                transport.sendPacket(PeqPacketCodec.buildGetBand(b, PeqChannel.RIGHT))
            }
            transport.sendPacket(
                PeqPacketCodec.buildGetBand(PeqPacketCodec.BAND_IDX_CUSTOM_MATRIX_QUERY, PeqChannel.LINKED)
            )
            transport.sendPacket(
                PeqPacketCodec.buildGetBand(PeqPacketCodec.BAND_IDX_EXT_STATE_QUERY, PeqChannel.LINKED)
            )
            delay(150)
            suppressOfflineDraftTracking = false
        }
    }

    fun pushCurrentStateToHardware(commitToNvs: Boolean = false) {
        val state = _uiState.value
        if (!state.usbInfo.connected) {
            _uiState.update { it.copy(statusBanner = "Connect USB DAC first to preview curves.") }
            return
        }
        viewModelScope.launch {
            transport.sendPacket(PeqPacketCodec.buildSetDualMono(state.linkedStereo))
            transport.sendPacket(PeqPacketCodec.buildSetPreampLive(state.preampDb))
            if (state.linkedStereo) {
                transport.sendPacket(PeqPacketCodec.buildSetBandCount(state.leftBands.size, PeqChannel.LINKED))
                state.leftBands.forEachIndexed { idx, band ->
                    transport.sendPacket(PeqPacketCodec.buildSetBandLive(idx, PeqChannel.LINKED, band))
                }
            } else {
                transport.sendPacket(PeqPacketCodec.buildSetBandCount(state.leftBands.size, PeqChannel.LEFT))
                state.leftBands.forEachIndexed { idx, band ->
                    transport.sendPacket(PeqPacketCodec.buildSetBandLive(idx, PeqChannel.LEFT, band))
                }
                transport.sendPacket(PeqPacketCodec.buildSetBandCount(state.rightBands.size, PeqChannel.RIGHT))
                state.rightBands.forEachIndexed { idx, band ->
                    transport.sendPacket(PeqPacketCodec.buildSetBandLive(idx, PeqChannel.RIGHT, band))
                }
            }
            transport.sendPacket(PeqPacketCodec.buildSetMixer(state.mixer))
            transport.sendPacket(PeqPacketCodec.buildSetDynamics(state.dynamics))
            if (commitToNvs) {
                transport.sendPacket(PeqPacketCodec.buildSaveToPreset())
                _uiState.update {
                    it.copy(
                        pendingOfflineDraft = false,
                        statusBanner = "Applied profile & saved to DAC."
                    )
                }
                appendLog("Applied active settings & saved to DAC.")
            } else {
                _uiState.update {
                    it.copy(
                        pendingOfflineDraft = false,
                        statusBanner = "Previewing draft curve live on DAC."
                    )
                }
                appendLog("Previewing active settings live on DAC.")
            }
        }
    }

    private fun handleIncomingMessage(msg: HidIncomingMessage) {
        when (msg) {
            is HidIncomingMessage.MixerMatrixReport -> {
                _uiState.update { it.copy(mixer = msg.mixer) }
            }
            is HidIncomingMessage.ExtendedStateReport -> {
                _uiState.update { state ->
                    val updatedNames = state.slotNames.toMutableList()
                    if (msg.activeSlot in updatedNames.indices) {
                        updatedNames[msg.activeSlot] = msg.slotName
                    }
                    state.copy(
                        activePresetSlot = msg.activeSlot,
                        slotNames = updatedNames,
                        dynamics = msg.dynamics
                    )
                }
            }
            is HidIncomingMessage.BandProfileReport -> {
                _uiState.update { state ->
                    val newRate = if (msg.sampleRateHz in DspDefaults.SUPPORTED_SAMPLE_RATES) {
                        msg.sampleRateHz
                    } else {
                        state.sampleRateHz
                    }
                    val updatedTarget = resizeOrUpdateBand(
                        existing = if (msg.channel == PeqChannel.RIGHT && !msg.linkedStereo) state.rightBands else state.leftBands,
                        targetCount = msg.numBands,
                        bandIdx = msg.bandIdx,
                        band = msg.band
                    )
                    val nextLeft = if (msg.channel == PeqChannel.RIGHT && !msg.linkedStereo) state.leftBands else updatedTarget
                    val nextRight = if (msg.linkedStereo) nextLeft else if (msg.channel == PeqChannel.RIGHT) updatedTarget else state.rightBands
                    val nextChannel = when {
                        msg.linkedStereo -> PeqChannel.LINKED
                        state.selectedChannel == PeqChannel.LINKED -> PeqChannel.LEFT
                        else -> state.selectedChannel
                    }
                    state.copy(
                        sampleRateHz = newRate,
                        linkedStereo = msg.linkedStereo,
                        abBypass = msg.abBypass,
                        activePresetSlot = msg.activePresetSlot,
                        preampDb = msg.preampDb,
                        leftBands = nextLeft,
                        rightBands = nextRight,
                        selectedChannel = nextChannel,
                        selectedBandIdx = state.selectedBandIdx.coerceIn(0, nextLeft.lastIndex),
                        mixer = state.mixer.copy(
                            mode = msg.mixerMode,
                            balance = msg.mixerBalance,
                            widthPct = msg.mixerWidthPct,
                            crossfeedLevel = msg.mixerCrossfeedLevel
                        ).sanitized()
                    )
                }
            }
        }
    }

    private fun resizeOrUpdateBand(
        existing: List<PeqBand>,
        targetCount: Int,
        bandIdx: Int,
        band: PeqBand
    ): List<PeqBand> {
        val base = if (existing.size != targetCount) {
            DspDefaults.createDefaultBands(targetCount).toMutableList().also { fresh ->
                for (i in 0 until minOf(existing.size, fresh.size)) {
                    fresh[i] = existing[i]
                }
            }
        } else {
            existing.toMutableList()
        }
        if (bandIdx in base.indices) {
            base[bandIdx] = band
        }
        return base
    }

    fun selectBand(idx: Int) {
        val count = _uiState.value.activeBands.size
        if (count == 0) return
        _uiState.update { it.copy(selectedBandIdx = idx.coerceIn(0, count - 1)) }
    }

    fun togglePhase(show: Boolean) {
        _uiState.update { it.copy(showPhase = show) }
    }

    fun toggleGroupDelay(show: Boolean) {
        _uiState.update { it.copy(showGroupDelay = show) }
    }

    fun selectEditorChannel(ch: PeqChannel) {
        val prev = _uiState.value
        when (ch) {
            PeqChannel.LINKED -> {
                _uiState.update {
                    it.copy(
                        linkedStereo = true,
                        selectedChannel = PeqChannel.LINKED,
                        rightBands = it.leftBands,
                        selectedBandIdx = it.selectedBandIdx.coerceIn(0, it.leftBands.lastIndex)
                    )
                }
                if (prev.usbInfo.connected) {
                    transport.sendPacket(PeqPacketCodec.buildSetDualMono(true))
                    _uiState.value.leftBands.forEachIndexed { idx, band ->
                        transport.sendPacket(PeqPacketCodec.buildSetBandLive(idx, PeqChannel.LINKED, band))
                    }
                }
                appendLog("Switched to Stereo Linked PEQ (L mirrored to R).")
            }
            PeqChannel.LEFT, PeqChannel.RIGHT -> {
                val wasLinked = prev.linkedStereo
                _uiState.update {
                    val target = if (ch == PeqChannel.RIGHT) it.rightBands else it.leftBands
                    it.copy(
                        linkedStereo = false,
                        selectedChannel = ch,
                        selectedBandIdx = it.selectedBandIdx.coerceIn(0, target.lastIndex)
                    )
                }
                if (wasLinked && prev.usbInfo.connected) {
                    transport.sendPacket(PeqPacketCodec.buildSetDualMono(false))
                }
                appendLog("Editing ${ch.label} Channel (Dual-Mono).")
            }
        }
    }

    fun toggleAbBypass() {
        val next = !_uiState.value.abBypass
        _uiState.update { it.copy(abBypass = next) }
        transport.sendPacket(PeqPacketCodec.buildSetAbBypass(next))
        appendLog(if (next) "A/B Compare: EQ BYPASSED (Preamp-normalized)" else "A/B Compare: EQ ACTIVE")
    }

    fun updatePreamp(gainDb: Float) {
        val clamped = gainDb.coerceIn(DspDefaults.MIN_PREAMP_DB, DspDefaults.MAX_PREAMP_DB)
        _uiState.update {
            it.copy(
                preampDb = clamped,
                pendingOfflineDraft = !it.usbInfo.connected || it.pendingOfflineDraft
            )
        }
        preampThrottleJob?.cancel()
        preampThrottleJob = viewModelScope.launch {
            delay(16)
            transport.sendPacket(PeqPacketCodec.buildSetPreampLive(clamped))
        }
    }

    fun updateBand(index: Int, updated: PeqBand, throttleMs: Long = 16L) {
        val clamped = updated.clamped()
        val state = _uiState.value
        val ch = state.selectedChannel
        val targetList = state.activeBands
        if (index !in targetList.indices) return

        val nextList = targetList.toMutableList().apply { this[index] = clamped }
        _uiState.update { cur ->
            val draftFlag = if (!cur.usbInfo.connected && !suppressOfflineDraftTracking) true else cur.pendingOfflineDraft
            when {
                cur.linkedStereo -> cur.copy(
                    leftBands = nextList,
                    rightBands = nextList,
                    pendingOfflineDraft = draftFlag
                )
                ch == PeqChannel.RIGHT -> cur.copy(
                    rightBands = nextList,
                    pendingOfflineDraft = draftFlag
                )
                else -> cur.copy(
                    leftBands = nextList,
                    pendingOfflineDraft = draftFlag
                )
            }
        }

        val key = ch to index
        bandThrottleJobs[key]?.cancel()
        bandThrottleJobs[key] = viewModelScope.launch {
            if (throttleMs > 0) delay(throttleMs)
            transport.sendPacket(PeqPacketCodec.buildSetBandLive(index, ch, clamped))
        }
    }

    fun addBand(freqHz: Int? = null, gainDb: Float = 0.0f) {
        val state = _uiState.value
        val cur = state.activeBands
        if (cur.size >= DspDefaults.MAX_BANDS) return

        val newFreq = (freqHz ?: DspDefaults.suggestNextFrequency(cur))
            .coerceIn(DspDefaults.MIN_FREQ_HZ, DspDefaults.MAX_FREQ_HZ)
        val newBand = PeqBand(
            type = FilterType.PEAKING.code,
            freq = newFreq,
            gain = gainDb.coerceIn(DspDefaults.MIN_GAIN_DB, DspDefaults.MAX_GAIN_DB),
            q = 1.00f
        )
        val newList = cur + newBand
        val ch = state.selectedChannel

        _uiState.update {
            when {
                it.linkedStereo -> it.copy(
                    leftBands = newList,
                    rightBands = newList,
                    selectedBandIdx = newList.lastIndex,
                    pendingOfflineDraft = !it.usbInfo.connected || it.pendingOfflineDraft
                )
                ch == PeqChannel.RIGHT -> it.copy(
                    rightBands = newList,
                    selectedBandIdx = newList.lastIndex,
                    pendingOfflineDraft = !it.usbInfo.connected || it.pendingOfflineDraft
                )
                else -> it.copy(
                    leftBands = newList,
                    selectedBandIdx = newList.lastIndex,
                    pendingOfflineDraft = !it.usbInfo.connected || it.pendingOfflineDraft
                )
            }
        }
        transport.sendPacket(PeqPacketCodec.buildSetBandCount(newList.size, ch))
        transport.sendPacket(PeqPacketCodec.buildSetBandLive(newList.lastIndex, ch, newBand))
        appendLog("Added Band ${newList.size} @ ${newFreq}Hz")
    }

    fun removeBand(idx: Int) {
        val state = _uiState.value
        val cur = state.activeBands
        if (cur.size <= DspDefaults.MIN_BANDS || idx !in cur.indices) return
        val newList = cur.toMutableList().apply { removeAt(idx) }
        val ch = state.selectedChannel

        _uiState.update {
            val nextSel = it.selectedBandIdx.coerceIn(0, newList.lastIndex)
            when {
                it.linkedStereo -> it.copy(
                    leftBands = newList,
                    rightBands = newList,
                    selectedBandIdx = nextSel,
                    pendingOfflineDraft = !it.usbInfo.connected || it.pendingOfflineDraft
                )
                ch == PeqChannel.RIGHT -> it.copy(
                    rightBands = newList,
                    selectedBandIdx = nextSel,
                    pendingOfflineDraft = !it.usbInfo.connected || it.pendingOfflineDraft
                )
                else -> it.copy(
                    leftBands = newList,
                    selectedBandIdx = nextSel,
                    pendingOfflineDraft = !it.usbInfo.connected || it.pendingOfflineDraft
                )
            }
        }
        transport.sendPacket(PeqPacketCodec.buildSetBandCount(newList.size, ch))
        newList.forEachIndexed { bIdx, b ->
            transport.sendPacket(PeqPacketCodec.buildSetBandLive(bIdx, ch, b))
        }
        appendLog("Removed band ${idx + 1} (${newList.size} bands active).")
    }

    fun updateMixer(config: MixerConfig) {
        val safe = config.sanitized()
        _uiState.update {
            it.copy(
                mixer = safe,
                pendingOfflineDraft = !it.usbInfo.connected || it.pendingOfflineDraft
            )
        }
        mixerThrottleJob?.cancel()
        mixerThrottleJob = viewModelScope.launch {
            delay(16)
            transport.sendPacket(PeqPacketCodec.buildSetMixer(safe))
        }
    }

    fun selectHardwarePreset(slot: Int) {
        val safeSlot = slot.coerceIn(0, DspDefaults.USER_SLOT_COUNT - 1)
        _uiState.update { it.copy(activePresetSlot = safeSlot) }
        if (_uiState.value.usbInfo.connected) {
            transport.sendPacket(PeqPacketCodec.buildSelectProfile(safeSlot))
            viewModelScope.launch {
                delay(80)
                syncFromHardware()
            }
        }
        val slotLabel = DspDefaults.formatSlotLabel(safeSlot, _uiState.value.slotNames.getOrNull(safeSlot))
        appendLog("Selected Slot ${safeSlot + 1} ($slotLabel).")
    }

    fun applyStarterCurve(curveKey: String) {
        val (preamp, bands) = when (curveKey) {
            "bass" -> -2.5f to listOf(
                PeqBand(FilterType.LOW_SHELF.code, 80, 4.5f, 0.71f),
                PeqBand(FilterType.PEAKING.code, 250, 1.5f, 1.00f),
                PeqBand(FilterType.PEAKING.code, 1000, 0.0f, 1.00f),
                PeqBand(FilterType.PEAKING.code, 4000, -1.0f, 1.00f),
                PeqBand(FilterType.HIGH_SHELF.code, 10000, 0.5f, 0.71f)
            )
            "harman" -> -4.0f to listOf(
                PeqBand(FilterType.LOW_SHELF.code, 105, 5.0f, 0.71f),
                PeqBand(FilterType.PEAKING.code, 200, -1.5f, 1.20f),
                PeqBand(FilterType.PEAKING.code, 1200, 1.0f, 1.40f),
                PeqBand(FilterType.PEAKING.code, 3000, 2.5f, 1.80f),
                PeqBand(FilterType.HIGH_SHELF.code, 8000, -1.0f, 0.71f)
            )
            else -> 0.0f to DspDefaults.createDefaultBands(5)
        }
        _uiState.update {
            it.copy(
                linkedStereo = true,
                selectedChannel = PeqChannel.LINKED,
                preampDb = preamp,
                leftBands = bands,
                rightBands = bands,
                selectedBandIdx = 0,
                pendingOfflineDraft = !it.usbInfo.connected || it.pendingOfflineDraft,
                statusBanner = "Loaded '$curveKey' template into Slot ${it.activePresetSlot + 1}."
            )
        }
        if (_uiState.value.usbInfo.connected) {
            transport.sendPacket(PeqPacketCodec.buildSetDualMono(true))
            transport.sendPacket(PeqPacketCodec.buildSetBandCount(bands.size, PeqChannel.LINKED))
            transport.sendPacket(PeqPacketCodec.buildSetPreampLive(preamp))
            bands.forEachIndexed { idx, b ->
                transport.sendPacket(PeqPacketCodec.buildSetBandLive(idx, PeqChannel.LINKED, b))
            }
        }
        appendLog("Applied '$curveKey' template to Slot ${_uiState.value.activePresetSlot + 1}.")
    }

    fun renameActiveSlot(newName: String) {
        val state = _uiState.value
        val slot = state.activePresetSlot.coerceIn(0, DspDefaults.USER_SLOT_COUNT - 1)
        val trimmed = newName.trim().take(16).ifEmpty { "Slot ${slot + 1}" }
        _uiState.update { cur ->
            val updated = cur.slotNames.toMutableList()
            if (slot in updated.indices) updated[slot] = trimmed
            cur.copy(slotNames = updated, statusBanner = "Renamed Slot ${slot + 1} to '$trimmed'.")
        }
        if (state.usbInfo.connected) {
            transport.sendPacket(PeqPacketCodec.buildRenameSlot(slot, trimmed))
        }
        appendLog("Renamed Slot ${slot + 1} to '$trimmed'.")
    }

    fun factoryResetActiveSlot() {
        val state = _uiState.value
        val slot = state.activePresetSlot.coerceIn(0, DspDefaults.USER_SLOT_COUNT - 1)
        val defaultName = "Slot ${slot + 1}"
        val flatBands = DspDefaults.createDefaultBands(5)
        _uiState.update { cur ->
            val updated = cur.slotNames.toMutableList()
            if (slot in updated.indices) updated[slot] = defaultName
            cur.copy(
                slotNames = updated,
                linkedStereo = true,
                selectedChannel = PeqChannel.LINKED,
                preampDb = 0.0f,
                leftBands = flatBands,
                rightBands = flatBands,
                selectedBandIdx = 0,
                statusBanner = "Reset Slot ${slot + 1} to Reference Flat."
            )
        }
        if (state.usbInfo.connected) {
            transport.sendPacket(PeqPacketCodec.buildFactoryResetSlot(slot))
        }
        appendLog("Factory reset Slot ${slot + 1} to Reference Flat.")
    }

    fun updateDynamics(config: DynamicsConfig) {
        _uiState.update {
            it.copy(
                dynamics = config,
                pendingOfflineDraft = !it.usbInfo.connected || it.pendingOfflineDraft
            )
        }
        if (_uiState.value.usbInfo.connected) {
            transport.sendPacket(PeqPacketCodec.buildSetDynamics(config))
        }
    }

    fun updatePowerConfig(autoMuteSec: Int, oledDimSec: Int) {
        val safeMute = autoMuteSec.coerceIn(0, 3600)
        val safeDim = oledDimSec.coerceIn(0, 3600)
        _uiState.update {
            it.copy(dynamics = it.dynamics.copy(autoMuteSec = safeMute, oledDimSec = safeDim))
        }
        if (_uiState.value.usbInfo.connected) {
            transport.sendPacket(PeqPacketCodec.buildSetPowerConfig(safeMute, safeDim))
        }
        appendLog("Updated Power Cfg: Auto-Mute=${safeMute}s, OLED Dim=${safeDim}s.")
    }

    fun uploadFirFile(bytes: ByteArray, fileName: String) {
        val state = _uiState.value
        val ch = state.selectedChannel
        val parsed = parseFirImpulseBytes(bytes, fileName)
        if (parsed.isEmpty()) {
            _uiState.update { it.copy(statusBanner = "No valid FIR taps found in $fileName.") }
            return
        }
        val maxCap = if (state.sampleRateHz > 48000) 128 else 256
        val taps = if (parsed.size > maxCap) parsed.copyOf(maxCap) else parsed
        _uiState.update { cur ->
            val nextL = if (ch == PeqChannel.LINKED || ch == PeqChannel.LEFT) taps.size else cur.dynamics.firTapsL
            val nextR = if (ch == PeqChannel.LINKED || ch == PeqChannel.RIGHT) taps.size else cur.dynamics.firTapsR
            cur.copy(
                dynamics = cur.dynamics.copy(
                    firEnabled = true,
                    firTapsL = nextL,
                    firTapsR = nextR,
                    firMaxTaps = maxCap
                ),
                statusBanner = "Uploaded ${taps.size}-tap FIR (${fileName}) to ${ch.label}."
            )
        }
        if (state.usbInfo.connected) {
            PeqPacketCodec.buildUploadFirChunks(ch, taps, commitNvs = true).forEach { pkt ->
                transport.sendPacket(pkt)
            }
        }
        appendLog("Uploaded ${taps.size}-tap FIR (${ch.label}) from $fileName.")
    }

    fun clearFirImpulse() {
        val state = _uiState.value
        val ch = state.selectedChannel
        _uiState.update { cur ->
            val nextL = if (ch == PeqChannel.LINKED || ch == PeqChannel.LEFT) 0 else cur.dynamics.firTapsL
            val nextR = if (ch == PeqChannel.LINKED || ch == PeqChannel.RIGHT) 0 else cur.dynamics.firTapsR
            cur.copy(
                dynamics = cur.dynamics.copy(
                    firEnabled = (nextL > 0 || nextR > 0),
                    firTapsL = nextL,
                    firTapsR = nextR
                ),
                statusBanner = "Cleared FIR taps on ${ch.label}."
            )
        }
        if (state.usbInfo.connected) {
            PeqPacketCodec.buildUploadFirChunks(ch, FloatArray(0), commitNvs = true).forEach { pkt ->
                transport.sendPacket(pkt)
            }
        }
        appendLog("Cleared FIR taps on ${ch.label}.")
    }

    private fun parseFirImpulseBytes(bytes: ByteArray, fileName: String): FloatArray {
        val lower = fileName.lowercase(Locale.US)
        if (lower.endsWith(".wav") && bytes.size >= 44) {
            val bb = ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN)
            val riff = String(bytes, 0, 4, Charsets.US_ASCII)
            val wave = String(bytes, 8, 4, Charsets.US_ASCII)
            if (riff == "RIFF" && wave == "WAVE") {
                var pos = 12
                var audioFmt = 1
                var channels = 1
                var bits = 16
                while (pos + 8 <= bytes.size) {
                    val id = String(bytes, pos, 4, Charsets.US_ASCII)
                    val sz = bb.getInt(pos + 4)
                    val dataOff = pos + 8
                    if (id == "fmt " && sz >= 16) {
                        audioFmt = bb.getShort(dataOff).toInt() and 0xFFFF
                        channels = bb.getShort(dataOff + 2).toInt() and 0xFFFF
                        bits = bb.getShort(dataOff + 14).toInt() and 0xFFFF
                    } else if (id == "data" && sz > 0) {
                        val bps = maxOf(1, bits shr 3)
                        val frameBytes = bps * maxOf(1, channels)
                        val avail = minOf(sz, bytes.size - dataOff)
                        val count = avail / frameBytes
                        val out = FloatArray(count)
                        for (f in 0 until count) {
                            val off = dataOff + f * frameBytes
                            out[f] = when {
                                audioFmt == 3 && bits == 32 -> bb.getFloat(off)
                                bits == 16 -> bb.getShort(off).toFloat() / 32768.0f
                                bits == 24 -> {
                                    val b0 = bytes[off].toInt() and 0xFF
                                    val b1 = bytes[off + 1].toInt() and 0xFF
                                    val b2 = bytes[off + 2].toInt()
                                    ((b0 or (b1 shl 8) or (b2 shl 16)).toFloat()) / 8388608.0f
                                }
                                bits == 32 -> bb.getInt(off).toFloat() / 2147483648.0f
                                else -> 0.0f
                            }
                        }
                        return out
                    }
                    pos = dataOff + sz + (sz and 1)
                }
            }
        }
        val text = String(bytes, Charsets.UTF_8)
        val list = ArrayList<Float>()
        text.split(Regex("[\\s,;]+")).forEach { token ->
            val v = token.trim().toFloatOrNull()
            if (v != null && v.isFinite()) list.add(v)
        }
        return list.toFloatArray()
    }

    fun saveToHardwareNvs() {
        if (!_uiState.value.usbInfo.connected) {
            _uiState.update { it.copy(statusBanner = "Connect USB DAC first to save settings.") }
            return
        }
        transport.sendPacket(PeqPacketCodec.buildSaveToPreset())
        _uiState.update {
            it.copy(
                pendingOfflineDraft = false,
                statusBanner = "Saved Slot ${it.activePresetSlot + 1} & Dynamics to DAC!"
            )
        }
        appendLog("Saved active slot, Matrix & Dynamics to DAC.")
    }

    fun resetActiveChannelFlat() {
        val state = _uiState.value
        val fresh = DspDefaults.createDefaultBands(state.activeBands.size)
        val ch = state.selectedChannel
        _uiState.update {
            when {
                it.linkedStereo -> it.copy(leftBands = fresh, rightBands = fresh, preampDb = 0.0f)
                ch == PeqChannel.RIGHT -> it.copy(rightBands = fresh, preampDb = 0.0f)
                else -> it.copy(leftBands = fresh, preampDb = 0.0f)
            }
        }
        fresh.forEachIndexed { idx, b -> transport.sendPacket(PeqPacketCodec.buildSetBandLive(idx, ch, b)) }
        transport.sendPacket(PeqPacketCodec.buildSetPreampLive(0.0f))
        appendLog("Reset ${ch.label} curve (${fresh.size} bands) to 0.0 dB flat.")
    }

    // Unlimited Local Phone Preset Library
    fun saveCurrentAsLocalPreset(name: String) {
        val state = _uiState.value
        val newPreset = LocalPreset(
            id = UUID.randomUUID().toString(),
            name = name.trim().ifEmpty { "Headphone Profile" },
            updatedAtEpochMs = System.currentTimeMillis(),
            linkedStereo = state.linkedStereo,
            preampDb = state.preampDb,
            leftBands = state.leftBands,
            rightBands = state.rightBands,
            mixer = state.mixer
        )
        val updated = listOf(newPreset) + state.localPresets
        _uiState.update {
            it.copy(
                localPresets = updated,
                statusBanner = "Saved '${newPreset.name}' to phone library."
            )
        }
        viewModelScope.launch { presetRepo.saveAll(updated) }
        appendLog("Saved local preset '${newPreset.name}'.")
    }

    fun applyLocalPreset(preset: LocalPreset, alsoSaveToNvs: Boolean = false) {
        _uiState.update {
            it.copy(
                linkedStereo = preset.linkedStereo,
                selectedChannel = if (preset.linkedStereo) PeqChannel.LINKED else PeqChannel.LEFT,
                preampDb = preset.preampDb,
                leftBands = preset.leftBands,
                rightBands = if (preset.linkedStereo) preset.leftBands else preset.rightBands,
                mixer = preset.mixer,
                selectedBandIdx = 0,
                statusBanner = "Loaded '${preset.name}'."
            )
        }
        if (_uiState.value.usbInfo.connected) {
            pushCurrentStateToHardware(commitToNvs = alsoSaveToNvs)
        }
        appendLog("Applied local preset '${preset.name}'.")
    }

    fun renameLocalPreset(id: String, newName: String) {
        val clean = newName.trim()
        if (clean.isEmpty()) return
        val updated = _uiState.value.localPresets.map { p ->
            if (p.id == id) p.copy(name = clean, updatedAtEpochMs = System.currentTimeMillis()) else p
        }
        _uiState.update { it.copy(localPresets = updated) }
        viewModelScope.launch { presetRepo.saveAll(updated) }
    }

    fun deleteLocalPreset(id: String) {
        val updated = _uiState.value.localPresets.filterNot { it.id == id }
        _uiState.update { it.copy(localPresets = updated) }
        viewModelScope.launch { presetRepo.saveAll(updated) }
    }

    // Equalizer APO / AutoEQ .txt & JSON Import / Export
    fun importProfileText(rawText: String, fileName: String = "imported") {
        val trimmed = rawText.trim()
        if (trimmed.startsWith("{")) {
            try {
                val snap = EqualizerApoCodec.importSnapshotJson(trimmed)
                _uiState.update {
                    it.copy(
                        linkedStereo = snap.linkedStereo,
                        selectedChannel = if (snap.linkedStereo) PeqChannel.LINKED else PeqChannel.LEFT,
                        preampDb = snap.preampDb,
                        leftBands = snap.leftBands,
                        rightBands = if (snap.linkedStereo) snap.leftBands else snap.rightBands,
                        mixer = snap.mixer,
                        selectedBandIdx = 0,
                        pendingOfflineDraft = !it.usbInfo.connected || it.pendingOfflineDraft,
                        statusBanner = "Imported JSON snapshot ($fileName)."
                    )
                }
                if (_uiState.value.usbInfo.connected) pushCurrentStateToHardware(commitToNvs = false)
                appendLog("Imported JSON snapshot from $fileName.")
                return
            } catch (t: Throwable) {
                _uiState.update { it.copy(statusBanner = "Invalid JSON profile: ${t.message}") }
                return
            }
        }

        val parsed = EqualizerApoCodec.parseEqualizerApoText(rawText, _uiState.value.preampDb)
        if (parsed.bands.isEmpty()) {
            _uiState.update { it.copy(statusBanner = "No Equalizer APO Filter lines found.") }
            return
        }
        val ch = _uiState.value.selectedChannel
        _uiState.update {
            val nextLeft = if (ch == PeqChannel.RIGHT && !it.linkedStereo) it.leftBands else parsed.bands
            val nextRight = if (it.linkedStereo || ch == PeqChannel.RIGHT) parsed.bands else it.rightBands
            it.copy(
                preampDb = parsed.preampDb,
                leftBands = nextLeft,
                rightBands = nextRight,
                selectedBandIdx = 0,
                pendingOfflineDraft = !it.usbInfo.connected || it.pendingOfflineDraft,
                statusBanner = "Imported ${parsed.bands.size} APO bands ($fileName)."
            )
        }
        if (_uiState.value.usbInfo.connected) {
            transport.sendPacket(PeqPacketCodec.buildSetBandCount(parsed.bands.size, ch))
            parsed.bands.forEachIndexed { idx, b ->
                transport.sendPacket(PeqPacketCodec.buildSetBandLive(idx, ch, b))
            }
            transport.sendPacket(PeqPacketCodec.buildSetPreampLive(parsed.preampDb))
        }
        appendLog("Imported ${parsed.bands.size} Equalizer APO filters (preamp ${parsed.preampDb} dB).")
    }

    fun exportCurrentApoText(): String {
        val state = _uiState.value
        return EqualizerApoCodec.exportEqualizerApoText(state.preampDb, state.activeBands)
    }

    fun exportCurrentJsonSnapshot(): String {
        val state = _uiState.value
        return EqualizerApoCodec.exportSnapshotJson(
            WebHidSnapshotJson(
                version = 2,
                linkedStereo = state.linkedStereo,
                preampDb = state.preampDb,
                leftBands = state.leftBands,
                rightBands = state.rightBands,
                mixer = state.mixer
            )
        )
    }

    private fun appendLog(line: String) {
        val stamped = "[${timeFmt.format(Date())}] $line"
        _uiState.update { state ->
            state.copy(logs = (state.logs + stamped).takeLast(80))
        }
    }

    override fun onCleared() {
        super.onCleared()
        transport.release()
    }
}
