package com.ascendance.dac.usb

import com.ascendance.dac.model.DspDefaults
import com.ascendance.dac.model.MixerConfig
import com.ascendance.dac.model.PeqBand
import com.ascendance.dac.model.PeqChannel
import java.nio.ByteBuffer
import java.nio.ByteOrder
import kotlin.math.roundToInt

sealed interface HidIncomingMessage {
    data class BandProfileReport(
        val bandIdx: Int,
        val band: PeqBand,
        val numBands: Int,
        val channel: PeqChannel,
        val linkedStereo: Boolean,
        val abBypass: Boolean,
        val activePresetSlot: Int,
        val preampDb: Float,
        val mixerMode: Int,
        val mixerBalance: Int,
        val mixerWidthPct: Int,
        val mixerCrossfeedLevel: Int,
        val sampleRateHz: Int
    ) : HidIncomingMessage

    data class MixerMatrixReport(
        val mixer: MixerConfig
    ) : HidIncomingMessage
}

object PeqPacketCodec {
    const val REPORT_SIZE = 64
    const val MAGIC_16 = 0x4551

    const val USB_VENDOR_ID = 0x303A
    const val USB_PRODUCT_ID = 0x4002

    const val CMD_GET_ACTIVE_PROFILE = 0x01
    const val CMD_SET_BAND_LIVE = 0x02
    const val CMD_SET_PREAMP_LIVE = 0x03
    const val CMD_SELECT_PROFILE = 0x04
    const val CMD_SAVE_TO_PRESET = 0x05
    const val CMD_RESET_TO_PRESET = 0x06
    const val CMD_SET_BAND_COUNT = 0x07
    const val CMD_SET_AB_BYPASS = 0x08
    const val CMD_SET_DUAL_MONO = 0x09
    const val CMD_SET_MIXER = 0x13

    const val BAND_IDX_CUSTOM_MATRIX_QUERY = 0xFE

    private fun newPacket(cmd: Int): ByteBuffer =
        ByteBuffer.allocate(REPORT_SIZE).order(ByteOrder.LITTLE_ENDIAN).apply {
            putShort(0, MAGIC_16.toShort())
            put(2, cmd.toByte())
        }

    fun buildGetBand(bandIdx: Int, channel: PeqChannel): ByteArray {
        val bb = newPacket(CMD_GET_ACTIVE_PROFILE)
        bb.put(3, bandIdx.toByte())
        bb.put(13, channel.code.toByte())
        return bb.array()
    }

    fun buildSetBandCount(count: Int, channel: PeqChannel): ByteArray {
        val safeCount = count.coerceIn(DspDefaults.MIN_BANDS, DspDefaults.MAX_BANDS)
        val bb = newPacket(CMD_SET_BAND_COUNT)
        bb.put(3, safeCount.toByte())
        bb.put(13, channel.code.toByte())
        return bb.array()
    }

    fun buildSetBandLive(bandIdx: Int, channel: PeqChannel, band: PeqBand): ByteArray {
        val clamped = band.clamped()
        val bb = newPacket(CMD_SET_BAND_LIVE)
        bb.put(3, bandIdx.coerceIn(0, DspDefaults.MAX_BANDS - 1).toByte())
        bb.put(4, clamped.type.toByte())
        bb.put(5, if (clamped.type != 0) 1.toByte() else 0.toByte())
        bb.putShort(6, clamped.freq.toShort())
        bb.putShort(8, (clamped.gain * 10.0f).roundToInt().toShort())
        bb.putShort(10, (clamped.q * 100.0f).roundToInt().toShort())
        bb.put(13, channel.code.toByte())
        return bb.array()
    }

    fun buildSetPreampLive(preampDb: Float): ByteArray {
        val clamped = preampDb.coerceIn(DspDefaults.MIN_PREAMP_DB, DspDefaults.MAX_PREAMP_DB)
        val bb = newPacket(CMD_SET_PREAMP_LIVE)
        bb.putShort(8, (clamped * 10.0f).roundToInt().toShort())
        return bb.array()
    }

    fun buildSelectProfile(slot: Int): ByteArray {
        val bb = newPacket(CMD_SELECT_PROFILE)
        bb.put(3, slot.coerceIn(0, 3).toByte())
        return bb.array()
    }

    fun buildSaveToPreset(): ByteArray = newPacket(CMD_SAVE_TO_PRESET).array()

    fun buildSetAbBypass(bypass: Boolean): ByteArray {
        val bb = newPacket(CMD_SET_AB_BYPASS)
        bb.put(3, if (bypass) 1.toByte() else 0.toByte())
        return bb.array()
    }

    fun buildSetDualMono(linkedStereo: Boolean): ByteArray {
        val bb = newPacket(CMD_SET_DUAL_MONO)
        bb.put(3, if (linkedStereo) 1.toByte() else 0.toByte())
        return bb.array()
    }

    fun buildSetMixer(config: MixerConfig): ByteArray {
        val safe = config.sanitized()
        val bb = newPacket(CMD_SET_MIXER)
        bb.put(3, safe.mode.toByte())
        bb.put(4, safe.balance.toByte())
        bb.put(5, safe.widthPct.toByte())
        bb.putShort(6, safe.crossfeedLevel.toShort())
        for (i in 0 until 4) {
            bb.putFloat(12 + i * 4, safe.customMatrix[i])
        }
        return bb.array()
    }

    fun parseReport(raw: ByteArray): HidIncomingMessage? {
        if (raw.size < 28) return null
        val bb = ByteBuffer.wrap(raw).order(ByteOrder.LITTLE_ENDIAN)
        val magic = bb.getShort(0).toInt() and 0xFFFF
        if (magic != MAGIC_16) return null

        val cmd = bb.get(2).toInt() and 0xFF
        if (cmd == CMD_SET_MIXER) {
            val mat = List(4) { idx ->
                bb.getFloat(12 + idx * 4).takeIf { it.isFinite() } ?: if (idx == 0 || idx == 3) 1.0f else 0.0f
            }
            val mixer = MixerConfig(
                mode = bb.get(3).toInt() and 0xFF,
                balance = bb.get(4).toInt(), // signed int8
                widthPct = bb.get(5).toInt() and 0xFF,
                crossfeedLevel = bb.getShort(6).toInt() and 0xFFFF,
                customMatrix = mat
            ).sanitized()
            return HidIncomingMessage.MixerMatrixReport(mixer)
        }

        if (cmd != CMD_GET_ACTIVE_PROFILE) return null

        val bandIdx = bb.get(3).toInt() and 0xFF
        val filterType = bb.get(4).toInt() and 0xFF
        val freqHz = bb.getShort(6).toInt() and 0xFFFF
        val gainDb = bb.getShort(8).toFloat() / 10.0f
        val qVal = (bb.getShort(10).toInt() and 0xFFFF).toFloat() / 100.0f

        val numBands = bb.get(12).toInt() and 0xFF
        val repCh = PeqChannel.fromCode(bb.get(13).toInt() and 0xFF)
        val flags = bb.get(14).toInt() and 0xFF
        val activeSlot = bb.get(15).toInt() and 0xFF
        val preampDb = bb.getShort(16).toFloat() / 10.0f
        val mixerMode = bb.get(18).toInt() and 0xFF
        val mixerBalance = bb.get(19).toInt() // signed int8
        val mixerWidthPct = bb.get(20).toInt() and 0xFF
        val mixerCrossfeedLevel = bb.get(21).toInt() and 0xFF
        val sampleRateHz = bb.getInt(22)

        return HidIncomingMessage.BandProfileReport(
            bandIdx = bandIdx,
            band = PeqBand(type = filterType, freq = freqHz, gain = gainDb, q = qVal).clamped(),
            numBands = numBands.coerceIn(DspDefaults.MIN_BANDS, DspDefaults.MAX_BANDS),
            channel = repCh,
            linkedStereo = (flags and 0x01) != 0,
            abBypass = (flags and 0x02) != 0,
            activePresetSlot = activeSlot,
            preampDb = preampDb.coerceIn(DspDefaults.MIN_PREAMP_DB, DspDefaults.MAX_PREAMP_DB),
            mixerMode = mixerMode,
            mixerBalance = mixerBalance.coerceIn(-100, 100),
            mixerWidthPct = mixerWidthPct.coerceIn(0, 200),
            mixerCrossfeedLevel = mixerCrossfeedLevel.coerceIn(0, 2),
            sampleRateHz = sampleRateHz
        )
    }
}
