package com.ascendance.dac.ui.screens

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.SegmentedButton
import androidx.compose.material3.SegmentedButtonDefaults
import androidx.compose.material3.SingleChoiceSegmentedButtonRow
import androidx.compose.material3.Slider
import androidx.compose.material3.SliderDefaults
import androidx.compose.material3.Surface
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.ascendance.dac.model.DynamicsConfig
import com.ascendance.dac.model.LoudnessMode
import com.ascendance.dac.model.MixerConfig
import com.ascendance.dac.model.MixerMode
import com.ascendance.dac.ui.theme.AscendanceColors
import java.util.Locale
import kotlin.math.roundToInt

@Composable
fun MatrixTab(
    mixer: MixerConfig,
    dynamics: DynamicsConfig,
    sampleRateHz: Int,
    onUpdateMixer: (MixerConfig) -> Unit,
    onUpdateDynamics: (DynamicsConfig) -> Unit,
    onUploadFirFile: () -> Unit,
    onClearFir: () -> Unit,
    modifier: Modifier = Modifier
) {
    var showModeMenu by remember { mutableStateOf(false) }

    Column(
        modifier = modifier
            .fillMaxWidth()
            .verticalScroll(rememberScrollState()),
        verticalArrangement = Arrangement.spacedBy(10.dp)
    ) {
        Surface(
            color = AscendanceColors.CardDark,
            shape = RoundedCornerShape(12.dp),
            border = BorderStroke(1.dp, AscendanceColors.BorderDark)
        ) {
            Column(modifier = Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.SpaceBetween,
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    Text("Routing Mode", color = AscendanceColors.TextSecondary, fontSize = 13.sp)
                    Box {
                        OutlinedButton(onClick = { showModeMenu = true }) {
                            Text(mixer.mixerMode.label, fontSize = 12.sp)
                        }
                        DropdownMenu(expanded = showModeMenu, onDismissRequest = { showModeMenu = false }) {
                            MixerMode.entries.forEach { mode ->
                                DropdownMenuItem(
                                    text = { Text(mode.label) },
                                    onClick = {
                                        showModeMenu = false
                                        val nextCrossfeed = if (mode == MixerMode.CROSSFEED && mixer.crossfeedLevel == 0) {
                                            1
                                        } else {
                                            mixer.crossfeedLevel
                                        }
                                        onUpdateMixer(mixer.copy(mode = mode.code, crossfeedLevel = nextCrossfeed))
                                    }
                                )
                            }
                        }
                    }
                }

                // L/R Balance
                val balLabel = when {
                    mixer.balance == 0 -> "Center (0)"
                    mixer.balance < 0 -> "Left ${-mixer.balance}%"
                    else -> "Right +${mixer.balance}%"
                }
                Row(modifier = Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
                    Text("Channel Balance (L / R)", color = AscendanceColors.TextSecondary, fontSize = 12.sp)
                    Text(balLabel, color = AscendanceColors.AccentBlue, fontFamily = FontFamily.Monospace, fontSize = 12.sp)
                }
                Slider(
                    value = mixer.balance.toFloat(),
                    onValueChange = { onUpdateMixer(mixer.copy(balance = it.roundToInt())) },
                    valueRange = -100f..100f
                )

                // Mid-Side Stereo Width
                Row(modifier = Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
                    Text("M/S Stereo Width", color = AscendanceColors.TextSecondary, fontSize = 12.sp)
                    Text("${mixer.widthPct}%", color = AscendanceColors.AccentBlue, fontFamily = FontFamily.Monospace, fontSize = 12.sp)
                }
                Slider(
                    value = mixer.widthPct.toFloat(),
                    onValueChange = {
                        val w = (it / 5f).roundToInt() * 5
                        val nextMode = if (mixer.mode != MixerMode.WIDTH.code) MixerMode.WIDTH.code else mixer.mode
                        onUpdateMixer(mixer.copy(mode = nextMode, widthPct = w))
                    },
                    valueRange = 0f..200f,
                    colors = SliderDefaults.colors(
                        thumbColor = AscendanceColors.AccentGreen,
                        activeTrackColor = AscendanceColors.AccentGreen
                    )
                )

                // Headphone Crossfeed
                Text("Headphone Crossfeed (Chu Moy)", color = AscendanceColors.TextSecondary, fontSize = 12.sp)
                val cfLabels = listOf("Off", "Low (-4.5 dB)", "High (-6.0 dB)")
                SingleChoiceSegmentedButtonRow(modifier = Modifier.fillMaxWidth()) {
                    cfLabels.forEachIndexed { idx, lbl ->
                        SegmentedButton(
                            selected = mixer.crossfeedLevel == idx,
                            onClick = {
                                val nextMode = if (idx > 0 && mixer.mode == MixerMode.STEREO.code) {
                                    MixerMode.CROSSFEED.code
                                } else {
                                    mixer.mode
                                }
                                onUpdateMixer(mixer.copy(mode = nextMode, crossfeedLevel = idx))
                            },
                            shape = SegmentedButtonDefaults.itemShape(index = idx, count = cfLabels.size),
                            label = { Text(lbl, fontSize = 11.sp) }
                        )
                    }
                }
            }
        }

        // Dynamics, Equal Loudness & FIR Impulse Filter
        Surface(
            color = AscendanceColors.CardDark,
            shape = RoundedCornerShape(12.dp),
            border = BorderStroke(1.dp, AscendanceColors.BorderDark)
        ) {
            Column(modifier = Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
                Text("Dynamics & FIR Impulse Filter", color = AscendanceColors.TextSecondary, fontSize = 12.sp, fontWeight = FontWeight.Medium)

                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.SpaceBetween,
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    Column(modifier = Modifier.weight(1f)) {
                        Text("True-Peak Limiter", color = AscendanceColors.TextPrimary, fontSize = 13.sp)
                        Text("1.5 ms lookahead with -0.5 dBFS ceiling", color = AscendanceColors.TextSecondary, fontSize = 11.sp)
                    }
                    Switch(
                        checked = dynamics.limiterEnabled,
                        onCheckedChange = { onUpdateDynamics(dynamics.copy(limiterEnabled = it)) }
                    )
                }

                Text("Equal Loudness (ISO 226, Vol < 80%)", color = AscendanceColors.TextSecondary, fontSize = 12.sp)
                SingleChoiceSegmentedButtonRow(modifier = Modifier.fillMaxWidth()) {
                    LoudnessMode.entries.forEachIndexed { idx, mode ->
                        SegmentedButton(
                            selected = dynamics.loudnessMode == mode.code,
                            onClick = { onUpdateDynamics(dynamics.copy(loudnessMode = mode.code)) },
                            shape = SegmentedButtonDefaults.itemShape(index = idx, count = LoudnessMode.entries.size),
                            label = { Text(mode.label, fontSize = 11.sp) }
                        )
                    }
                }

                val maxCap = if (sampleRateHz > 48000) 128 else 256
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.SpaceBetween,
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    Column(modifier = Modifier.weight(1f)) {
                        Text("FIR Impulse Filter", color = AscendanceColors.TextPrimary, fontSize = 13.sp)
                        Text(
                            "Loaded L:${dynamics.firTapsL} / R:${dynamics.firTapsR} taps (max $maxCap @ ${sampleRateHz / 1000}k)",
                            color = AscendanceColors.TextSecondary,
                            fontSize = 11.sp,
                            fontFamily = FontFamily.Monospace
                        )
                    }
                    Switch(
                        checked = dynamics.firEnabled,
                        onCheckedChange = { onUpdateDynamics(dynamics.copy(firEnabled = it)) }
                    )
                }

                Row(modifier = Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    OutlinedButton(onClick = onUploadFirFile, modifier = Modifier.weight(1f)) {
                        Text("Upload FIR (.txt/.wav)", fontSize = 11.sp)
                    }
                    OutlinedButton(onClick = onClearFir, modifier = Modifier.weight(0.75f)) {
                        Text("Clear FIR", fontSize = 11.sp)
                    }
                }
            }
        }

        // Custom Channel Matrix Grid
        Surface(
            color = AscendanceColors.CardDark,
            shape = RoundedCornerShape(12.dp),
            border = BorderStroke(1.dp, AscendanceColors.BorderDark)
        ) {
            Column(modifier = Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text("Custom Channel Matrix (-4.0 to +4.0)", color = AscendanceColors.TextSecondary, fontSize = 12.sp, fontWeight = FontWeight.Medium)
                Row(modifier = Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    MatrixCoeffField("mLL (L->L)", mixer.customMatrix[0], Modifier.weight(1f)) { v ->
                        val nextMat = mixer.customMatrix.toMutableList().apply { this[0] = v }
                        onUpdateMixer(mixer.copy(mode = MixerMode.CUSTOM_MATRIX.code, customMatrix = nextMat))
                    }
                    MatrixCoeffField("mRL (R->L)", mixer.customMatrix[1], Modifier.weight(1f)) { v ->
                        val nextMat = mixer.customMatrix.toMutableList().apply { this[1] = v }
                        onUpdateMixer(mixer.copy(mode = MixerMode.CUSTOM_MATRIX.code, customMatrix = nextMat))
                    }
                }
                Row(modifier = Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    MatrixCoeffField("mLR (L->R)", mixer.customMatrix[2], Modifier.weight(1f)) { v ->
                        val nextMat = mixer.customMatrix.toMutableList().apply { this[2] = v }
                        onUpdateMixer(mixer.copy(mode = MixerMode.CUSTOM_MATRIX.code, customMatrix = nextMat))
                    }
                    MatrixCoeffField("mRR (R->R)", mixer.customMatrix[3], Modifier.weight(1f)) { v ->
                        val nextMat = mixer.customMatrix.toMutableList().apply { this[3] = v }
                        onUpdateMixer(mixer.copy(mode = MixerMode.CUSTOM_MATRIX.code, customMatrix = nextMat))
                    }
                }
            }
        }
    }
}

@Composable
private fun MatrixCoeffField(
    label: String,
    value: Float,
    modifier: Modifier = Modifier,
    onCommit: (Float) -> Unit
) {
    var text by remember(value) { mutableStateOf(String.format(Locale.US, "%.2f", value)) }
    OutlinedTextField(
        value = text,
        onValueChange = { raw ->
            text = raw
            raw.toFloatOrNull()?.let { parsed ->
                if (parsed.isFinite()) onCommit(parsed.coerceIn(-4.0f, 4.0f))
            }
        },
        label = { Text(label, fontSize = 11.sp) },
        singleLine = true,
        keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Decimal),
        modifier = modifier
    )
}
