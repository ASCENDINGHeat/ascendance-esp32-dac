package com.ascendance.dac.ui.screens

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.outlined.Tune
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.SegmentedButton
import androidx.compose.material3.SegmentedButtonDefaults
import androidx.compose.material3.SingleChoiceSegmentedButtonRow
import androidx.compose.material3.Surface
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.hapticfeedback.HapticFeedbackType
import androidx.compose.ui.platform.LocalHapticFeedback
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.ascendance.dac.model.DspProConfig
import com.ascendance.dac.model.DynamicsConfig
import com.ascendance.dac.model.LoudnessMode
import com.ascendance.dac.ui.theme.AscendanceColors
import java.util.Locale

@Composable
internal fun ToneDynamicsCard(
    dynamics: DynamicsConfig,
    dspPro: DspProConfig,
    onUpdateDynamics: (DynamicsConfig) -> Unit,
    onUpdateDspPro: (DspProConfig) -> Unit
) {
    val colors = MaterialTheme.colorScheme
    val haptics = LocalHapticFeedback.current

    Surface(
        color = colors.surface,
        shape = RoundedCornerShape(12.dp),
        border = BorderStroke(1.dp, colors.outline)
    ) {
        Column(modifier = Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
            ToneSectionHeader(
                icon = Icons.Outlined.Tune,
                title = "Dynamics & Equal Loudness"
            )

            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically
            ) {
                Column(modifier = Modifier.weight(1f)) {
                    Text("True-Peak Limiter", color = colors.onSurface, fontSize = 13.sp)
                    Text("1.5 ms lookahead with -0.5 dBFS ceiling", color = colors.onSurfaceVariant, fontSize = 11.sp)
                }
                Switch(
                    checked = dynamics.limiterEnabled,
                    onCheckedChange = {
                        haptics.performHapticFeedback(HapticFeedbackType.TextHandleMove)
                        onUpdateDynamics(dynamics.copy(limiterEnabled = it))
                    }
                )
            }

            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically
            ) {
                Column(modifier = Modifier.weight(1f)) {
                    Row(
                        verticalAlignment = Alignment.CenterVertically,
                        horizontalArrangement = Arrangement.spacedBy(6.dp)
                    ) {
                        Text("Auto-Preamp Headroom Guard", color = colors.onSurface, fontSize = 13.sp)
                        if (dynamics.autoPreampEnabled) {
                            Text(
                                text = String.format(Locale.US, "%.1f dB", dynamics.autoPreampDb),
                                color = AscendanceColors.AccentAmber,
                                fontFamily = FontFamily.Monospace,
                                fontWeight = FontWeight.SemiBold,
                                fontSize = 11.sp
                            )
                        }
                    }
                    Text(
                        "Auto-attenuates peak EQ & harmonics boost",
                        color = colors.onSurfaceVariant,
                        fontSize = 11.sp
                    )
                }
                Switch(
                    checked = dynamics.autoPreampEnabled,
                    onCheckedChange = {
                        haptics.performHapticFeedback(HapticFeedbackType.TextHandleMove)
                        onUpdateDynamics(dynamics.copy(autoPreampEnabled = it))
                    }
                )
            }

            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically
            ) {
                Column(modifier = Modifier.weight(1f)) {
                    Text("Subsonic / DC Blocker (18 Hz)", color = colors.onSurface, fontSize = 13.sp)
                    Text("Filters infrasonic rumble before limiter", color = colors.onSurfaceVariant, fontSize = 11.sp)
                }
                Switch(
                    checked = dspPro.subsonicFilterEnabled,
                    onCheckedChange = {
                        haptics.performHapticFeedback(HapticFeedbackType.TextHandleMove)
                        onUpdateDspPro(dspPro.copy(subsonicFilterEnabled = it))
                    }
                )
            }

            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically
            ) {
                Column(modifier = Modifier.weight(1f)) {
                    Text("24-Bit TPDF Dither", color = colors.onSurface, fontSize = 13.sp)
                    Text("Decorrelates quantization noise at low volume", color = colors.onSurfaceVariant, fontSize = 11.sp)
                }
                Switch(
                    checked = dspPro.tpdfDitherEnabled,
                    onCheckedChange = {
                        haptics.performHapticFeedback(HapticFeedbackType.TextHandleMove)
                        onUpdateDspPro(dspPro.copy(tpdfDitherEnabled = it))
                    }
                )
            }

            Text("Equal Loudness (ISO 226, Vol < 80%)", color = colors.onSurfaceVariant, fontSize = 12.sp)
            SingleChoiceSegmentedButtonRow(modifier = Modifier.fillMaxWidth()) {
                LoudnessMode.entries.forEachIndexed { idx, mode ->
                    SegmentedButton(
                        selected = dynamics.loudnessMode == mode.code,
                        onClick = {
                            haptics.performHapticFeedback(HapticFeedbackType.TextHandleMove)
                            onUpdateDynamics(dynamics.copy(loudnessMode = mode.code))
                        },
                        shape = SegmentedButtonDefaults.itemShape(index = idx, count = LoudnessMode.entries.size),
                        label = { Text(mode.label, fontSize = 11.sp) }
                    )
                }
            }
        }
    }
}
