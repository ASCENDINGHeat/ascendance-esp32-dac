package com.ascendance.dac.ui.screens

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.outlined.GraphicEq
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.SegmentedButton
import androidx.compose.material3.SegmentedButtonDefaults
import androidx.compose.material3.SingleChoiceSegmentedButtonRow
import androidx.compose.material3.Slider
import androidx.compose.material3.SliderDefaults
import androidx.compose.material3.Surface
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.hapticfeedback.HapticFeedbackType
import androidx.compose.ui.platform.LocalHapticFeedback
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.ascendance.dac.model.DspDefaults
import com.ascendance.dac.model.DspProConfig
import com.ascendance.dac.model.ExciterMode
import com.ascendance.dac.model.HarmonicsConfig
import com.ascendance.dac.ui.theme.AscendanceColors
import java.util.Locale
import kotlin.math.roundToInt

@Composable
internal fun ToneHarmonicsCard(
    harmonics: HarmonicsConfig,
    onUpdateHarmonics: (HarmonicsConfig) -> Unit,
    dspPro: DspProConfig,
    onUpdateDspPro: (DspProConfig) -> Unit
) {
    val colors = MaterialTheme.colorScheme
    val haptics = LocalHapticFeedback.current

    Surface(
        color = colors.surface,
        shape = RoundedCornerShape(12.dp),
        border = BorderStroke(1.dp, colors.outline)
    ) {
        Column(modifier = Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            ToneSectionHeader(
                icon = Icons.Outlined.GraphicEq,
                title = "Bass Enhancer & Harmonic Exciter"
            )

            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically
            ) {
                Column(modifier = Modifier.weight(1f)) {
                    Text("Bass Enhancer", color = colors.onSurface, fontSize = 13.sp)
                    Text(
                        "Psychoacoustic missing-fundamental low-end warmth",
                        color = colors.onSurfaceVariant,
                        fontSize = 11.sp
                    )
                }
                Switch(
                    checked = harmonics.bassEnabled,
                    onCheckedChange = {
                        haptics.performHapticFeedback(HapticFeedbackType.TextHandleMove)
                        onUpdateHarmonics(harmonics.copy(bassEnabled = it))
                    }
                )
            }

            Row(modifier = Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
                Text("Bass Drive", color = colors.onSurfaceVariant, fontSize = 11.sp)
                Text(
                    text = "${harmonics.bassAmountPct}%",
                    color = if (harmonics.bassEnabled) AscendanceColors.AccentBlue else colors.onSurfaceVariant,
                    fontFamily = FontFamily.Monospace,
                    fontSize = 11.sp
                )
            }
            Slider(
                value = harmonics.bassAmountPct.toFloat(),
                onValueChange = { onUpdateHarmonics(harmonics.copy(bassAmountPct = it.roundToInt())) },
                valueRange = DspDefaults.MIN_HARMONICS_AMOUNT_PCT.toFloat()..DspDefaults.MAX_HARMONICS_AMOUNT_PCT.toFloat(),
                colors = SliderDefaults.colors(
                    thumbColor = AscendanceColors.AccentBlue,
                    activeTrackColor = AscendanceColors.AccentBlue
                )
            )

            Row(modifier = Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
                Text("Cutoff (50–150 Hz)", color = colors.onSurfaceVariant, fontSize = 11.sp)
                Text(
                    text = "${harmonics.bassFreqHz} Hz",
                    color = if (harmonics.bassEnabled) AscendanceColors.AccentBlue else colors.onSurfaceVariant,
                    fontFamily = FontFamily.Monospace,
                    fontSize = 11.sp
                )
            }
            Slider(
                value = harmonics.bassFreqHz.toFloat(),
                onValueChange = { onUpdateHarmonics(harmonics.copy(bassFreqHz = it.roundToInt())) },
                valueRange = DspDefaults.MIN_BASS_FREQ_HZ.toFloat()..DspDefaults.MAX_BASS_FREQ_HZ.toFloat(),
                colors = SliderDefaults.colors(
                    thumbColor = AscendanceColors.AccentBlue,
                    activeTrackColor = AscendanceColors.AccentBlue
                )
            )

            Box(
                modifier = Modifier
                    .fillMaxWidth()
                    .height(1.dp)
                    .background(colors.outline)
            )

            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically
            ) {
                Column(modifier = Modifier.weight(1f)) {
                    Text("Dynamic Bass", color = colors.onSurface, fontSize = 13.sp)
                    Text(
                        "Envelope-controlled 90 Hz sub-bass boost that ducks on loud hits",
                        color = colors.onSurfaceVariant,
                        fontSize = 11.sp
                    )
                }
                Switch(
                    checked = dspPro.dynamicBassEnabled,
                    onCheckedChange = {
                        haptics.performHapticFeedback(HapticFeedbackType.TextHandleMove)
                        onUpdateDspPro(dspPro.copy(dynamicBassEnabled = it))
                    }
                )
            }

            Row(modifier = Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
                Text("Sub-Bass Amount", color = colors.onSurfaceVariant, fontSize = 11.sp)
                Text(
                    text = "${dspPro.dynamicBassAmountPct}%",
                    color = if (dspPro.dynamicBassEnabled) AscendanceColors.AccentBlue else colors.onSurfaceVariant,
                    fontFamily = FontFamily.Monospace,
                    fontSize = 11.sp
                )
            }
            Slider(
                value = dspPro.dynamicBassAmountPct.toFloat(),
                onValueChange = { onUpdateDspPro(dspPro.copy(dynamicBassAmountPct = it.roundToInt())) },
                valueRange = 0f..100f,
                colors = SliderDefaults.colors(
                    thumbColor = AscendanceColors.AccentBlue,
                    activeTrackColor = AscendanceColors.AccentBlue
                )
            )

            Box(
                modifier = Modifier
                    .fillMaxWidth()
                    .height(1.dp)
                    .background(colors.outline)
            )

            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically
            ) {
                Column(modifier = Modifier.weight(1f)) {
                    Text("Harmonic Exciter", color = colors.onSurface, fontSize = 13.sp)
                    Text(
                        "Upper-harmonic saturation & presence sheen",
                        color = colors.onSurfaceVariant,
                        fontSize = 11.sp
                    )
                }
                Switch(
                    checked = harmonics.exciterEnabled,
                    onCheckedChange = {
                        haptics.performHapticFeedback(HapticFeedbackType.TextHandleMove)
                        onUpdateHarmonics(harmonics.copy(exciterEnabled = it))
                    }
                )
            }

            SingleChoiceSegmentedButtonRow(modifier = Modifier.fillMaxWidth()) {
                ExciterMode.entries.forEachIndexed { idx, mode ->
                    SegmentedButton(
                        selected = harmonics.exciterMode == mode.code,
                        onClick = {
                            haptics.performHapticFeedback(HapticFeedbackType.TextHandleMove)
                            onUpdateHarmonics(harmonics.copy(exciterMode = mode.code))
                        },
                        shape = SegmentedButtonDefaults.itemShape(index = idx, count = ExciterMode.entries.size),
                        label = { Text(mode.label, fontSize = 11.sp) }
                    )
                }
            }

            Row(modifier = Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
                Text("Excite", color = colors.onSurfaceVariant, fontSize = 11.sp)
                Text(
                    text = "${harmonics.exciterAmountPct}%",
                    color = if (harmonics.exciterEnabled) AscendanceColors.AccentAmber else colors.onSurfaceVariant,
                    fontFamily = FontFamily.Monospace,
                    fontSize = 11.sp
                )
            }
            Slider(
                value = harmonics.exciterAmountPct.toFloat(),
                onValueChange = { onUpdateHarmonics(harmonics.copy(exciterAmountPct = it.roundToInt())) },
                valueRange = DspDefaults.MIN_HARMONICS_AMOUNT_PCT.toFloat()..DspDefaults.MAX_HARMONICS_AMOUNT_PCT.toFloat(),
                colors = SliderDefaults.colors(
                    thumbColor = AscendanceColors.AccentAmber,
                    activeTrackColor = AscendanceColors.AccentAmber
                )
            )

            Row(modifier = Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
                Text("Cutoff (1.5–8.0 kHz)", color = colors.onSurfaceVariant, fontSize = 11.sp)
                Text(
                    text = String.format(Locale.US, "%.1f kHz", harmonics.exciterFreqHz / 1000.0f),
                    color = if (harmonics.exciterEnabled) AscendanceColors.AccentAmber else colors.onSurfaceVariant,
                    fontFamily = FontFamily.Monospace,
                    fontSize = 11.sp
                )
            }
            Slider(
                value = harmonics.exciterFreqHz.toFloat(),
                onValueChange = {
                    val hz = (it / 100f).roundToInt() * 100
                    onUpdateHarmonics(harmonics.copy(exciterFreqHz = hz))
                },
                valueRange = DspDefaults.MIN_EXCITER_FREQ_HZ.toFloat()..DspDefaults.MAX_EXCITER_FREQ_HZ.toFloat(),
                colors = SliderDefaults.colors(
                    thumbColor = AscendanceColors.AccentAmber,
                    activeTrackColor = AscendanceColors.AccentAmber
                )
            )
        }
    }
}
