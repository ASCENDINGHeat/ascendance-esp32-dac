package com.ascendance.dac.ui.components

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.gestures.awaitEachGesture
import androidx.compose.foundation.gestures.awaitFirstDown
import androidx.compose.foundation.gestures.calculatePan
import androidx.compose.foundation.gestures.calculateZoom
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.FilterChip
import androidx.compose.material3.FilterChipDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.PathEffect
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.StrokeJoin
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.drawText
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.rememberTextMeasurer
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.ascendance.dac.dsp.BiquadMath
import com.ascendance.dac.model.DspDefaults
import com.ascendance.dac.model.FilterType
import com.ascendance.dac.model.PeqBand
import com.ascendance.dac.ui.theme.AscendanceColors
import java.util.Locale
import kotlin.math.exp
import kotlin.math.hypot
import kotlin.math.ln
import kotlin.math.pow
import kotlin.math.roundToInt

private const val LOG_MIN = 2.995732273553991 // ln(20.0)
private const val LOG_MAX = 9.903487552536127 // ln(20000.0)

private fun freqToFraction(freqHz: Double): Float =
    ((ln(freqHz.coerceIn(20.0, 20000.0)) - LOG_MIN) / (LOG_MAX - LOG_MIN)).toFloat()

private fun fractionToFreq(fraction: Float): Int {
    val f = exp(LOG_MIN + fraction.coerceIn(0f, 1f) * (LOG_MAX - LOG_MIN))
    return f.roundToInt().coerceIn(DspDefaults.MIN_FREQ_HZ, DspDefaults.MAX_FREQ_HZ)
}

private fun gainToFraction(gainDb: Float): Float =
    ((12.0f - gainDb.coerceIn(-24.0f, 12.0f)) / 36.0f)

private fun fractionToGain(fraction: Float): Float {
    val g = 12.0f - fraction.coerceIn(0f, 1f) * 36.0f
    return ((g * 10.0f).roundToInt() / 10.0f).coerceIn(DspDefaults.MIN_GAIN_DB, DspDefaults.MAX_GAIN_DB)
}

@Composable
fun PeqCanvasCard(
    bands: List<PeqBand>,
    selectedBandIdx: Int,
    preampDb: Float,
    abBypass: Boolean,
    sampleRateHz: Int,
    showPhase: Boolean,
    showGroupDelay: Boolean,
    onSelectBand: (Int) -> Unit,
    onUpdateBand: (Int, PeqBand) -> Unit,
    onAddBandAt: (Int, Float) -> Unit,
    onTogglePhase: (Boolean) -> Unit,
    onToggleGroupDelay: (Boolean) -> Unit,
    modifier: Modifier = Modifier
) {
    val latestBands by rememberUpdatedState(bands)
    val latestSelected by rememberUpdatedState(selectedBandIdx)

    val curvePoints = remember(bands, preampDb, abBypass, sampleRateHz) {
        BiquadMath.sampleCurve(
            bands = bands,
            preampDb = preampDb,
            abBypass = abBypass,
            sampleRateHz = sampleRateHz,
            steps = 160
        )
    }

    val textMeasurer = rememberTextMeasurer()

    val colors = MaterialTheme.colorScheme

    Surface(
        modifier = modifier.fillMaxWidth(),
        color = colors.surface,
        shape = RoundedCornerShape(14.dp),
        border = BorderStroke(1.dp, colors.outline)
    ) {
        Column(modifier = Modifier.padding(10.dp)) {
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(bottom = 6.dp),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically
            ) {
                val sel = bands.getOrNull(selectedBandIdx)
                val readout = if (sel != null) {
                    val gTxt = if (sel.filterType.usesGain) {
                        String.format(Locale.US, "  %+.1fdB", sel.gain)
                    } else ""
                    "B${selectedBandIdx + 1} ${sel.filterType.shortLabel}: ${sel.freq}Hz$gTxt  Q=${
                        String.format(Locale.US, "%.2f", sel.q)
                    }"
                } else {
                    "No Band Selected"
                }
                Text(
                    text = if (abBypass) "A/B BYPASS ACTIVE (PREAMP ${
                        String.format(Locale.US, "%+.1fdB", preampDb)
                    })" else readout,
                    color = if (abBypass) AscendanceColors.AccentAmber else colors.primary,
                    fontSize = 12.sp,
                    fontFamily = FontFamily.Monospace,
                    fontWeight = FontWeight.SemiBold
                )

                Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                    FilterChip(
                        selected = showPhase,
                        onClick = { onTogglePhase(!showPhase) },
                        label = { Text("Phase", fontSize = 11.sp) },
                        colors = FilterChipDefaults.filterChipColors(
                            selectedContainerColor = AscendanceColors.AccentAmber.copy(alpha = 0.22f),
                            selectedLabelColor = AscendanceColors.AccentAmber
                        ),
                        modifier = Modifier.height(26.dp)
                    )
                    FilterChip(
                        selected = showGroupDelay,
                        onClick = { onToggleGroupDelay(!showGroupDelay) },
                        label = { Text("GD", fontSize = 11.sp) },
                        colors = FilterChipDefaults.filterChipColors(
                            selectedContainerColor = AscendanceColors.AccentPurple.copy(alpha = 0.22f),
                            selectedLabelColor = AscendanceColors.AccentPurple
                        ),
                        modifier = Modifier.height(26.dp)
                    )
                }
            }

            Box(
                modifier = Modifier
                    .fillMaxWidth()
                    .height(175.dp)
                    .background(colors.background, RoundedCornerShape(10.dp))
            ) {
                Canvas(
                    modifier = Modifier
                        .fillMaxSize()
                        .pointerInput(Unit) {
                            detectTapGestures(
                                onTap = { tap ->
                                    val w = size.width.toFloat().coerceAtLeast(1f)
                                    val h = size.height.toFloat().coerceAtLeast(1f)
                                    val hitIdx = findNearestBand(latestBands, tap, w, h, 42f)
                                    if (hitIdx >= 0) {
                                        onSelectBand(hitIdx)
                                    }
                                },
                                onDoubleTap = { tap ->
                                    val w = size.width.toFloat().coerceAtLeast(1f)
                                    val h = size.height.toFloat().coerceAtLeast(1f)
                                    val hitIdx = findNearestBand(latestBands, tap, w, h, 42f)
                                    if (hitIdx >= 0) {
                                        val b = latestBands[hitIdx]
                                        onSelectBand(hitIdx)
                                        onUpdateBand(hitIdx, b.copy(gain = 0.0f))
                                    } else if (latestBands.size < DspDefaults.MAX_BANDS) {
                                        val freq = fractionToFreq(tap.x / w)
                                        val gain = fractionToGain(tap.y / h)
                                        onAddBandAt(freq, gain)
                                    }
                                }
                            )
                        }
                        .pointerInput(Unit) {
                            awaitEachGesture {
                                val firstDown = awaitFirstDown(requireUnconsumed = false)
                                val w = size.width.toFloat().coerceAtLeast(1f)
                                val h = size.height.toFloat().coerceAtLeast(1f)
                                var draggedIdx = findNearestBand(latestBands, firstDown.position, w, h, 48f)
                                if (draggedIdx >= 0) {
                                    onSelectBand(draggedIdx)
                                }

                                do {
                                    val event = awaitPointerEvent()
                                    val pressedCount = event.changes.count { it.pressed }
                                    if (pressedCount >= 2) {
                                        val zoom = event.calculateZoom()
                                        if (zoom != 1f) {
                                            val selIdx = latestSelected.coerceIn(0, latestBands.lastIndex.coerceAtLeast(0))
                                            val b = latestBands.getOrNull(selIdx)
                                            if (b != null) {
                                                val newQ = (b.q * zoom).coerceIn(DspDefaults.MIN_Q, DspDefaults.MAX_Q)
                                                val roundedQ = (newQ * 100f).roundToInt() / 100f
                                                onUpdateBand(selIdx, b.copy(q = roundedQ))
                                                event.changes.forEach { it.consume() }
                                            }
                                        }
                                    } else if (pressedCount == 1 && draggedIdx >= 0) {
                                        val change = event.changes.firstOrNull { it.pressed }
                                        if (change != null && change.position != change.previousPosition) {
                                            val b = latestBands.getOrNull(draggedIdx)
                                            if (b != null) {
                                                val newFreq = fractionToFreq(change.position.x / w)
                                                val newGain = if (b.filterType.usesGain) {
                                                    fractionToGain(change.position.y / h)
                                                } else {
                                                    b.gain
                                                }
                                                onUpdateBand(draggedIdx, b.copy(freq = newFreq, gain = newGain))
                                                change.consume()
                                            }
                                        } else if (event.calculatePan() != Offset.Zero) {
                                            event.changes.forEach { it.consume() }
                                        }
                                    }
                                } while (event.changes.any { it.pressed })
                            }
                        }
                ) {
                    val w = size.width
                    val h = size.height
                    val labelStyle = TextStyle(
                        color = Color(0xFF6E7681),
                        fontSize = 9.sp,
                        fontFamily = FontFamily.Monospace
                    )

                    // Vertical frequency grid lines
                    val gridFreqs = listOf(20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000)
                    for (gf in gridFreqs) {
                        val x = freqToFraction(gf.toDouble()) * w
                        drawLine(
                            color = Color(0xFF1B212A),
                            start = Offset(x, 0f),
                            end = Offset(x, h),
                            strokeWidth = 1f
                        )
                        val lbl = if (gf >= 1000) "${gf / 1000}k" else "$gf"
                        val textX = (x - 8f).coerceIn(2f, w - 26f)
                        drawText(textMeasurer, lbl, Offset(textX, h - 14.dp.toPx()), style = labelStyle)
                    }

                    // Horizontal dB grid lines
                    val gridDb = listOf(12, 6, 0, -6, -12, -18, -24)
                    for (db in gridDb) {
                        val y = gainToFraction(db.toFloat()) * h
                        val isZero = db == 0
                        drawLine(
                            color = if (isZero) Color(0xFF3A4452) else Color(0xFF1B212A),
                            start = Offset(0f, y),
                            end = Offset(w, y),
                            strokeWidth = if (isZero) 1.5f else 1f
                        )
                        if (db != -24) {
                            drawText(textMeasurer, "${if (db > 0) "+" else ""}$db", Offset(4f, y - 6f), style = labelStyle)
                        }
                    }

                    // Selected band bandwidth guide
                    bands.getOrNull(selectedBandIdx)?.let { selBand ->
                        if (selBand.type != FilterType.BYPASS.code) {
                            val color = Color(DspDefaults.BAND_COLORS_ARGB[selectedBandIdx % DspDefaults.BAND_COLORS_ARGB.size])
                            val cx = freqToFraction(selBand.freq.toDouble()) * w
                            drawLine(
                                color = color.copy(alpha = 0.45f),
                                start = Offset(cx, 0f),
                                end = Offset(cx, h),
                                strokeWidth = 1.5f,
                                pathEffect = PathEffect.dashPathEffect(floatArrayOf(6f, 6f))
                            )
                        }
                    }

                    // Phase overlay (-180..+180 deg mapped across canvas height)
                    if (showPhase && curvePoints.size > 1) {
                        val phasePath = Path()
                        curvePoints.forEachIndexed { idx, pt ->
                            val x = freqToFraction(pt.freqHz) * w
                            val y = ((180.0 - pt.phaseDeg) / 360.0).toFloat() * h
                            if (idx == 0) phasePath.moveTo(x, y) else phasePath.lineTo(x, y)
                        }
                        drawPath(
                            path = phasePath,
                            color = AscendanceColors.AccentAmber.copy(alpha = 0.85f),
                            style = Stroke(width = 2.dp.toPx())
                        )
                    }

                    // Group Delay overlay (-5..+15 ms mapped across canvas height)
                    if (showGroupDelay && curvePoints.size > 1) {
                        val gdPath = Path()
                        curvePoints.forEachIndexed { idx, pt ->
                            val x = freqToFraction(pt.freqHz) * w
                            val y = ((15.0 - pt.groupDelayMs) / 20.0).toFloat().coerceIn(0f, 1f) * h
                            if (idx == 0) gdPath.moveTo(x, y) else gdPath.lineTo(x, y)
                        }
                        drawPath(
                            path = gdPath,
                            color = AscendanceColors.AccentPurple.copy(alpha = 0.85f),
                            style = Stroke(width = 2.dp.toPx())
                        )
                    }

                    // Combined EQ magnitude curve + subtle gradient fill
                    if (curvePoints.size > 1) {
                        val linePath = Path()
                        val fillPath = Path()
                        val zeroY = gainToFraction(0f) * h
                        curvePoints.forEachIndexed { idx, pt ->
                            val x = freqToFraction(pt.freqHz) * w
                            val y = gainToFraction(pt.magDb.toFloat()) * h
                            if (idx == 0) {
                                linePath.moveTo(x, y)
                                fillPath.moveTo(x, zeroY)
                                fillPath.lineTo(x, y)
                            } else {
                                linePath.lineTo(x, y)
                                fillPath.lineTo(x, y)
                            }
                        }
                        fillPath.lineTo(w, zeroY)
                        fillPath.close()

                        drawPath(
                            path = fillPath,
                            brush = Brush.verticalGradient(
                                colors = listOf(
                                    AscendanceColors.AccentBlue.copy(alpha = 0.25f),
                                    AscendanceColors.AccentBlue.copy(alpha = 0.02f)
                                )
                            )
                        )
                        drawPath(
                            path = linePath,
                            color = if (abBypass) AscendanceColors.TextSecondary else AscendanceColors.AccentBlue,
                            style = Stroke(width = 2.5.dp.toPx(), cap = StrokeCap.Round, join = StrokeJoin.Round)
                        )
                    }

                    // Band control handles
                    bands.forEachIndexed { idx, band ->
                        val color = Color(DspDefaults.BAND_COLORS_ARGB[idx % DspDefaults.BAND_COLORS_ARGB.size])
                        val cx = freqToFraction(band.freq.toDouble()) * w
                        val cy = if (band.filterType.usesGain) {
                            gainToFraction(band.gain) * h
                        } else {
                            gainToFraction(0f) * h
                        }
                        val isSel = idx == selectedBandIdx
                        val radius = if (isSel) 9.dp.toPx() else 6.dp.toPx()
                        val alpha = if (band.type == FilterType.BYPASS.code) 0.35f else 1.0f

                        if (isSel) {
                            drawCircle(
                                color = color.copy(alpha = 0.28f),
                                radius = radius + 5.dp.toPx(),
                                center = Offset(cx, cy)
                            )
                        }
                        drawCircle(
                            color = color.copy(alpha = alpha),
                            radius = radius,
                            center = Offset(cx, cy)
                        )
                        drawCircle(
                            color = Color.White.copy(alpha = if (isSel) 0.95f else 0.55f),
                            radius = radius,
                            center = Offset(cx, cy),
                            style = Stroke(width = if (isSel) 2.dp.toPx() else 1.dp.toPx())
                        )
                    }
                }
            }

            Text(
                text = "Drag node: Freq/Gain  •  Pinch: Q  •  Double-tap node: 0 dB  •  Double-tap empty: +Band",
                color = AscendanceColors.TextSecondary,
                fontSize = 10.sp,
                modifier = Modifier.padding(top = 5.dp)
            )
        }
    }
}

private fun findNearestBand(bands: List<PeqBand>, pt: Offset, w: Float, h: Float, maxDistPx: Float): Int {
    var bestIdx = -1
    var bestDist = maxDistPx
    bands.forEachIndexed { idx, band ->
        val bx = freqToFraction(band.freq.toDouble()) * w
        val by = if (band.filterType.usesGain) gainToFraction(band.gain) * h else gainToFraction(0f) * h
        val d = hypot(pt.x - bx, pt.y - by)
        if (d < bestDist) {
            bestDist = d
            bestIdx = idx
        }
    }
    return bestIdx
}
