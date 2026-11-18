package com.ascendance.dac.ui

import android.app.Application
import android.content.Intent
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.ascendance.dac.data.LocalPresetRepository
import com.ascendance.dac.dsp.EqualizerApoCodec
import com.ascendance.dac.dsp.WebHidSnapshotJson
import com.ascendance.dac.model.DspDefaults
import com.ascendance.dac.model.FilterType
import com.ascendance.dac.model.HardwarePreset
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
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.UUID

data class DacUiState(
    val usbInfo: UsbConnectionInfo = UsbConnectionInfo(),
    val offlineDemoMode: Boolean = true,
    val linkedStereo: Boolean = true,
    val selectedChannel: PeqChannel = PeqChannel.LINKED,
    val activePresetSlot: Int = HardwarePreset.USER_CUSTOM.slot,
    val sampleRateHz: Int = 48000,
    val preampDb: Float = 0.0f,
    val abBypass: Boolean = false,
    val leftBands: List<PeqBand> = DspDefaults.createDefaultBands(5),
    val rightBands: List<PeqBand> = DspDefaults.createDefaultBands(5),
    val selectedBandIdx: Int = 0,
    val showPhase: Boolean = false,
    val showGroupDelay: Boolean = false,
    val mixer: MixerConfig = MixerConfig(),
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
            appendLog("Querying hardware Dual-Mono PEQ & Matrix state...")
            for (b in 0 until DspDefaults.MAX_BANDS) {
                transport.sendPacket(PeqPacketCodec.buildGetBand(b, PeqChannel.LEFT))
            }
            for (b in 0 until DspDefaults.MAX_BANDS) {
                transport.sendPacket(PeqPacketCodec.buildGetBand(b, PeqChannel.RIGHT))
            }
            transport.sendPacket(
                PeqPacketCodec.buildGetBand(PeqPacketCodec.BAND_IDX_CUSTOM_MATRIX_QUERY, PeqChannel.LINKED)
            )
            delay(150)
            suppressOfflineDraftTracking = false
        }
    }

    fun pushCurrentStateToHardware(commitToNvs: Boolean = false) {
        val state = _uiState.value
        if (!state.usbInfo.connected) {
            _uiState.update { it.copy(statusBanner = "Connect USB DAC first to push curves.") }
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
            if (commitToNvs) {
                transport.sendPacket(PeqPacketCodec.buildSaveToPreset())
                _uiState.update {
                    it.copy(
                        pendingOfflineDraft = false,
                        statusBanner = "Pushed profile and committed to NVS v2 flash."
                    )
                }
                appendLog("Pushed active state & saved to Hardware NVS.")
            } else {
                _uiState.update {
                    it.copy(
                        pendingOfflineDraft = false,
                        statusBanner = "Pushed draft curve to DAC volatile SRAM."
                    )
                }
                appendLog("Pushed active state to DAC live SRAM.")
            }
        }
    }

    private fun handleIncomingMessage(msg: HidIncomingMessage) {
        when (msg) {
            is HidIncomingMessage.MixerMatrixReport -> {
                _uiState.update { it.copy(mixer = msg.mixer) }
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
                activePresetSlot = HardwarePreset.USER_CUSTOM.slot,
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
                    activePresetSlot = HardwarePreset.USER_CUSTOM.slot,
                    pendingOfflineDraft = draftFlag
                )
                ch == PeqChannel.RIGHT -> cur.copy(
                    rightBands = nextList,
                    activePresetSlot = HardwarePreset.USER_CUSTOM.slot,
                    pendingOfflineDraft = draftFlag
                )
                else -> cur.copy(
                    leftBands = nextList,
                    activePresetSlot = HardwarePreset.USER_CUSTOM.slot,
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
                    activePresetSlot = HardwarePreset.USER_CUSTOM.slot,
                    pendingOfflineDraft = !it.usbInfo.connected || it.pendingOfflineDraft
                )
                ch == PeqChannel.RIGHT -> it.copy(
                    rightBands = newList,
                    selectedBandIdx = newList.lastIndex,
                    activePresetSlot = HardwarePreset.USER_CUSTOM.slot,
                    pendingOfflineDraft = !it.usbInfo.connected || it.pendingOfflineDraft
                )
                else -> it.copy(
                    leftBands = newList,
                    selectedBandIdx = newList.lastIndex,
                    activePresetSlot = HardwarePreset.USER_CUSTOM.slot,
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
                    activePresetSlot = HardwarePreset.USER_CUSTOM.slot,
                    pendingOfflineDraft = !it.usbInfo.connected || it.pendingOfflineDraft
                )
                ch == PeqChannel.RIGHT -> it.copy(
                    rightBands = newList,
                    selectedBandIdx = nextSel,
                    activePresetSlot = HardwarePreset.USER_CUSTOM.slot,
                    pendingOfflineDraft = !it.usbInfo.connected || it.pendingOfflineDraft
                )
                else -> it.copy(
                    leftBands = newList,
                    selectedBandIdx = nextSel,
                    activePresetSlot = HardwarePreset.USER_CUSTOM.slot,
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
        _uiState.update { it.copy(activePresetSlot = slot) }
        if (_uiState.value.usbInfo.connected) {
            transport.sendPacket(PeqPacketCodec.buildSelectProfile(slot))
            viewModelScope.launch {
                delay(80)
                syncFromHardware()
            }
        } else {
            val presetBands = when (slot) {
                HardwarePreset.FLAT.slot -> DspDefaults.createDefaultBands(5)
                HardwarePreset.BASS_BOOST.slot -> listOf(
                    PeqBand(FilterType.LOW_SHELF.code, 105, 4.5f, 0.71f),
                    PeqBand(FilterType.PEAKING.code, 250, 0.0f, 1.00f),
                    PeqBand(FilterType.PEAKING.code, 1000, 0.0f, 1.00f),
                    PeqBand(FilterType.PEAKING.code, 4000, 0.0f, 1.00f),
                    PeqBand(FilterType.HIGH_SHELF.code, 12000, 0.0f, 0.71f)
                )
                HardwarePreset.HARMAN.slot -> listOf(
                    PeqBand(FilterType.LOW_SHELF.code, 105, 5.5f, 0.71f),
                    PeqBand(FilterType.PEAKING.code, 200, -1.5f, 1.20f),
                    PeqBand(FilterType.PEAKING.code, 1200, 1.0f, 1.40f),
                    PeqBand(FilterType.PEAKING.code, 3000, 2.5f, 1.80f),
                    PeqBand(FilterType.HIGH_SHELF.code, 8000, -1.0f, 0.71f)
                )
                else -> _uiState.value.leftBands
            }
            _uiState.update {
                it.copy(
                    leftBands = presetBands,
                    rightBands = presetBands,
                    linkedStereo = true,
                    selectedChannel = PeqChannel.LINKED,
                    selectedBandIdx = 0
                )
            }
        }
        appendLog("Selected hardware preset slot #$slot (${HardwarePreset.fromSlot(slot).label}).")
    }

    fun saveToHardwareNvs() {
        if (!_uiState.value.usbInfo.connected) {
            _uiState.update { it.copy(statusBanner = "Connect USB DAC to commit to hardware NVS.") }
            return
        }
        transport.sendPacket(PeqPacketCodec.buildSaveToPreset())
        _uiState.update {
            it.copy(
                pendingOfflineDraft = false,
                statusBanner = "Saved Dual-Mono PEQ & Matrix to NVS v2 flash!"
            )
        }
        appendLog("Committed Dual-Mono PEQ & Matrix config to ESP32-S3 NVS v2 flash.")
    }

    fun resetActiveChannelFlat() {
        val state = _uiState.value
        val fresh = DspDefaults.createDefaultBands(state.activeBands.size)
        val ch = state.selectedChannel
        _uiState.update {
            when {
                it.linkedStereo -> it.copy(leftBands = fresh, rightBands = fresh, preampDb = 0.0f, activePresetSlot = 3)
                ch == PeqChannel.RIGHT -> it.copy(rightBands = fresh, preampDb = 0.0f, activePresetSlot = 3)
                else -> it.copy(leftBands = fresh, preampDb = 0.0f, activePresetSlot = 3)
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
                activePresetSlot = HardwarePreset.USER_CUSTOM.slot,
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
                        activePresetSlot = HardwarePreset.USER_CUSTOM.slot,
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
                activePresetSlot = HardwarePreset.USER_CUSTOM.slot,
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
