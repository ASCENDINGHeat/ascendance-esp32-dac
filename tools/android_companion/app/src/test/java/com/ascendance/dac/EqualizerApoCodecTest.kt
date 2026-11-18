package com.ascendance.dac

import com.ascendance.dac.dsp.EqualizerApoCodec
import com.ascendance.dac.dsp.WebHidSnapshotJson
import com.ascendance.dac.model.FilterType
import com.ascendance.dac.model.MixerConfig
import com.ascendance.dac.model.MixerMode
import com.ascendance.dac.model.PeqBand
import org.junit.Assert.assertEquals
import org.junit.Test

class EqualizerApoCodecTest {

    @Test
    fun parseAndExportApoRoundTrip() {
        val sample = """
            # AutoEQ Test Profile
            Preamp: -4.5 dB
            Filter 1: ON LSC Fc 105 Hz Gain 5.5 dB Q 0.70
            Filter 2: ON PK Fc 1000 Hz Gain -3.2 dB Q 2.10
            Filter 3: ON HPQ Fc 35 Hz Q 0.71
            Filter 4: ON NO Fc 6000 Hz Q 8.00
            Filter 5: OFF PK Fc 250 Hz Gain 1.0 dB Q 1.00
        """.trimIndent()

        val parsed = EqualizerApoCodec.parseEqualizerApoText(sample)
        assertEquals(-4.5f, parsed.preampDb, 0.01f)
        assertEquals(5, parsed.bands.size)
        assertEquals(FilterType.LOW_SHELF.code, parsed.bands[0].type)
        assertEquals(FilterType.PEAKING.code, parsed.bands[1].type)
        assertEquals(FilterType.HIGH_PASS.code, parsed.bands[2].type)
        assertEquals(FilterType.NOTCH.code, parsed.bands[3].type)
        assertEquals(FilterType.BYPASS.code, parsed.bands[4].type)

        val exported = EqualizerApoCodec.exportEqualizerApoText(parsed.preampDb, parsed.bands)
        val reparsed = EqualizerApoCodec.parseEqualizerApoText(exported)
        assertEquals(parsed.preampDb, reparsed.preampDb, 0.01f)
        assertEquals(parsed.bands.size, reparsed.bands.size)
        assertEquals(parsed.bands[1].freq, reparsed.bands[1].freq)
        assertEquals(parsed.bands[1].gain, reparsed.bands[1].gain, 0.05f)
    }

    @Test
    fun jsonSnapshotRoundTrip() {
        val snap = WebHidSnapshotJson(
            version = 2,
            linkedStereo = false,
            preampDb = -3.5f,
            leftBands = listOf(PeqBand(FilterType.PEAKING.code, 1200, 4.0f, 1.4f)),
            rightBands = listOf(PeqBand(FilterType.HIGH_PASS.code, 45, 0.0f, 0.71f)),
            mixer = MixerConfig(mode = MixerMode.CROSSFEED.code, balance = -10, widthPct = 120, crossfeedLevel = 2)
        )
        val raw = EqualizerApoCodec.exportSnapshotJson(snap)
        val restored = EqualizerApoCodec.importSnapshotJson(raw)
        assertEquals(false, restored.linkedStereo)
        assertEquals(-3.5f, restored.preampDb, 0.01f)
        assertEquals(1200, restored.leftBands[0].freq)
        assertEquals(FilterType.HIGH_PASS.code, restored.rightBands[0].type)
        assertEquals(2, restored.mixer.crossfeedLevel)
    }
}
