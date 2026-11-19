package com.ascendance.dac.ui.screens

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.outlined.Refresh
import androidx.compose.material.icons.outlined.Usb
import androidx.compose.material3.Button
import androidx.compose.material3.FilterChip
import androidx.compose.material3.Icon
import androidx.compose.material3.OutlinedButton
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
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.viewmodel.compose.viewModel
import com.ascendance.dac.model.DspDefaults
import com.ascendance.dac.model.ExciterMode
import com.ascendance.dac.model.HarmonicsConfig
import com.ascendance.dac.model.OledViewMode
import com.ascendance.dac.ui.DacUiState
import com.ascendance.dac.ui.DacViewModel
import com.ascendance.dac.ui.theme.AscendanceColors
import java.util.Locale
import kotlin.math.roundToInt

@Composable
fun DeviceTab(
    state: DacUiState,
    onConnectUsb: () -> Unit,
    onDisconnectUsb: () -> Unit,
    onSyncFromHardware: () -> Unit,
    onPushDraftLive: () -> Unit,
    onPushDraftAndSave: () -> Unit,
    onToggleOfflineDemo: (Boolean) -> Unit,
    onSetOledView: (Int) -> Unit,
    onSetPowerConfig: (Int, Int) -> Unit,
    modifier: Modifier = Modifier,
    onUpdateHarmonics: ((HarmonicsConfig) -> Unit)? = null
) {
    val resolvedUpdateHarmonics: (HarmonicsConfig) -> Unit =
        if (onUpdateHarmonics != null) {
            onUpdateHarmonics
        } else {
            val vm: DacViewModel = viewModel()
            vm::updateHarmonics
        }

    Column(
        modifier = modifier
            .fillMaxWidth()
            .verticalScroll(rememberScrollState()),
        verticalArrangement = Arrangement.spacedBy(10.dp)
    ) {
        // USB OTG Status & HID-Only Coexistence Card
        Surface(
            color = AscendanceColors.CardDark,
            shape = RoundedCornerShape(12.dp),
            border = BorderStroke(1.dp, AscendanceColors.BorderDark)
        ) {
            Column(modifier = Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.SpaceBetween,
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    Text("USB Connection", color = AscendanceColors.TextSecondary, fontSize = 12.sp, fontWeight = FontWeight.Medium)
                    Text(
                        text = if (state.usbInfo.connected) "CONNECTED" else "DISCONNECTED",
                        color = if (state.usbInfo.connected) AscendanceColors.AccentGreen else AscendanceColors.AccentRed,
                        fontSize = 11.sp,
                        fontFamily = FontFamily.Monospace,
                        fontWeight = FontWeight.Bold
                    )
                }

                if (state.usbInfo.connected) {
                    val info = state.usbInfo
                    Text(
                        text = "${info.productName} (VID:${
                            String.format(Locale.US, "%04X", info.vendorId)
                        } PID:${String.format(Locale.US, "%04X", info.productId)})",
                        color = AscendanceColors.TextPrimary,
                        fontSize = 12.sp
                    )
                    Text(
                        text = "Control Interface #${info.hidInterfaceId} active • Android USB Audio untouched",
                        color = AscendanceColors.TextSecondary,
                        fontSize = 11.sp,
                        fontFamily = FontFamily.Monospace
                    )
                    Text(
                        text = "Active Audio Format: ${
                            String.format(Locale.US, "%.1f kHz / 24-bit", state.sampleRateHz / 1000.0f)
                        }",
                        color = AscendanceColors.AccentBlue,
                        fontSize = 12.sp,
                        fontFamily = FontFamily.Monospace
                    )
                } else {
                    Text(
                        text = "Connect your ASCENDANCE DAC via USB-C. Only the control interface is claimed so Android music playback stays uninterrupted.",
                        color = AscendanceColors.TextSecondary,
                        fontSize = 12.sp
                    )
                }

                Row(modifier = Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    if (state.usbInfo.connected) {
                        OutlinedButton(onClick = onDisconnectUsb, modifier = Modifier.weight(1f)) {
                            Text("Disconnect", fontSize = 12.sp)
                        }
                        Button(onClick = onSyncFromHardware, modifier = Modifier.weight(1f)) {
                            Icon(Icons.Outlined.Refresh, contentDescription = null, modifier = Modifier.size(16.dp))
                            Spacer(Modifier.width(4.dp))
                            Text("Sync from DAC", fontSize = 12.sp)
                        }
                    } else {
                        Button(onClick = onConnectUsb, modifier = Modifier.fillMaxWidth()) {
                            Icon(Icons.Outlined.Usb, contentDescription = null, modifier = Modifier.size(16.dp))
                            Spacer(Modifier.width(6.dp))
                            Text("Connect USB DAC", fontSize = 12.sp)
                        }
                    }
                }

                if (state.usbInfo.connected && state.pendingOfflineDraft) {
                    Surface(
                        color = AscendanceColors.AccentAmber.copy(alpha = 0.12f),
                        shape = RoundedCornerShape(8.dp),
                        border = BorderStroke(1.dp, AscendanceColors.AccentAmber.copy(alpha = 0.5f))
                    ) {
                        Column(modifier = Modifier.padding(10.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                            Text(
                                "Offline draft curve ready to sync to DAC:",
                                color = AscendanceColors.AccentAmber,
                                fontSize = 12.sp,
                                fontWeight = FontWeight.Medium
                            )
                            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                                OutlinedButton(onClick = onPushDraftLive, modifier = Modifier.weight(1f)) {
                                    Text("Preview Live", fontSize = 11.sp)
                                }
                                Button(onClick = onPushDraftAndSave, modifier = Modifier.weight(1f)) {
                                    Text("Save to DAC", fontSize = 11.sp)
                                }
                            }
                        }
                    }
                }

                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.SpaceBetween,
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    Column(modifier = Modifier.weight(1f)) {
                        Text("Offline Preview Mode", color = AscendanceColors.TextPrimary, fontSize = 13.sp)
                        Text("Design and preview curves without DAC attached", color = AscendanceColors.TextSecondary, fontSize = 11.sp)
                    }
                    Switch(
                        checked = state.offlineDemoMode,
                        onCheckedChange = onToggleOfflineDemo,
                        enabled = !state.usbInfo.connected
                    )
                }
            }
        }

        // Psychoacoustic Bass Enhancer & Harmonic Exciter
        val harm = state.harmonics
        Surface(
            color = AscendanceColors.CardDark,
            shape = RoundedCornerShape(12.dp),
            border = BorderStroke(1.dp, AscendanceColors.BorderDark)
        ) {
            Column(modifier = Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text(
                    text = "Bass Enhancer & Harmonic Exciter",
                    color = AscendanceColors.TextSecondary,
                    fontSize = 12.sp,
                    fontWeight = FontWeight.Medium
                )

                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.SpaceBetween,
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    Column(modifier = Modifier.weight(1f)) {
                        Text("Bass Enhancer", color = AscendanceColors.TextPrimary, fontSize = 13.sp)
                        Text(
                            "Psychoacoustic missing-fundamental low-end warmth",
                            color = AscendanceColors.TextSecondary,
                            fontSize = 11.sp
                        )
                    }
                    Switch(
                        checked = harm.bassEnabled,
                        onCheckedChange = { resolvedUpdateHarmonics(harm.copy(bassEnabled = it)) }
                    )
                }

                Row(modifier = Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
                    Text("Bass Drive", color = AscendanceColors.TextSecondary, fontSize = 11.sp)
                    Text(
                        text = "${harm.bassAmountPct}%",
                        color = if (harm.bassEnabled) AscendanceColors.AccentBlue else AscendanceColors.TextSecondary,
                        fontFamily = FontFamily.Monospace,
                        fontSize = 11.sp
                    )
                }
                Slider(
                    value = harm.bassAmountPct.toFloat(),
                    onValueChange = { resolvedUpdateHarmonics(harm.copy(bassAmountPct = it.roundToInt())) },
                    valueRange = DspDefaults.MIN_HARMONICS_AMOUNT_PCT.toFloat()..DspDefaults.MAX_HARMONICS_AMOUNT_PCT.toFloat(),
                    colors = SliderDefaults.colors(
                        thumbColor = AscendanceColors.AccentBlue,
                        activeTrackColor = AscendanceColors.AccentBlue
                    )
                )

                Row(modifier = Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
                    Text("Cutoff (50–150 Hz)", color = AscendanceColors.TextSecondary, fontSize = 11.sp)
                    Text(
                        text = "${harm.bassFreqHz} Hz",
                        color = if (harm.bassEnabled) AscendanceColors.AccentBlue else AscendanceColors.TextSecondary,
                        fontFamily = FontFamily.Monospace,
                        fontSize = 11.sp
                    )
                }
                Slider(
                    value = harm.bassFreqHz.toFloat(),
                    onValueChange = { resolvedUpdateHarmonics(harm.copy(bassFreqHz = it.roundToInt())) },
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
                        .background(AscendanceColors.BorderDark)
                )

                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.SpaceBetween,
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    Column(modifier = Modifier.weight(1f)) {
                        Text("Harmonic Exciter", color = AscendanceColors.TextPrimary, fontSize = 13.sp)
                        Text(
                            "Upper-harmonic saturation & presence sheen",
                            color = AscendanceColors.TextSecondary,
                            fontSize = 11.sp
                        )
                    }
                    Switch(
                        checked = harm.exciterEnabled,
                        onCheckedChange = { resolvedUpdateHarmonics(harm.copy(exciterEnabled = it)) }
                    )
                }

                SingleChoiceSegmentedButtonRow(modifier = Modifier.fillMaxWidth()) {
                    ExciterMode.entries.forEachIndexed { idx, mode ->
                        SegmentedButton(
                            selected = harm.exciterMode == mode.code,
                            onClick = { resolvedUpdateHarmonics(harm.copy(exciterMode = mode.code)) },
                            shape = SegmentedButtonDefaults.itemShape(index = idx, count = ExciterMode.entries.size),
                            label = { Text(mode.label, fontSize = 11.sp) }
                        )
                    }
                }

                Row(modifier = Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
                    Text("Excite", color = AscendanceColors.TextSecondary, fontSize = 11.sp)
                    Text(
                        text = "${harm.exciterAmountPct}%",
                        color = if (harm.exciterEnabled) AscendanceColors.AccentAmber else AscendanceColors.TextSecondary,
                        fontFamily = FontFamily.Monospace,
                        fontSize = 11.sp
                    )
                }
                Slider(
                    value = harm.exciterAmountPct.toFloat(),
                    onValueChange = { resolvedUpdateHarmonics(harm.copy(exciterAmountPct = it.roundToInt())) },
                    valueRange = DspDefaults.MIN_HARMONICS_AMOUNT_PCT.toFloat()..DspDefaults.MAX_HARMONICS_AMOUNT_PCT.toFloat(),
                    colors = SliderDefaults.colors(
                        thumbColor = AscendanceColors.AccentAmber,
                        activeTrackColor = AscendanceColors.AccentAmber
                    )
                )

                Row(modifier = Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
                    Text("Cutoff (1.5–8.0 kHz)", color = AscendanceColors.TextSecondary, fontSize = 11.sp)
                    Text(
                        text = String.format(Locale.US, "%.1f kHz", harm.exciterFreqHz / 1000.0f),
                        color = if (harm.exciterEnabled) AscendanceColors.AccentAmber else AscendanceColors.TextSecondary,
                        fontFamily = FontFamily.Monospace,
                        fontSize = 11.sp
                    )
                }
                Slider(
                    value = harm.exciterFreqHz.toFloat(),
                    onValueChange = {
                        val hz = (it / 100f).roundToInt() * 100
                        resolvedUpdateHarmonics(harm.copy(exciterFreqHz = hz))
                    },
                    valueRange = DspDefaults.MIN_EXCITER_FREQ_HZ.toFloat()..DspDefaults.MAX_EXCITER_FREQ_HZ.toFloat(),
                    colors = SliderDefaults.colors(
                        thumbColor = AscendanceColors.AccentAmber,
                        activeTrackColor = AscendanceColors.AccentAmber
                    )
                )
            }
        }

        // OLED Playback View & Power / Burn-in Guard Settings
        Surface(
            color = AscendanceColors.CardDark,
            shape = RoundedCornerShape(12.dp),
            border = BorderStroke(1.dp, AscendanceColors.BorderDark)
        ) {
            Column(modifier = Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text("Display & Power Settings", color = AscendanceColors.TextSecondary, fontSize = 12.sp, fontWeight = FontWeight.Medium)

                Text("OLED Screen View", color = AscendanceColors.TextSecondary, fontSize = 11.sp)
                SingleChoiceSegmentedButtonRow(modifier = Modifier.fillMaxWidth()) {
                    OledViewMode.entries.forEachIndexed { idx, view ->
                        SegmentedButton(
                            selected = state.dynamics.oledView == view.code,
                            onClick = { onSetOledView(view.code) },
                            shape = SegmentedButtonDefaults.itemShape(index = idx, count = OledViewMode.entries.size),
                            label = { Text(view.label, fontSize = 10.sp) }
                        )
                    }
                }

                Text("Auto-Mute on Silence", color = AscendanceColors.TextSecondary, fontSize = 11.sp)
                Row(modifier = Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                    listOf(0 to "Off", 10 to "10s", 30 to "30s", 60 to "60s").forEach { (sec, lbl) ->
                        FilterChip(
                            selected = state.dynamics.autoMuteSec == sec,
                            onClick = { onSetPowerConfig(sec, state.dynamics.oledDimSec) },
                            label = { Text(lbl, fontSize = 11.sp) },
                            modifier = Modifier.weight(1f)
                        )
                    }
                }

                Text("OLED Auto-Dim & Sleep", color = AscendanceColors.TextSecondary, fontSize = 11.sp)
                Row(modifier = Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                    listOf(0 to "Off", 60 to "60s", 120 to "120s").forEach { (sec, lbl) ->
                        FilterChip(
                            selected = state.dynamics.oledDimSec == sec,
                            onClick = { onSetPowerConfig(state.dynamics.autoMuteSec, sec) },
                            label = { Text(lbl, fontSize = 11.sp) },
                            modifier = Modifier.weight(1f)
                        )
                    }
                }
            }
        }

        // Activity Log
        Surface(
            color = AscendanceColors.CardDark,
            shape = RoundedCornerShape(12.dp),
            border = BorderStroke(1.dp, AscendanceColors.BorderDark)
        ) {
            Column(modifier = Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                Text("Activity Log", color = AscendanceColors.TextSecondary, fontSize = 12.sp, fontWeight = FontWeight.Medium)
                Column(
                    modifier = Modifier
                        .fillMaxWidth()
                        .height(170.dp)
                        .background(Color(0xFF0A0D12), RoundedCornerShape(8.dp))
                        .padding(8.dp)
                        .verticalScroll(rememberScrollState())
                ) {
                    Text(
                        text = state.logs.joinToString("\n"),
                        color = Color(0xFF8B949E),
                        fontSize = 11.sp,
                        fontFamily = FontFamily.Monospace,
                        lineHeight = 15.sp
                    )
                }
            }
        }
    }
}
