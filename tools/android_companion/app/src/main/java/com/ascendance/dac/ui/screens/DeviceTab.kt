package com.ascendance.dac.ui.screens

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
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
import androidx.compose.material.icons.outlined.ListAlt
import androidx.compose.material.icons.outlined.Palette
import androidx.compose.material.icons.outlined.Refresh
import androidx.compose.material.icons.outlined.SettingsBrightness
import androidx.compose.material.icons.outlined.Usb
import androidx.compose.material3.Button
import androidx.compose.material3.FilterChip
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.SegmentedButton
import androidx.compose.material3.SegmentedButtonDefaults
import androidx.compose.material3.SingleChoiceSegmentedButtonRow
import androidx.compose.material3.Surface
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.hapticfeedback.HapticFeedbackType
import androidx.compose.ui.platform.LocalHapticFeedback
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.ascendance.dac.model.OledViewMode
import com.ascendance.dac.ui.DacUiState
import com.ascendance.dac.ui.theme.AppThemeMode
import com.ascendance.dac.ui.theme.AscendanceColors
import java.util.Locale

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
    onSelectThemeMode: (AppThemeMode) -> Unit,
    modifier: Modifier = Modifier
) {
    val colors = MaterialTheme.colorScheme
    val haptics = LocalHapticFeedback.current

    Column(
        modifier = modifier
            .fillMaxWidth()
            .verticalScroll(rememberScrollState()),
        verticalArrangement = Arrangement.spacedBy(10.dp)
    ) {
        // 1. USB OTG Status & HID-Only Coexistence Card
        Surface(
            color = colors.surface,
            shape = RoundedCornerShape(12.dp),
            border = BorderStroke(1.dp, colors.outline)
        ) {
            Column(modifier = Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.SpaceBetween,
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    DeviceSectionHeader(icon = Icons.Outlined.Usb, title = "USB Connection")
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
                        color = colors.onSurface,
                        fontSize = 12.sp
                    )
                    Text(
                        text = "Control Interface #${info.hidInterfaceId} active • Android USB Audio untouched",
                        color = colors.onSurfaceVariant,
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
                        color = colors.onSurfaceVariant,
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
                        Text("Offline Preview Mode", color = colors.onSurface, fontSize = 13.sp)
                        Text("Design and preview curves without DAC attached", color = colors.onSurfaceVariant, fontSize = 11.sp)
                    }
                    Switch(
                        checked = state.offlineDemoMode,
                        onCheckedChange = {
                            haptics.performHapticFeedback(HapticFeedbackType.TextHandleMove)
                            onToggleOfflineDemo(it)
                        },
                        enabled = !state.usbInfo.connected
                    )
                }
            }
        }

        // 2. OLED Playback View & Power / Burn-in Guard Settings
        Surface(
            color = colors.surface,
            shape = RoundedCornerShape(12.dp),
            border = BorderStroke(1.dp, colors.outline)
        ) {
            Column(modifier = Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                DeviceSectionHeader(icon = Icons.Outlined.SettingsBrightness, title = "Display & Power Settings")

                Text("OLED Screen View", color = colors.onSurfaceVariant, fontSize = 11.sp)
                SingleChoiceSegmentedButtonRow(modifier = Modifier.fillMaxWidth()) {
                    OledViewMode.entries.forEachIndexed { idx, view ->
                        SegmentedButton(
                            selected = state.dynamics.oledView == view.code,
                            onClick = {
                                haptics.performHapticFeedback(HapticFeedbackType.TextHandleMove)
                                onSetOledView(view.code)
                            },
                            shape = SegmentedButtonDefaults.itemShape(index = idx, count = OledViewMode.entries.size),
                            label = { Text(view.label, fontSize = 10.sp) }
                        )
                    }
                }

                Text("Auto-Mute on Silence", color = colors.onSurfaceVariant, fontSize = 11.sp)
                Row(modifier = Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                    listOf(0 to "Off", 10 to "10s", 30 to "30s", 60 to "60s").forEach { (sec, lbl) ->
                        FilterChip(
                            selected = state.dynamics.autoMuteSec == sec,
                            onClick = {
                                haptics.performHapticFeedback(HapticFeedbackType.TextHandleMove)
                                onSetPowerConfig(sec, state.dynamics.oledDimSec)
                            },
                            label = { Text(lbl, fontSize = 11.sp) },
                            modifier = Modifier.weight(1f)
                        )
                    }
                }

                Text("OLED Auto-Dim & Sleep", color = colors.onSurfaceVariant, fontSize = 11.sp)
                Row(modifier = Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                    listOf(0 to "Off", 60 to "60s", 120 to "120s").forEach { (sec, lbl) ->
                        FilterChip(
                            selected = state.dynamics.oledDimSec == sec,
                            onClick = {
                                haptics.performHapticFeedback(HapticFeedbackType.TextHandleMove)
                                onSetPowerConfig(state.dynamics.autoMuteSec, sec)
                            },
                            label = { Text(lbl, fontSize = 11.sp) },
                            modifier = Modifier.weight(1f)
                        )
                    }
                }
            }
        }

        // 3. Appearance (App Theme Switcher)
        Surface(
            color = colors.surface,
            shape = RoundedCornerShape(12.dp),
            border = BorderStroke(1.dp, colors.outline)
        ) {
            Column(modifier = Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                DeviceSectionHeader(icon = Icons.Outlined.Palette, title = "Appearance")
                Text(
                    "Choose the app palette. Dynamic uses Android Material You colors on Android 12+.",
                    color = colors.onSurfaceVariant,
                    fontSize = 11.sp
                )
                SingleChoiceSegmentedButtonRow(modifier = Modifier.fillMaxWidth()) {
                    AppThemeMode.entries.forEachIndexed { idx, mode ->
                        SegmentedButton(
                            selected = state.themeMode == mode,
                            onClick = {
                                haptics.performHapticFeedback(HapticFeedbackType.TextHandleMove)
                                onSelectThemeMode(mode)
                            },
                            shape = SegmentedButtonDefaults.itemShape(index = idx, count = AppThemeMode.entries.size),
                            label = { Text(mode.label, fontSize = 11.sp) }
                        )
                    }
                }
            }
        }

        // 4. Activity Log
        Surface(
            color = colors.surface,
            shape = RoundedCornerShape(12.dp),
            border = BorderStroke(1.dp, colors.outline)
        ) {
            Column(modifier = Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                DeviceSectionHeader(icon = Icons.Outlined.ListAlt, title = "Activity Log")
                Column(
                    modifier = Modifier
                        .fillMaxWidth()
                        .height(170.dp)
                        .background(colors.background, RoundedCornerShape(8.dp))
                        .padding(8.dp)
                        .verticalScroll(rememberScrollState())
                ) {
                    Text(
                        text = state.logs.joinToString("\n"),
                        color = colors.onSurfaceVariant,
                        fontSize = 11.sp,
                        fontFamily = FontFamily.Monospace,
                        lineHeight = 15.sp
                    )
                }
            }
        }
    }
}

@Composable
private fun DeviceSectionHeader(
    icon: ImageVector,
    title: String
) {
    val colors = MaterialTheme.colorScheme
    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(6.dp)) {
        Icon(
            imageVector = icon,
            contentDescription = null,
            tint = colors.primary,
            modifier = Modifier.size(16.dp)
        )
        Text(
            text = title,
            color = colors.onSurfaceVariant,
            fontSize = 12.sp,
            fontWeight = FontWeight.Medium
        )
    }
}
