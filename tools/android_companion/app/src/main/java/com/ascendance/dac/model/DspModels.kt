package com.ascendance.dac.model

import kotlinx.serialization.Serializable
import kotlin.math.pow
import kotlin.math.roundToInt

enum class FilterType(val code: Int, val label: String, val shortLabel: String, val usesGain: Boolean) {
    BYPASS(0, "Bypass", "BYP", false),
    PEAKING(1, "Peaking", "PK", true),
    LOW_SHELF(2, "Low Shelf", "L-SH", true),
    HIGH_SHELF(3, "High Shelf", "H-SH", true),
    HIGH_PASS(4, "High Pass", "HPF", false),
    LOW_PASS(5, "Low Pass", "LPF", false),
    NOTCH(6, "Notch", "NTCH", false),
    BAND_PASS(7, "Band Pass", "BPF", false);

    companion object {
        fun fromCode(code: Int): FilterType =
            entries.firstOrNull { it.code == code } ?: BYPASS
    }
}

enum class PeqChannel(val code: Int, val label: String) {
    LINKED(0, "Linked"),
    LEFT(1, "Left"),
    RIGHT(2, "Right");

    companion object {
        fun fromCode(code: Int): PeqChannel =
            entries.firstOrNull { it.code == code } ?: LINKED
    }
}

enum class MixerMode(val code: Int, val label: String) {
    STEREO(0, "Normal Stereo"),
    MONO(1, "Mono Summed (Downmix)"),
    SWAP(2, "Channel Swap (L <-> R)"),
    WIDTH(3, "M/S Stereo Width"),
    CROSSFEED(4, "Headphone Crossfeed (Chu Moy)"),
    CUSTOM_MATRIX(5, "Custom Channel Matrix");

    companion object {
        fun fromCode(code: Int): MixerMode =
            entries.firstOrNull { it.code == code } ?: STEREO
    }
}

enum class LoudnessMode(val code: Int, val label: String) {
    OFF(0, "Off"),
    MILD(1, "ISO Mild"),
    FULL(2, "ISO Full");

    companion object {
        fun fromCode(code: Int): LoudnessMode =
            entries.firstOrNull { it.code == code } ?: OFF
    }
}

enum class OledViewMode(val code: Int, val label: String) {
    HUD(0, "Classic"),
    CURVE(1, "EQ Curve"),
    VU(2, "VU Meters"),
    SPLIT(3, "Split");

    companion object {
        fun fromCode(code: Int): OledViewMode =
            entries.firstOrNull { it.code == code } ?: HUD
    }
}

enum class ExciterMode(val code: Int, val label: String) {
    TUBE(0, "Tube Warmth"),
    TAPE(1, "Tape"),
    AIR(2, "Air Sheen");

    companion object {
        fun fromCode(code: Int): ExciterMode =
            entries.firstOrNull { it.code == code } ?: TUBE
    }
}

@Serializable
data class DynamicsConfig(
    val limiterEnabled: Boolean = true,
    val autoPreampEnabled: Boolean = false,
    val autoPreampDb: Float = 0.0f,
    val loudnessMode: Int = LoudnessMode.OFF.code,
    val firEnabled: Boolean = false,
    val oledView: Int = OledViewMode.HUD.code,
    val autoMuteSec: Int = 10,
    val oledDimSec: Int = 60,
    val firTapsL: Int = 0,
    val firTapsR: Int = 0,
    val firMaxTaps: Int = 256
)

@Serializable
data class HarmonicsConfig(
    val bassEnabled: Boolean = false,
    val bassAmountPct: Int = 35,
    val bassFreqHz: Int = 80,
    val exciterEnabled: Boolean = false,
    val exciterMode: Int = ExciterMode.TUBE.code,
    val exciterAmountPct: Int = 25,
    val exciterFreqHz: Int = 3500
) {
    val exciterModeEnum: ExciterMode
        get() = ExciterMode.fromCode(exciterMode)

    fun clamped(): HarmonicsConfig = copy(
        bassAmountPct = bassAmountPct.coerceIn(
            DspDefaults.MIN_HARMONICS_AMOUNT_PCT,
            DspDefaults.MAX_HARMONICS_AMOUNT_PCT
        ),
        bassFreqHz = bassFreqHz.coerceIn(
            DspDefaults.MIN_BASS_FREQ_HZ,
            DspDefaults.MAX_BASS_FREQ_HZ
        ),
        exciterMode = ExciterMode.fromCode(exciterMode).code,
        exciterAmountPct = exciterAmountPct.coerceIn(
            DspDefaults.MIN_HARMONICS_AMOUNT_PCT,
            DspDefaults.MAX_HARMONICS_AMOUNT_PCT
        ),
        exciterFreqHz = exciterFreqHz.coerceIn(
            DspDefaults.MIN_EXCITER_FREQ_HZ,
            DspDefaults.MAX_EXCITER_FREQ_HZ
        )
    )
}

@Serializable
data class DspProConfig(
    val tpdfDitherEnabled: Boolean = true,
    val subsonicFilterEnabled: Boolean = false,
    val volTaperLogEnabled: Boolean = false,
    val dynamicBassEnabled: Boolean = false,
    val dynamicBassAmountPct: Int = 40,
    val autoPreampEnabled: Boolean = false
) {
    fun clamped(): DspProConfig = copy(
        dynamicBassAmountPct = dynamicBassAmountPct.coerceIn(0, 100)
    )
}

@Serializable
data class PeqBand(
    val type: Int = FilterType.PEAKING.code,
    val freq: Int = 1000,
    val gain: Float = 0.0f,
    val q: Float = 0.71f
) {
    val filterType: FilterType
        get() = FilterType.fromCode(type)

    fun clamped(): PeqBand = copy(
        type = FilterType.fromCode(type).code,
        freq = freq.coerceIn(DspDefaults.MIN_FREQ_HZ, DspDefaults.MAX_FREQ_HZ),
        gain = gain.coerceIn(DspDefaults.MIN_GAIN_DB, DspDefaults.MAX_GAIN_DB),
        q = q.coerceIn(DspDefaults.MIN_Q, DspDefaults.MAX_Q)
    )
}

@Serializable
data class MixerConfig(
    val mode: Int = MixerMode.STEREO.code,
    val balance: Int = 0,
    val widthPct: Int = 100,
    val crossfeedLevel: Int = 0,
    val customMatrix: List<Float> = listOf(1.0f, 0.0f, 0.0f, 1.0f)
) {
    val mixerMode: MixerMode
        get() = MixerMode.fromCode(mode)

    fun sanitized(): MixerConfig {
        val mat = (0 until 4).map { idx ->
            (customMatrix.getOrNull(idx) ?: if (idx == 0 || idx == 3) 1.0f else 0.0f)
                .takeIf { it.isFinite() }
                ?.coerceIn(-4.0f, 4.0f)
                ?: if (idx == 0 || idx == 3) 1.0f else 0.0f
        }
        return copy(
            mode = MixerMode.fromCode(mode).code,
            balance = balance.coerceIn(-100, 100),
            widthPct = widthPct.coerceIn(0, 200),
            crossfeedLevel = crossfeedLevel.coerceIn(0, 3),
            customMatrix = mat
        )
    }
}

@Serializable
data class LocalPreset(
    val id: String,
    val name: String,
    val updatedAtEpochMs: Long,
    val linkedStereo: Boolean = true,
    val preampDb: Float = 0.0f,
    val leftBands: List<PeqBand> = DspDefaults.createDefaultBands(5),
    val rightBands: List<PeqBand> = DspDefaults.createDefaultBands(5),
    val mixer: MixerConfig = MixerConfig()
)

object DspDefaults {
    const val USER_SLOT_COUNT = 8
    const val MIN_BANDS = 1
    const val MAX_BANDS = 10
    const val MIN_FREQ_HZ = 20
    const val MAX_FREQ_HZ = 20_000
    const val MIN_GAIN_DB = -24.0f
    const val MAX_GAIN_DB = 12.0f
    const val MIN_Q = 0.10f
    const val MAX_Q = 10.00f
    const val MIN_PREAMP_DB = -12.0f
    const val MAX_PREAMP_DB = 0.0f
    const val MIN_HARMONICS_AMOUNT_PCT = 0
    const val MAX_HARMONICS_AMOUNT_PCT = 100
    const val MIN_BASS_FREQ_HZ = 50
    const val MAX_BASS_FREQ_HZ = 150
    const val MIN_EXCITER_FREQ_HZ = 1500
    const val MAX_EXCITER_FREQ_HZ = 8000

    val SUPPORTED_SAMPLE_RATES = listOf(44100, 48000, 96000)

    fun defaultSlotNames(): List<String> = List(USER_SLOT_COUNT) { "Slot ${it + 1}" }

    fun formatSlotLabel(index: Int, rawName: String?): String {
        val defName = "Slot ${index + 1}"
        val clean = rawName?.trim().orEmpty().ifEmpty { defName }
        return if (clean == defName) clean else "${index + 1}: $clean"
    }

    val BAND_COLORS_ARGB = listOf(
        0xFF58A6FFL, 0xFF3FB950L, 0xFFD29922L, 0xFFF85149L, 0xFFBC8CFFL,
        0xFF39C5BBL, 0xFFF0883EL, 0xFF79C0FFL, 0xFF56D364L, 0xFFE3B341L
    )

    private val ISO_FREQS = mapOf(
        1 to listOf(1000),
        2 to listOf(150, 4000),
        3 to listOf(100, 1000, 10000),
        4 to listOf(100, 500, 2500, 10000),
        5 to listOf(80, 250, 1000, 4000, 10000),
        6 to listOf(80, 200, 800, 2500, 6300, 14000),
        7 to listOf(63, 160, 400, 1000, 2500, 6300, 16000),
        8 to listOf(40, 100, 250, 630, 1600, 4000, 10000, 16000),
        9 to listOf(31, 63, 160, 400, 1000, 2500, 5000, 10000, 16000),
        10 to listOf(31, 63, 125, 250, 500, 1000, 2000, 4000, 8000, 16000)
    )

    fun createDefaultBands(count: Int): List<PeqBand> {
        val safeCount = count.coerceIn(MIN_BANDS, MAX_BANDS)
        val freqs = ISO_FREQS[safeCount] ?: ISO_FREQS.getValue(5)
        return List(safeCount) { i ->
            val type = when (i) {
                0 -> FilterType.LOW_SHELF.code
                safeCount - 1 -> FilterType.HIGH_SHELF.code
                else -> FilterType.PEAKING.code
            }
            val q = if (type == FilterType.PEAKING.code) 1.00f else 0.71f
            PeqBand(
                type = type,
                freq = freqs.getOrElse(i) { (100.0 * 2.0.pow(i)).roundToInt() },
                gain = 0.0f,
                q = q
            )
        }
    }

    fun suggestNextFrequency(existing: List<PeqBand>): Int {
        if (existing.isEmpty()) return 1000
        val sorted = existing.map { it.freq.toDouble() }.sorted()
        var bestFreq = 1000.0
        var maxRatio = 1.0
        val edges = listOf(20.0) + sorted + listOf(20000.0)
        for (i in 0 until edges.lastIndex) {
            val ratio = edges[i + 1] / edges[i]
            if (ratio > maxRatio) {
                maxRatio = ratio
                bestFreq = kotlin.math.sqrt(edges[i] * edges[i + 1])
            }
        }
        return bestFreq.roundToInt().coerceIn(MIN_FREQ_HZ, MAX_FREQ_HZ)
    }
}
