package com.ascendance.dac.dsp

import com.ascendance.dac.model.DspDefaults
import com.ascendance.dac.model.FilterType
import com.ascendance.dac.model.MixerConfig
import com.ascendance.dac.model.PeqBand
import kotlinx.serialization.Serializable
import kotlinx.serialization.encodeToString
import kotlinx.serialization.json.Json
import kotlin.math.roundToInt

data class ApoParseResult(
    val preampDb: Float,
    val bands: List<PeqBand>
)

@Serializable
data class WebHidSnapshotJson(
    val version: Int = 2,
    val linkedStereo: Boolean = true,
    val preampDb: Float = 0.0f,
    val leftBands: List<PeqBand> = emptyList(),
    val rightBands: List<PeqBand> = emptyList(),
    val mixer: MixerConfig = MixerConfig()
)

object EqualizerApoCodec {

    private val TYPE_MAP = mapOf(
        "PK" to FilterType.PEAKING.code,
        "PEQ" to FilterType.PEAKING.code,
        "LS" to FilterType.LOW_SHELF.code,
        "LSC" to FilterType.LOW_SHELF.code,
        "HS" to FilterType.HIGH_SHELF.code,
        "HSC" to FilterType.HIGH_SHELF.code,
        "HP" to FilterType.HIGH_PASS.code,
        "HPQ" to FilterType.HIGH_PASS.code,
        "LP" to FilterType.LOW_PASS.code,
        "LPQ" to FilterType.LOW_PASS.code,
        "NO" to FilterType.NOTCH.code,
        "NOTCH" to FilterType.NOTCH.code,
        "BP" to FilterType.BAND_PASS.code
    )

    private val CODE_MAP = mapOf(
        FilterType.BYPASS.code to "PK",
        FilterType.PEAKING.code to "PK",
        FilterType.LOW_SHELF.code to "LSC",
        FilterType.HIGH_SHELF.code to "HSC",
        FilterType.HIGH_PASS.code to "HPQ",
        FilterType.LOW_PASS.code to "LPQ",
        FilterType.NOTCH.code to "NO",
        FilterType.BAND_PASS.code to "BP"
    )

    private val PREAMP_REGEX = Regex("""^Preamp:\s*([+-]?\d+(?:\.\d+)?)\s*dB""", RegexOption.IGNORE_CASE)
    private val FILTER_REGEX = Regex(
        """^Filter\s*\d*\s*:\s*(ON|OFF)\s+([A-Z]+)\s+Fc\s+([0-9.]+)\s*Hz(?:\s+Gain\s+([+-]?[0-9.]+)\s*dB)?(?:\s+Q\s+([0-9.]+))?""",
        RegexOption.IGNORE_CASE
    )

    private val json = Json {
        ignoreUnknownKeys = true
        prettyPrint = true
        encodeDefaults = true
    }

    fun parseEqualizerApoText(text: String, fallbackPreampDb: Float = 0.0f): ApoParseResult {
        var parsedPreamp = fallbackPreampDb.coerceIn(DspDefaults.MIN_PREAMP_DB, DspDefaults.MAX_PREAMP_DB)
        val parsedBands = mutableListOf<PeqBand>()

        for (rawLine in text.lineSequence()) {
            val line = rawLine.trim()
            if (line.isEmpty() || line.startsWith("#")) continue

            val preampMatch = PREAMP_REGEX.find(line)
            if (preampMatch != null) {
                val v = preampMatch.groupValues[1].toFloatOrNull() ?: parsedPreamp
                parsedPreamp = v.coerceIn(DspDefaults.MIN_PREAMP_DB, DspDefaults.MAX_PREAMP_DB)
                continue
            }

            val filterMatch = FILTER_REGEX.find(line)
            if (filterMatch != null && parsedBands.size < DspDefaults.MAX_BANDS) {
                val isOn = filterMatch.groupValues[1].equals("ON", ignoreCase = true)
                val code = filterMatch.groupValues[2].uppercase()
                val mappedType = if (isOn) (TYPE_MAP[code] ?: FilterType.PEAKING.code) else FilterType.BYPASS.code
                val freq = (filterMatch.groupValues[3].toFloatOrNull() ?: 1000f)
                    .roundToInt()
                    .coerceIn(DspDefaults.MIN_FREQ_HZ, DspDefaults.MAX_FREQ_HZ)
                val gain = (filterMatch.groupValues[4].takeIf { it.isNotEmpty() }?.toFloatOrNull() ?: 0.0f)
                    .coerceIn(DspDefaults.MIN_GAIN_DB, DspDefaults.MAX_GAIN_DB)
                val q = (filterMatch.groupValues[5].takeIf { it.isNotEmpty() }?.toFloatOrNull() ?: 0.71f)
                    .coerceIn(DspDefaults.MIN_Q, DspDefaults.MAX_Q)
                parsedBands.add(PeqBand(type = mappedType, freq = freq, gain = gain, q = q))
            }
        }

        return ApoParseResult(preampDb = parsedPreamp, bands = parsedBands)
    }

    fun exportEqualizerApoText(preampDb: Float, bands: List<PeqBand>): String {
        val sb = StringBuilder()
        sb.append("Preamp: ").append(String.format(java.util.Locale.US, "%.1f", preampDb)).append(" dB\n")
        bands.forEachIndexed { idx, b ->
            val state = if (b.type == FilterType.BYPASS.code) "OFF" else "ON"
            val code = CODE_MAP[b.type] ?: "PK"
            val fStr = b.freq.coerceIn(DspDefaults.MIN_FREQ_HZ, DspDefaults.MAX_FREQ_HZ)
            val qStr = String.format(java.util.Locale.US, "%.2f", b.q)
            if (b.filterType.usesGain || b.type == FilterType.BYPASS.code) {
                val gStr = String.format(java.util.Locale.US, "%.1f", b.gain)
                sb.append("Filter ${idx + 1}: $state $code Fc $fStr Hz Gain $gStr dB Q $qStr\n")
            } else {
                sb.append("Filter ${idx + 1}: $state $code Fc $fStr Hz Q $qStr\n")
            }
        }
        return sb.toString()
    }

    fun exportSnapshotJson(snapshot: WebHidSnapshotJson): String =
        json.encodeToString(snapshot)

    fun importSnapshotJson(text: String): WebHidSnapshotJson {
        val decoded = json.decodeFromString<WebHidSnapshotJson>(text)
        return decoded.copy(
            preampDb = decoded.preampDb.coerceIn(DspDefaults.MIN_PREAMP_DB, DspDefaults.MAX_PREAMP_DB),
            leftBands = decoded.leftBands.take(DspDefaults.MAX_BANDS).map { it.clamped() }
                .ifEmpty { DspDefaults.createDefaultBands(5) },
            rightBands = decoded.rightBands.take(DspDefaults.MAX_BANDS).map { it.clamped() }
                .ifEmpty { DspDefaults.createDefaultBands(5) },
            mixer = decoded.mixer.sanitized()
        )
    }
}
