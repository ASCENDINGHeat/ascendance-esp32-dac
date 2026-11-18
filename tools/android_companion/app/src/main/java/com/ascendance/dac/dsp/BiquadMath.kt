package com.ascendance.dac.dsp

import com.ascendance.dac.model.DspDefaults
import com.ascendance.dac.model.FilterType
import com.ascendance.dac.model.PeqBand
import kotlin.math.PI
import kotlin.math.abs
import kotlin.math.atan2
import kotlin.math.cos
import kotlin.math.ln
import kotlin.math.log10
import kotlin.math.max
import kotlin.math.pow
import kotlin.math.sin
import kotlin.math.sqrt

data class BiquadCoeffs(
    val b0: Double,
    val b1: Double,
    val b2: Double,
    val a1: Double,
    val a2: Double
) {
    companion object {
        val PASSTHROUGH = BiquadCoeffs(1.0, 0.0, 0.0, 0.0, 0.0)
    }
}

data class FreqEvalResult(
    val magDb: Double,
    val phaseRad: Double
)

data class CurvePoint(
    val freqHz: Double,
    val magDb: Double,
    val phaseDeg: Double,
    val groupDelayMs: Double
)

object BiquadMath {

    fun calcBiquadCoeffs(band: PeqBand, sampleRateHz: Int): BiquadCoeffs {
        val filterType = band.filterType
        if (filterType == FilterType.BYPASS) return BiquadCoeffs.PASSTHROUGH
        if (filterType.usesGain && abs(band.gain) < 0.05f) return BiquadCoeffs.PASSTHROUGH

        val fs = sampleRateHz.coerceIn(8000, 192000).toDouble()
        val f0 = band.freq.toDouble().coerceIn(10.0, fs * 0.49)
        val q = band.q.toDouble().coerceIn(0.05, 10.0)
        val g = band.gain.toDouble().coerceIn(DspDefaults.MIN_GAIN_DB.toDouble(), DspDefaults.MAX_GAIN_DB.toDouble())

        val omega = (2.0 * PI * f0) / fs
        val cosW = cos(omega)
        val sinW = sin(omega)
        val alpha = sinW / (2.0 * q)
        val aAmp = 10.0.pow(g / 40.0)
        val sqrtA = sqrt(aAmp)

        var b0 = 1.0
        var b1 = 0.0
        var b2 = 0.0
        var a0 = 1.0
        var a1 = 0.0
        var a2 = 0.0

        when (filterType) {
            FilterType.BYPASS -> Unit
            FilterType.PEAKING -> {
                b0 = 1.0 + alpha * aAmp
                b1 = -2.0 * cosW
                b2 = 1.0 - alpha * aAmp
                a0 = 1.0 + alpha / aAmp
                a1 = -2.0 * cosW
                a2 = 1.0 - alpha / aAmp
            }
            FilterType.LOW_SHELF -> {
                b0 = aAmp * ((aAmp + 1.0) - (aAmp - 1.0) * cosW + 2.0 * sqrtA * alpha)
                b1 = 2.0 * aAmp * ((aAmp - 1.0) - (aAmp + 1.0) * cosW)
                b2 = aAmp * ((aAmp + 1.0) - (aAmp - 1.0) * cosW - 2.0 * sqrtA * alpha)
                a0 = (aAmp + 1.0) + (aAmp - 1.0) * cosW + 2.0 * sqrtA * alpha
                a1 = -2.0 * ((aAmp - 1.0) + (aAmp + 1.0) * cosW)
                a2 = (aAmp + 1.0) + (aAmp - 1.0) * cosW - 2.0 * sqrtA * alpha
            }
            FilterType.HIGH_SHELF -> {
                b0 = aAmp * ((aAmp + 1.0) + (aAmp - 1.0) * cosW + 2.0 * sqrtA * alpha)
                b1 = -2.0 * aAmp * ((aAmp - 1.0) + (aAmp + 1.0) * cosW)
                b2 = aAmp * ((aAmp + 1.0) + (aAmp - 1.0) * cosW - 2.0 * sqrtA * alpha)
                a0 = (aAmp + 1.0) - (aAmp - 1.0) * cosW + 2.0 * sqrtA * alpha
                a1 = 2.0 * ((aAmp - 1.0) - (aAmp + 1.0) * cosW)
                a2 = (aAmp + 1.0) - (aAmp - 1.0) * cosW - 2.0 * sqrtA * alpha
            }
            FilterType.HIGH_PASS -> {
                b0 = (1.0 + cosW) * 0.5
                b1 = -(1.0 + cosW)
                b2 = (1.0 + cosW) * 0.5
                a0 = 1.0 + alpha
                a1 = -2.0 * cosW
                a2 = 1.0 - alpha
            }
            FilterType.LOW_PASS -> {
                b0 = (1.0 - cosW) * 0.5
                b1 = 1.0 - cosW
                b2 = (1.0 - cosW) * 0.5
                a0 = 1.0 + alpha
                a1 = -2.0 * cosW
                a2 = 1.0 - alpha
            }
            FilterType.NOTCH -> {
                b0 = 1.0
                b1 = -2.0 * cosW
                b2 = 1.0
                a0 = 1.0 + alpha
                a1 = -2.0 * cosW
                a2 = 1.0 - alpha
            }
            FilterType.BAND_PASS -> {
                b0 = alpha
                b1 = 0.0
                b2 = -alpha
                a0 = 1.0 + alpha
                a1 = -2.0 * cosW
                a2 = 1.0 - alpha
            }
        }

        if (!a0.isFinite() || abs(a0) < 1e-12) {
            return BiquadCoeffs.PASSTHROUGH
        }
        return BiquadCoeffs(b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0)
    }

    fun evalCascadeAtFreq(
        bands: List<PeqBand>,
        freqHz: Double,
        sampleRateHz: Int
    ): FreqEvalResult {
        val coeffs = bands.map { calcBiquadCoeffs(it, sampleRateHz) }
        return evalCoeffsAtFreq(coeffs, freqHz, sampleRateHz)
    }

    fun evalCoeffsAtFreq(
        coeffs: List<BiquadCoeffs>,
        freqHz: Double,
        sampleRateHz: Int
    ): FreqEvalResult {
        val fs = sampleRateHz.coerceIn(8000, 192000).toDouble()
        val w = (2.0 * PI * freqHz) / fs
        val c1 = cos(w)
        val s1 = -sin(w)
        val c2 = cos(2.0 * w)
        val s2 = -sin(2.0 * w)

        var re = 1.0
        var im = 0.0
        var phaseSum = 0.0

        for (c in coeffs) {
            val nr = c.b0 + c.b1 * c1 + c.b2 * c2
            val ni = c.b1 * s1 + c.b2 * s2
            val dr = 1.0 + c.a1 * c1 + c.a2 * c2
            val di = c.a1 * s1 + c.a2 * s2
            val den = dr * dr + di * di
            if (den < 1e-24) continue

            val hr = (nr * dr + ni * di) / den
            val hi = (ni * dr - nr * di) / den
            val newRe = re * hr - im * hi
            val newIm = re * hi + im * hr
            re = newRe
            im = newIm
            phaseSum += atan2(hi, hr)
        }

        val mag = sqrt(re * re + im * im)
        return FreqEvalResult(
            magDb = 20.0 * log10(max(mag, 1e-7)),
            phaseRad = phaseSum
        )
    }

    fun sampleCurve(
        bands: List<PeqBand>,
        preampDb: Float,
        abBypass: Boolean,
        sampleRateHz: Int,
        steps: Int = 180
    ): List<CurvePoint> {
        val effectiveBands = if (abBypass) emptyList() else bands
        val coeffs = effectiveBands.map { calcBiquadCoeffs(it, sampleRateHz) }
        val logMin = ln(20.0)
        val logMax = ln(20000.0)
        val points = ArrayList<CurvePoint>(steps + 1)
        var prevPhase = 0.0
        var prevOmega = 0.0

        for (i in 0..steps) {
            val freq = kotlin.math.exp(logMin + (i.toDouble() / steps) * (logMax - logMin))
            val res = evalCoeffsAtFreq(coeffs, freq, sampleRateHz)
            val totalDb = res.magDb + preampDb.toDouble()
            val omega = 2.0 * PI * freq
            val gdMs = if (i > 0 && omega > prevOmega) {
                var dPhase = res.phaseRad - prevPhase
                while (dPhase > PI) dPhase -= 2.0 * PI
                while (dPhase < -PI) dPhase += 2.0 * PI
                ((-dPhase / (omega - prevOmega)) * 1000.0).coerceIn(-10.0, 20.0)
            } else {
                0.0
            }
            prevPhase = res.phaseRad
            prevOmega = omega

            var wrappedDeg = Math.toDegrees(res.phaseRad)
            while (wrappedDeg > 180.0) wrappedDeg -= 360.0
            while (wrappedDeg < -180.0) wrappedDeg += 360.0

            points.add(
                CurvePoint(
                    freqHz = freq,
                    magDb = totalDb,
                    phaseDeg = wrappedDeg,
                    groupDelayMs = gdMs
                )
            )
        }
        return points
    }
}
