package com.ascendance.dac

import com.ascendance.dac.model.FilterType
import com.ascendance.dac.model.MixerConfig
import com.ascendance.dac.model.MixerMode
import com.ascendance.dac.model.PeqBand
import com.ascendance.dac.model.PeqChannel
import com.ascendance.dac.usb.HidIncomingMessage
import com.ascendance.dac.usb.PeqPacketCodec
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.nio.ByteBuffer
import java.nio.ByteOrder

class PeqPacketCodecTest {

    @Test
    fun encodesLiveBandDualMonoAbBypassAndMixerPackets() {
        val bandPkt = PeqPacketCodec.buildSetBandLive(
            bandIdx = 2,
            channel = PeqChannel.LEFT,
            band = PeqBand(FilterType.HIGH_PASS.code, 85, 0.0f, 0.71f)
        )
        assertEquals(64, bandPkt.size)
        val bb = ByteBuffer.wrap(bandPkt).order(ByteOrder.LITTLE_ENDIAN)
        assertEquals(0x4551, bb.getShort(0).toInt() and 0xFFFF)
        assertEquals(PeqPacketCodec.CMD_SET_BAND_LIVE, bb.get(2).toInt() and 0xFF)
        assertEquals(2, bb.get(3).toInt() and 0xFF)
        assertEquals(4, bb.get(4).toInt() and 0xFF)
        assertEquals(1, bb.get(5).toInt() and 0xFF)
        assertEquals(85, bb.getShort(6).toInt() and 0xFFFF)
        assertEquals(0, bb.getShort(8).toInt())
        assertEquals(71, bb.getShort(10).toInt() and 0xFFFF)
        assertEquals(PeqChannel.LEFT.code, bb.get(13).toInt() and 0xFF)

        val abPkt = PeqPacketCodec.buildSetAbBypass(true)
        assertEquals(PeqPacketCodec.CMD_SET_AB_BYPASS, abPkt[2].toInt() and 0xFF)
        assertEquals(1, abPkt[3].toInt() and 0xFF)

        val dmPkt = PeqPacketCodec.buildSetDualMono(false)
        assertEquals(PeqPacketCodec.CMD_SET_DUAL_MONO, dmPkt[2].toInt() and 0xFF)
        assertEquals(0, dmPkt[3].toInt() and 0xFF)

        val mixPkt = PeqPacketCodec.buildSetMixer(
            MixerConfig(
                mode = MixerMode.CUSTOM_MATRIX.code,
                balance = -15,
                widthPct = 140,
                crossfeedLevel = 2,
                customMatrix = listOf(0.8f, 0.2f, 0.1f, 0.9f)
            )
        )
        val parsedMixer = PeqPacketCodec.parseReport(mixPkt) as? HidIncomingMessage.MixerMatrixReport
        assertNotNull(parsedMixer)
        assertEquals(-15, parsedMixer!!.mixer.balance)
        assertEquals(140, parsedMixer.mixer.widthPct)
        assertEquals(2, parsedMixer.mixer.crossfeedLevel)
        assertEquals(0.8f, parsedMixer.mixer.customMatrix[0], 1e-5f)
        assertEquals(0.9f, parsedMixer.mixer.customMatrix[3], 1e-5f)
    }

    @Test
    fun decodesProfileReportWithByte12MetadataAndSampleRate() {
        val raw = ByteBuffer.allocate(64).order(ByteOrder.LITTLE_ENDIAN).apply {
            putShort(0, 0x4551)
            put(2, PeqPacketCodec.CMD_GET_ACTIVE_PROFILE.toByte())
            put(3, 1) // band 1
            put(4, FilterType.PEAKING.code.toByte())
            put(5, 1)
            putShort(6, 1000)
            putShort(8, (-35).toShort())
            putShort(10, 141)
            put(12, 7) // num_bands @ byte 12 (reserved[0])
            put(13, PeqChannel.RIGHT.code.toByte())
            put(14, 0x02) // dual-mono (linked=0) + ab_bypass=1
            put(15, 3) // User Custom
            putShort(16, (-40).toShort()) // -4.0 dB preamp
            put(18, MixerMode.CROSSFEED.code.toByte())
            put(19, (-25).toByte())
            put(20, 110)
            put(21, 1)
            putInt(22, 96000)
        }.array()

        val msg = PeqPacketCodec.parseReport(raw) as? HidIncomingMessage.BandProfileReport
        assertNotNull(msg)
        assertEquals(7, msg!!.numBands)
        assertEquals(PeqChannel.RIGHT, msg.channel)
        assertTrue(!msg.linkedStereo)
        assertTrue(msg.abBypass)
        assertEquals(3, msg.activePresetSlot)
        assertEquals(-4.0f, msg.preampDb, 0.01f)
        assertEquals(-25, msg.mixerBalance)
        assertEquals(96000, msg.sampleRateHz)
        assertEquals(-3.5f, msg.band.gain, 0.01f)
        assertEquals(1.41f, msg.band.q, 0.01f)
    }
}
