package com.ascendance.dac

import com.ascendance.dac.dsp.BiquadMath
import com.ascendance.dac.model.FilterType
import com.ascendance.dac.model.PeqBand
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class BiquadMathTest {

    @Test
    fun allEightTopologiesEvaluateExpectedResponseAt48kAnd96k() {
        val sampleRates = listOf(44100, 48000, 96000)
        for (fs in sampleRates) {
            // 0: Bypass
            val byp = BiquadMath.evalCascadeAtFreq(
                listOf(PeqBand(FilterType.BYPASS.code, 1000, 9.0f, 1.0f)), 1000.0, fs
            )
            assertEquals(0.0, byp.magDb, 0.01)

            // 1: Peaking +6 dB @ 1000 Hz
            val pk = BiquadMath.evalCascadeAtFreq(
                listOf(PeqBand(FilterType.PEAKING.code, 1000, 6.0f, 2.0f)), 1000.0, fs
            )
            assertEquals(6.0, pk.magDb, 0.05)

            // 2: Low Shelf +6 dB @ 200 Hz (at 20 Hz)
            val ls = BiquadMath.evalCascadeAtFreq(
                listOf(PeqBand(FilterType.LOW_SHELF.code, 200, 6.0f, 0.71f)), 20.0, fs
            )
            assertEquals(6.0, ls.magDb, 0.15)

            // 3: High Shelf -6 dB @ 2000 Hz (at 16 kHz)
            val hs = BiquadMath.evalCascadeAtFreq(
                listOf(PeqBand(FilterType.HIGH_SHELF.code, 2000, -6.0f, 0.71f)), 16000.0, fs
            )
            assertEquals(-6.0, hs.magDb, 0.25)

            // 4: High Pass @ 1000 Hz Q=0.7071 -> -3.01 dB at Fc, heavily cut at 50 Hz
            val hpfFc = BiquadMath.evalCascadeAtFreq(
                listOf(PeqBand(FilterType.HIGH_PASS.code, 1000, 0f, 0.7071f)), 1000.0, fs
            )
            assertEquals(-3.01, hpfFc.magDb, 0.1)
            val hpfLow = BiquadMath.evalCascadeAtFreq(
                listOf(PeqBand(FilterType.HIGH_PASS.code, 1000, 0f, 0.7071f)), 50.0, fs
            )
            assertTrue(hpfLow.magDb < -50.0)

            // 5: Low Pass @ 1000 Hz Q=0.7071 -> -3.01 dB at Fc, heavily cut at 15 kHz
            val lpfFc = BiquadMath.evalCascadeAtFreq(
                listOf(PeqBand(FilterType.LOW_PASS.code, 1000, 0f, 0.7071f)), 1000.0, fs
            )
            assertEquals(-3.01, lpfFc.magDb, 0.1)
            val lpfHigh = BiquadMath.evalCascadeAtFreq(
                listOf(PeqBand(FilterType.LOW_PASS.code, 1000, 0f, 0.7071f)), 15000.0, fs
            )
            assertTrue(lpfHigh.magDb < -40.0)

            // 6: Notch @ 1000 Hz
            val notchCenter = BiquadMath.evalCascadeAtFreq(
                listOf(PeqBand(FilterType.NOTCH.code, 1000, 0f, 5.0f)), 1000.0, fs
            )
            assertTrue(notchCenter.magDb < -60.0)

            // 7: Band Pass @ 1000 Hz
            val bpfCenter = BiquadMath.evalCascadeAtFreq(
                listOf(PeqBand(FilterType.BAND_PASS.code, 1000, 0f, 2.0f)), 1000.0, fs
            )
            assertEquals(0.0, bpfCenter.magDb, 0.05)
            val bpfLow = BiquadMath.evalCascadeAtFreq(
                listOf(PeqBand(FilterType.BAND_PASS.code, 1000, 0f, 2.0f)), 100.0, fs
            )
            assertTrue(bpfLow.magDb < -20.0)
        }
    }

    @Test
    fun abBypassFlattensEqWhileKeepingPreampOffset() {
        val bands = listOf(PeqBand(FilterType.PEAKING.code, 1000, 8.0f, 1.5f))
        val pts = BiquadMath.sampleCurve(bands, preampDb = -4.0f, abBypass = true, sampleRateHz = 48000, steps = 20)
        for (pt in pts) {
            assertEquals(-4.0, pt.magDb, 0.01)
            assertEquals(0.0, pt.phaseDeg, 0.01)
        }
    }
}
