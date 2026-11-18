package com.ascendance.dac.ui.screens

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.outlined.Add
import androidx.compose.material.icons.outlined.Delete
import androidx.compose.material.icons.outlined.Remove
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.FilterChip
import androidx.compose.material3.FilterChipDefaults
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.SegmentedButton
import androidx.compose.material3.SegmentedButtonDefaults
import androidx.compose.material3.SingleChoiceSegmentedButtonRow
import androidx.compose.material3.Slider
import androidx.compose.material3.SliderDefaults
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.ascendance.dac.model.DspDefaults
import com.ascendance.dac.model.FilterType
import com.ascendance.dac.model.PeqBand
import com.ascendance.dac.model.PeqChannel
import com.ascendance.dac.ui.DacUiState
import com.ascendance.dac.ui.theme.AscendanceColors
import java.util.Locale
import kotlin.math.ln
import kotlin.math.exp
import kotlin.math.roundToInt

private enum class NumericEditField { FREQ, GAIN, Q }

@Composable
fun EqTab(
    state: DacUiState,
    onSelectChannel: (PeqChannel) -> Unit,
    onSelectBand: (Int) -> Unit,
    onUpdateBand: (Int, PeqBand) -> Unit,
    onAddBand: () -> Unit,
    onRemoveBand: (Int) -> Unit,
    onUpdatePreamp: (Float) -> Unit,
    modifier: Modifier = Modifier
) {
    val bands = state.activeBands
    val selIdx = state.selectedBandIdx.coerceIn(0, (bands.size - 1).coerceAtLeast(0))
    val selBand = bands.getOrNull(selIdx)

    var showTypeMenu by remember { mutableStateOf(false) }
    var activeEditField by remember { mutableStateOf<NumericEditField?>(null) }
    var editInputText by remember { mutableStateOf("") }

    Column(
        modifier = modifier
            .fillMaxWidth()
            .verticalScroll(rememberScrollState()),
        verticalArrangement = Arrangement.spacedBy(10.dp)
    ) {
        // Dual-Mono Channel Selector + Band Count
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
                    Text(
                        text = "Channel Mode",
                        color = AscendanceColors.TextSecondary,
                        fontSize = 12.sp,
                        fontWeight = FontWeight.Medium
                    )
                    Text(
                        text = "${bands.size} / ${DspDefaults.MAX_BANDS} Bands",
                        color = AscendanceColors.AccentBlue,
                        fontSize = 12.sp,
                        fontFamily = FontFamily.Monospace
                    )
                }

                SingleChoiceSegmentedButtonRow(modifier = Modifier.fillMaxWidth()) {
                    PeqChannel.entries.forEachIndexed { idx, ch ->
                        val isSelected = if (state.linkedStereo) ch == PeqChannel.LINKED else state.selectedChannel == ch
                        SegmentedButton(
                            selected = isSelected,
                            onClick = { onSelectChannel(ch) },
                            shape = SegmentedButtonDefaults.itemShape(index = idx, count = PeqChannel.entries.size),
                            label = { Text(ch.label, fontSize = 12.sp) }
                        )
                    }
                }

                // Preamp Headroom slider
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.SpaceBetween,
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    Text("Preamp Headroom", color = AscendanceColors.TextSecondary, fontSize = 12.sp)
                    Text(
                        text = String.format(Locale.US, "%+.1f dB", state.preampDb),
                        color = AscendanceColors.TextPrimary,
                        fontSize = 12.sp,
                        fontFamily = FontFamily.Monospace,
                        fontWeight = FontWeight.SemiBold
                    )
                }
                Slider(
                    value = state.preampDb,
                    onValueChange = {
                        val step = (it * 10f).roundToInt() / 10f
                        onUpdatePreamp(step)
                    },
                    valueRange = DspDefaults.MIN_PREAMP_DB..DspDefaults.MAX_PREAMP_DB,
                    colors = SliderDefaults.colors(
                        thumbColor = AscendanceColors.AccentAmber,
                        activeTrackColor = AscendanceColors.AccentAmber
                    )
                )
            }
        }

        // Scrollable Band Pills + Add/Remove
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .horizontalScroll(rememberScrollState()),
            horizontalArrangement = Arrangement.spacedBy(6.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            bands.forEachIndexed { idx, band ->
                val bandColor = Color(DspDefaults.BAND_COLORS_ARGB[idx % DspDefaults.BAND_COLORS_ARGB.size])
                val isSel = idx == selIdx
                FilterChip(
                    selected = isSel,
                    onClick = { onSelectBand(idx) },
                    label = {
                        Text(
                            text = "B${idx + 1} ${band.freq}Hz",
                            fontSize = 11.sp,
                            fontFamily = FontFamily.Monospace
                        )
                    },
                    colors = FilterChipDefaults.filterChipColors(
                        selectedContainerColor = bandColor.copy(alpha = 0.24f),
                        selectedLabelColor = bandColor
                    ),
                    border = FilterChipDefaults.filterChipBorder(
                        enabled = true,
                        selected = isSel,
                        borderColor = AscendanceColors.BorderDark,
                        selectedBorderColor = bandColor
                    )
                )
            }

            if (bands.size < DspDefaults.MAX_BANDS) {
                FilledTonalButton(
                    onClick = onAddBand,
                    contentPadding = PaddingValues(horizontal = 10.dp, vertical = 4.dp),
                    modifier = Modifier.height(32.dp)
                ) {
                    Icon(Icons.Outlined.Add, contentDescription = "Add Band", modifier = Modifier.size(16.dp))
                    Spacer(Modifier.width(4.dp))
                    Text("Add", fontSize = 11.sp)
                }
            }
        }

        // Selected Band Detail Card
        if (selBand != null) {
            val bandColor = Color(DspDefaults.BAND_COLORS_ARGB[selIdx % DspDefaults.BAND_COLORS_ARGB.size])
            Surface(
                color = AscendanceColors.CardDark,
                shape = RoundedCornerShape(12.dp),
                border = BorderStroke(1.dp, bandColor.copy(alpha = 0.45f))
            ) {
                Column(modifier = Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    Row(
                        modifier = Modifier.fillMaxWidth(),
                        horizontalArrangement = Arrangement.SpaceBetween,
                        verticalAlignment = Alignment.CenterVertically
                    ) {
                        Text(
                            text = "Band ${selIdx + 1} (${state.selectedChannel.label})",
                            color = bandColor,
                            fontWeight = FontWeight.Bold,
                            fontSize = 14.sp
                        )

                        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                            Box {
                                OutlinedButton(
                                    onClick = { showTypeMenu = true },
                                    contentPadding = PaddingValues(horizontal = 10.dp, vertical = 4.dp),
                                    modifier = Modifier.height(32.dp)
                                ) {
                                    Text(selBand.filterType.label, fontSize = 12.sp)
                                }
                                DropdownMenu(
                                    expanded = showTypeMenu,
                                    onDismissRequest = { showTypeMenu = false }
                                ) {
                                    FilterType.entries.forEach { ft ->
                                        DropdownMenuItem(
                                            text = { Text("${ft.label} (${ft.shortLabel})") },
                                            onClick = {
                                                showTypeMenu = false
                                                onUpdateBand(selIdx, selBand.copy(type = ft.code))
                                            }
                                        )
                                    }
                                }
                            }

                            if (bands.size > DspDefaults.MIN_BANDS) {
                                IconButton(
                                    onClick = { onRemoveBand(selIdx) },
                                    modifier = Modifier.size(32.dp)
                                ) {
                                    Icon(
                                        Icons.Outlined.Delete,
                                        contentDescription = "Delete Band",
                                        tint = AscendanceColors.AccentRed,
                                        modifier = Modifier.size(18.dp)
                                    )
                                }
                            }
                        }
                    }

                    // Frequency (Log slider + steppers)
                    val logMin = ln(20.0)
                    val logMax = ln(20000.0)
                    val freqFrac = ((ln(selBand.freq.toDouble().coerceIn(20.0, 20000.0)) - logMin) / (logMax - logMin)).toFloat()

                    ParameterRow(
                        label = "Frequency",
                        valueText = "${selBand.freq} Hz",
                        onTapValue = {
                            editInputText = selBand.freq.toString()
                            activeEditField = NumericEditField.FREQ
                        },
                        onDecrement = {
                            val step = when {
                                selBand.freq > 5000 -> 100
                                selBand.freq > 1000 -> 25
                                else -> 5
                            }
                            onUpdateBand(selIdx, selBand.copy(freq = (selBand.freq - step).coerceAtLeast(20)))
                        },
                        onIncrement = {
                            val step = when {
                                selBand.freq >= 5000 -> 100
                                selBand.freq >= 1000 -> 25
                                else -> 5
                            }
                            onUpdateBand(selIdx, selBand.copy(freq = (selBand.freq + step).coerceAtMost(20000)))
                        }
                    )
                    Slider(
                        value = freqFrac,
                        onValueChange = { frac ->
                            val hz = exp(logMin + frac.coerceIn(0f, 1f) * (logMax - logMin)).roundToInt()
                            onUpdateBand(selIdx, selBand.copy(freq = hz))
                        },
                        colors = SliderDefaults.colors(thumbColor = bandColor, activeTrackColor = bandColor)
                    )

                    // Gain (only for Peaking, Low Shelf, High Shelf)
                    val usesGain = selBand.filterType.usesGain
                    ParameterRow(
                        label = if (usesGain) "Gain" else "Gain (Fixed 0 dB for ${selBand.filterType.shortLabel})",
                        valueText = String.format(Locale.US, "%+.1f dB", selBand.gain),
                        enabled = usesGain,
                        onTapValue = {
                            if (usesGain) {
                                editInputText = String.format(Locale.US, "%.1f", selBand.gain)
                                activeEditField = NumericEditField.GAIN
                            }
                        },
                        onDecrement = {
                            if (usesGain) {
                                val next = ((selBand.gain - 0.1f) * 10f).roundToInt() / 10f
                                onUpdateBand(selIdx, selBand.copy(gain = next))
                            }
                        },
                        onIncrement = {
                            if (usesGain) {
                                val next = ((selBand.gain + 0.1f) * 10f).roundToInt() / 10f
                                onUpdateBand(selIdx, selBand.copy(gain = next))
                            }
                        }
                    )
                    Slider(
                        value = selBand.gain,
                        enabled = usesGain,
                        onValueChange = { g ->
                            val rounded = (g * 10f).roundToInt() / 10f
                            onUpdateBand(selIdx, selBand.copy(gain = rounded))
                        },
                        valueRange = DspDefaults.MIN_GAIN_DB..DspDefaults.MAX_GAIN_DB,
                        colors = SliderDefaults.colors(thumbColor = bandColor, activeTrackColor = bandColor)
                    )

                    // Q Factor
                    ParameterRow(
                        label = "Q Factor",
                        valueText = String.format(Locale.US, "%.2f", selBand.q),
                        onTapValue = {
                            editInputText = String.format(Locale.US, "%.2f", selBand.q)
                            activeEditField = NumericEditField.Q
                        },
                        onDecrement = {
                            val next = ((selBand.q - 0.05f) * 100f).roundToInt() / 100f
                            onUpdateBand(selIdx, selBand.copy(q = next))
                        },
                        onIncrement = {
                            val next = ((selBand.q + 0.05f) * 100f).roundToInt() / 100f
                            onUpdateBand(selIdx, selBand.copy(q = next))
                        }
                    )
                    Slider(
                        value = selBand.q,
                        onValueChange = { q ->
                            val rounded = (q * 100f).roundToInt() / 100f
                            onUpdateBand(selIdx, selBand.copy(q = rounded))
                        },
                        valueRange = DspDefaults.MIN_Q..DspDefaults.MAX_Q,
                        colors = SliderDefaults.colors(thumbColor = bandColor, activeTrackColor = bandColor)
                    )
                }
            }
        }
    }

    // Exact numeric input dialog when tapping Freq/Gain/Q value
    val editField = activeEditField
    if (editField != null && selBand != null) {
        val title = when (editField) {
            NumericEditField.FREQ -> "Enter Frequency (20–20000 Hz)"
            NumericEditField.GAIN -> "Enter Gain (-24.0 to +12.0 dB)"
            NumericEditField.Q -> "Enter Q Factor (0.10 to 10.00)"
        }
        AlertDialog(
            onDismissRequest = { activeEditField = null },
            title = { Text(title, fontSize = 15.sp) },
            text = {
                OutlinedTextField(
                    value = editInputText,
                    onValueChange = { editInputText = it },
                    singleLine = true,
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Decimal),
                    modifier = Modifier.fillMaxWidth()
                )
            },
            confirmButton = {
                TextButton(
                    onClick = {
                        val parsed = editInputText.toFloatOrNull()
                        if (parsed != null) {
                            when (editField) {
                                NumericEditField.FREQ -> onUpdateBand(selIdx, selBand.copy(freq = parsed.roundToInt()))
                                NumericEditField.GAIN -> onUpdateBand(selIdx, selBand.copy(gain = parsed))
                                NumericEditField.Q -> onUpdateBand(selIdx, selBand.copy(q = parsed))
                            }
                        }
                        activeEditField = null
                    }
                ) { Text("Apply") }
            },
            dismissButton = {
                TextButton(onClick = { activeEditField = null }) { Text("Cancel") }
            }
        )
    }
}

@Composable
private fun ParameterRow(
    label: String,
    valueText: String,
    enabled: Boolean = true,
    onTapValue: () -> Unit,
    onDecrement: () -> Unit,
    onIncrement: () -> Unit
) {
    Row(
        modifier = Modifier.fillMaxWidth(),
        horizontalArrangement = Arrangement.SpaceBetween,
        verticalAlignment = Alignment.CenterVertically
    ) {
        Text(
            text = label,
            color = if (enabled) AscendanceColors.TextSecondary else AscendanceColors.TextSecondary.copy(alpha = 0.45f),
            fontSize = 12.sp
        )
        Row(verticalAlignment = Alignment.CenterVertically) {
            IconButton(onClick = onDecrement, enabled = enabled, modifier = Modifier.size(28.dp)) {
                Icon(Icons.Outlined.Remove, contentDescription = "Decrease $label", modifier = Modifier.size(16.dp))
            }
            TextButton(
                onClick = onTapValue,
                enabled = enabled,
                contentPadding = PaddingValues(horizontal = 8.dp, vertical = 2.dp),
                colors = ButtonDefaults.textButtonColors(contentColor = AscendanceColors.TextPrimary)
            ) {
                Text(valueText, fontFamily = FontFamily.Monospace, fontSize = 13.sp, fontWeight = FontWeight.SemiBold)
            }
            IconButton(onClick = onIncrement, enabled = enabled, modifier = Modifier.size(28.dp)) {
                Icon(Icons.Outlined.Add, contentDescription = "Increase $label", modifier = Modifier.size(16.dp))
            }
        }
    }
}
