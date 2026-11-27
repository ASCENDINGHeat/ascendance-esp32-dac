package com.ascendance.dac.ui.screens

import androidx.compose.animation.AnimatedVisibility
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.outlined.ExpandLess
import androidx.compose.material.icons.outlined.ExpandMore
import androidx.compose.material.icons.outlined.Headphones
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.SegmentedButton
import androidx.compose.material3.SegmentedButtonDefaults
import androidx.compose.material3.SingleChoiceSegmentedButtonRow
import androidx.compose.material3.Slider
import androidx.compose.material3.SliderDefaults
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.hapticfeedback.HapticFeedbackType
import androidx.compose.ui.platform.LocalHapticFeedback
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.ascendance.dac.model.MixerConfig
import com.ascendance.dac.model.MixerMode
import com.ascendance.dac.ui.theme.AscendanceColors
import java.util.Locale
import kotlin.math.roundToInt

@Composable
internal fun ToneMatrixCrossfeedCard(
    mixer: MixerConfig,
    onUpdateMixer: (MixerConfig) -> Unit
) {
    val colors = MaterialTheme.colorScheme
    val haptics = LocalHapticFeedback.current
    var showModeMenu by remember { mutableStateOf(false) }
    var customMatrixExpanded by rememberSaveable {
        mutableStateOf(mixer.mode == MixerMode.CUSTOM_MATRIX.code)
    }

    Surface(
        color = colors.surface,
        shape = RoundedCornerShape(12.dp),
        border = BorderStroke(1.dp, colors.outline)
    ) {
        Column(modifier = Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
            ToneSectionHeader(
                icon = Icons.Outlined.Headphones,
                title = "Channel Matrix & Crossfeed"
            )

            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically
            ) {
                Text("Routing Mode", color = colors.onSurfaceVariant, fontSize = 13.sp)
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
                                    haptics.performHapticFeedback(HapticFeedbackType.TextHandleMove)
                                    val nextCrossfeed = if (mode == MixerMode.CROSSFEED && mixer.crossfeedLevel == 0) {
                                        1
                                    } else {
                                        mixer.crossfeedLevel
                                    }
                                    if (mode == MixerMode.CUSTOM_MATRIX) {
                                        customMatrixExpanded = true
                                    }
                                    onUpdateMixer(mixer.copy(mode = mode.code, crossfeedLevel = nextCrossfeed))
                                }
                            )
                        }
                    }
                }
            }

            val balLabel = when {
                mixer.balance == 0 -> "Center (0)"
                mixer.balance < 0 -> "Left ${-mixer.balance}%"
                else -> "Right +${mixer.balance}%"
            }
            Row(modifier = Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
                Text("Channel Balance (L / R)", color = colors.onSurfaceVariant, fontSize = 12.sp)
                Text(balLabel, color = AscendanceColors.AccentBlue, fontFamily = FontFamily.Monospace, fontSize = 12.sp)
            }
            Slider(
                value = mixer.balance.toFloat(),
                onValueChange = { onUpdateMixer(mixer.copy(balance = it.roundToInt())) },
                valueRange = -100f..100f
            )

            Row(modifier = Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
                Text("M/S Stereo Width", color = colors.onSurfaceVariant, fontSize = 12.sp)
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

            Text("Headphone Crossfeed (Chu Moy)", color = colors.onSurfaceVariant, fontSize = 12.sp)
            val cfLabels = listOf("Off", "Low (-4.5 dB)", "High (-6.0 dB)")
            SingleChoiceSegmentedButtonRow(modifier = Modifier.fillMaxWidth()) {
                cfLabels.forEachIndexed { idx, lbl ->
                    SegmentedButton(
                        selected = mixer.crossfeedLevel == idx,
                        onClick = {
                            haptics.performHapticFeedback(HapticFeedbackType.TextHandleMove)
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

            Box(
                modifier = Modifier
                    .fillMaxWidth()
                    .height(1.dp)
                    .background(colors.outline)
            )

            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .clickable { customMatrixExpanded = !customMatrixExpanded }
                    .padding(vertical = 2.dp),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically
            ) {
                Text(
                    "Custom 2×2 Matrix (-4.0 to +4.0)",
                    color = colors.onSurfaceVariant,
                    fontSize = 12.sp,
                    fontWeight = FontWeight.Medium
                )
                Icon(
                    imageVector = if (customMatrixExpanded) Icons.Outlined.ExpandLess else Icons.Outlined.ExpandMore,
                    contentDescription = if (customMatrixExpanded) "Collapse Custom Matrix" else "Expand Custom Matrix",
                    tint = colors.onSurfaceVariant,
                    modifier = Modifier.size(18.dp)
                )
            }

            AnimatedVisibility(visible = customMatrixExpanded) {
                Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
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
