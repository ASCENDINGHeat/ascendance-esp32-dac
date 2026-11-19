package com.ascendance.dac

import com.ascendance.dac.model.DspDefaults
import com.ascendance.dac.model.DynamicsConfig
import com.ascendance.dac.model.ExciterMode
import com.ascendance.dac.model.FilterType
import com.ascendance.dac.model.HarmonicsConfig
import com.ascendance.dac.model.LoudnessMode
import com.ascendance.dac.model.MixerConfig
import com.ascendance.dac.model.MixerMode
import com.ascendance.dac.model.OledViewMode
import com.ascendance.dac.model.PeqBand
import com.ascendance.dac.model.PeqChannel
import com.ascendance.dac.usb.HidIncomingMessage
import com.ascendance.dac.usb.PeqPacketCodec
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
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
            put(15, 6) // Slot 7 (0..7)
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
        assertEquals(6, msg.activePresetSlot)
        assertEquals(-4.0f, msg.preampDb, 0.01f)
        assertEquals(-25, msg.mixerBalance)
        assertEquals(96000, msg.sampleRateHz)
        assertEquals(-3.5f, msg.band.gain, 0.01f)
        assertEquals(1.41f, msg.band.q, 0.01f)
    }

    @Test
    fun encodesV020SlotDynamicsPowerFirAndDecodesExtState() {
        val selPkt = PeqPacketCodec.buildSelectProfile(7)
        assertEquals(7, selPkt[3].toInt() and 0xFF)

        val renamePkt = PeqPacketCodec.buildRenameSlot(5, "HD600 Harman")
        assertEquals(PeqPacketCodec.CMD_RENAME_SLOT, renamePkt[2].toInt() and 0xFF)
        assertEquals(5, renamePkt[3].toInt() and 0xFF)
        assertEquals('H'.code.toByte(), renamePkt[12])

        val dynPkt = PeqPacketCodec.buildSetDynamics(
            DynamicsConfig(
                limiterEnabled = true,
                loudnessMode = LoudnessMode.FULL.code,
                firEnabled = true,
                oledView = OledViewMode.SPLIT.code
            )
        )
        assertEquals(PeqPacketCodec.CMD_SET_DYNAMICS, dynPkt[2].toInt() and 0xFF)
        assertEquals(OledViewMode.SPLIT.code, dynPkt[3].toInt() and 0xFF)
        assertEquals(LoudnessMode.FULL.code, dynPkt[4].toInt() and 0xFF)
        assertEquals(0x03, dynPkt[5].toInt() and 0xFF)

        val pwrPkt = ByteBuffer.wrap(PeqPacketCodec.buildSetPowerConfig(30, 120)).order(ByteOrder.LITTLE_ENDIAN)
        assertEquals(30, pwrPkt.getShort(6).toInt() and 0xFFFF)
        assertEquals(120, pwrPkt.getShort(10).toInt() and 0xFFFF)

        val taps = FloatArray(14) { i -> i * 0.05f }
        val chunks = PeqPacketCodec.buildUploadFirChunks(PeqChannel.LEFT, taps, commitNvs = true)
        assertEquals(2, chunks.size)
        assertEquals(12, chunks[0][4].toInt() and 0xFF)
        assertEquals(0, chunks[0][5].toInt() and 0xFF)
        assertEquals(2, chunks[1][4].toInt() and 0xFF)
        assertEquals(1, chunks[1][5].toInt() and 0xFF)

        val extRaw = ByteBuffer.allocate(64).order(ByteOrder.LITTLE_ENDIAN).apply {
            putShort(0, 0x4551)
            put(2, PeqPacketCodec.CMD_SET_DYNAMICS.toByte())
            put(3, PeqPacketCodec.BAND_IDX_EXT_STATE_QUERY.toByte())
            put(4, 5)
            put(5, (0x01 or (2 shl 1) or 0x08 or (3 shl 4)).toByte())
            putShort(6, 15)
            putShort(8, 90)
            putShort(10, 256)
            "HD600 Harman".toByteArray(Charsets.UTF_8).forEachIndexed { idx, b -> put(12 + idx, b) }
            putShort(28, 128)
            putShort(30, 256)
        }.array()

        val extMsg = PeqPacketCodec.parseReport(extRaw) as? HidIncomingMessage.ExtendedStateReport
        assertNotNull(extMsg)
        assertEquals(5, extMsg!!.activeSlot)
        assertEquals("HD600 Harman", extMsg.slotName)
        assertTrue(extMsg.dynamics.limiterEnabled)
        assertEquals(2, extMsg.dynamics.loudnessMode)
        assertTrue(extMsg.dynamics.firEnabled)
        assertEquals(3, extMsg.dynamics.oledView)
        assertEquals(15, extMsg.dynamics.autoMuteSec)
        assertEquals(90, extMsg.dynamics.oledDimSec)
        assertEquals(256, extMsg.dynamics.firTapsL)
        assertEquals(128, extMsg.dynamics.firTapsR)
    }

    @Test
    fun formatsDefaultAndCustomSlotLabels() {
        val defaults = DspDefaults.defaultSlotNames()
        assertEquals(8, defaults.size)
        assertEquals("Slot 1", defaults[0])
        assertEquals("Slot 8", defaults[7])
        assertEquals("Slot 1", DspDefaults.formatSlotLabel(0, "Slot 1"))
        assertEquals("Slot 2", DspDefaults.formatSlotLabel(1, ""))
        assertEquals("1: HD600", DspDefaults.formatSlotLabel(0, "HD600"))
        assertEquals("3: Bass", DspDefaults.formatSlotLabel(2, " Bass "))
    }

    @Test
    fun encodesHarmonicsGoldenVector() {
        val pkt = PeqPacketCodec.buildSetHarmonics(
            HarmonicsConfig(
                bassEnabled = true,
                bassAmountPct = 60,
                bassFreqHz = 90,
                exciterEnabled = true,
                exciterMode = ExciterMode.TAPE.code,
                exciterAmountPct = 35,
                exciterFreqHz = 4500
            )
        )
        assertEquals(64, pkt.size)
        val expected = ByteArray(64)
        val header = byteArrayOf(
            0x51,
            0x45,
            0x0E,
            0x07,
            0x3C,
            0x23,
            0x5A,
            0x00,
            0x00,
            0x00,
            0x94.toByte(),
            0x11
        )
        System.arraycopy(header, 0, expected, 0, header.size)
        assertArrayEquals(expected, pkt)
    }

    @Test
    fun clampsOutOfRangeHarmonicsInputs() {
        val lowPkt = PeqPacketCodec.buildSetHarmonics(
            HarmonicsConfig(
                bassEnabled = false,
                bassAmountPct = -25,
                bassFreqHz = 10,
                exciterEnabled = false,
                exciterMode = 99,
                exciterAmountPct = -5,
                exciterFreqHz = 400
            )
        )
        val lowBb = ByteBuffer.wrap(lowPkt).order(ByteOrder.LITTLE_ENDIAN)
        assertEquals(0x4551, lowBb.getShort(0).toInt() and 0xFFFF)
        assertEquals(PeqPacketCodec.CMD_SET_HARMONICS, lowBb.get(2).toInt() and 0xFF)
        assertEquals(0x00, lowBb.get(3).toInt() and 0xFF)
        assertEquals(0, lowBb.get(4).toInt() and 0xFF)
        assertEquals(0, lowBb.get(5).toInt() and 0xFF)
        assertEquals(50, lowBb.getShort(6).toInt() and 0xFFFF)
        assertEquals(1500, lowBb.getShort(10).toInt() and 0xFFFF)

        val highPkt = PeqPacketCodec.buildSetHarmonics(
            HarmonicsConfig(
                bassEnabled = true,
                bassAmountPct = 250,
                bassFreqHz = 400,
                exciterEnabled = true,
                exciterMode = ExciterMode.AIR.code,
                exciterAmountPct = 180,
                exciterFreqHz = 16000
            )
        )
        val highBb = ByteBuffer.wrap(highPkt).order(ByteOrder.LITTLE_ENDIAN)
        assertEquals(0x0B, highBb.get(3).toInt() and 0xFF)
        assertEquals(100, highBb.get(4).toInt() and 0xFF)
        assertEquals(100, highBb.get(5).toInt() and 0xFF)
        assertEquals(150, highBb.getShort(6).toInt() and 0xFFFF)
        assertEquals(8000, highBb.getShort(10).toInt() and 0xFFFF)
    }

    @Test
    fun decodesExtendedStateReportHarmonicsRoundTrip() {
        val goldenRaw = ByteBuffer.allocate(64).order(ByteOrder.LITTLE_ENDIAN).apply {
            putShort(0, 0x4551)
            put(2, PeqPacketCodec.CMD_SET_DYNAMICS.toByte())
            put(3, PeqPacketCodec.BAND_IDX_EXT_STATE_QUERY.toByte())
            put(4, 2)
            put(5, 0x01)
            putShort(6, 10)
            putShort(8, 60)
            putShort(10, 0)
            "Slot 3".toByteArray(Charsets.UTF_8).forEachIndexed { idx, b -> put(12 + idx, b) }
            putShort(28, 0)
            putShort(30, 256)
            // reserved[20..27] at packet bytes 32..39
            put(32, 0x07) // bass=1, exciter=1, mode=TAPE(1)
            put(33, 60)
            put(34, 35)
            putShort(35, 90)
            putShort(37, 4500)
            put(39, 0)
        }.array()

        val msg = PeqPacketCodec.parseReport(goldenRaw) as? HidIncomingMessage.ExtendedStateReport
        assertNotNull(msg)
        assertTrue(msg!!.harmonics.bassEnabled)
        assertEquals(60, msg.harmonics.bassAmountPct)
        assertEquals(90, msg.harmonics.bassFreqHz)
        assertTrue(msg.harmonics.exciterEnabled)
        assertEquals(ExciterMode.TAPE.code, msg.harmonics.exciterMode)
        assertEquals(ExciterMode.TAPE, msg.harmonics.exciterModeEnum)
        assertEquals(35, msg.harmonics.exciterAmountPct)
        assertEquals(4500, msg.harmonics.exciterFreqHz)

        val clampedRaw = ByteBuffer.allocate(64).order(ByteOrder.LITTLE_ENDIAN).apply {
            putShort(0, 0x4551)
            put(2, PeqPacketCodec.CMD_SET_DYNAMICS.toByte())
            put(3, PeqPacketCodec.BAND_IDX_EXT_STATE_QUERY.toByte())
            put(4, 0)
            put(5, 0x01)
            putShort(30, 256)
            // reserved[20..27]: bass off, exciter on, mode AIR(2), out-of-range amounts/freqs
            put(32, (0x02 or (ExciterMode.AIR.code shl 2)).toByte())
            put(33, 200.toByte())
            put(34, 150.toByte())
            putShort(35, 20)
            putShort(37, 12000)
            put(39, 0)
        }.array()

        val clampedMsg = PeqPacketCodec.parseReport(clampedRaw) as? HidIncomingMessage.ExtendedStateReport
        assertNotNull(clampedMsg)
        assertFalse(clampedMsg!!.harmonics.bassEnabled)
        assertEquals(100, clampedMsg.harmonics.bassAmountPct)
        assertEquals(50, clampedMsg.harmonics.bassFreqHz)
        assertTrue(clampedMsg.harmonics.exciterEnabled)
        assertEquals(ExciterMode.AIR.code, clampedMsg.harmonics.exciterMode)
        assertEquals(100, clampedMsg.harmonics.exciterAmountPct)
        assertEquals(8000, clampedMsg.harmonics.exciterFreqHz)
    }
}
