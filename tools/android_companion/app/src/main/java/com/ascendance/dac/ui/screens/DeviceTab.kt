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
import androidx.compose.material.icons.outlined.Refresh
import androidx.compose.material.icons.outlined.Usb
import androidx.compose.material3.Button
import androidx.compose.material3.Icon
import androidx.compose.material3.OutlinedButton
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
import com.ascendance.dac.ui.DacUiState
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
    modifier: Modifier = Modifier
) {
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
                    Text("USB OTG HID Transport", color = AscendanceColors.TextSecondary, fontSize = 12.sp, fontWeight = FontWeight.Medium)
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
                        text = "HID Interface #${info.hidInterfaceId} claimed • UAC Audio Interfaces #0/#1 left with Android Audio • EP OUT=${info.epOutAddress}, IN=${info.epInAddress}",
                        color = AscendanceColors.TextSecondary,
                        fontSize = 11.sp,
                        fontFamily = FontFamily.Monospace
                    )
                    Text(
                        text = "Active UAC Stream Format: ${
                            String.format(Locale.US, "%.1f kHz / 24-bit", state.sampleRateHz / 1000.0f)
                        }",
                        color = AscendanceColors.AccentBlue,
                        fontSize = 12.sp,
                        fontFamily = FontFamily.Monospace
                    )
                } else {
                    Text(
                        text = "Plug the ESP32-S3 DAC via USB-C OTG. Only HID Interface #2 is claimed so Spotify/Tidal/Android audio stays active over UAC.",
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
                            Text("Read DAC State", fontSize = 12.sp)
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
                                    Text("Push Live SRAM", fontSize = 11.sp)
                                }
                                Button(onClick = onPushDraftAndSave, modifier = Modifier.weight(1f)) {
                                    Text("Push & Save NVS", fontSize = 11.sp)
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
                        Text("Offline Demo / Edit Mode", color = AscendanceColors.TextPrimary, fontSize = 13.sp)
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

        // Hardware Event Log
        Surface(
            color = AscendanceColors.CardDark,
            shape = RoundedCornerShape(12.dp),
            border = BorderStroke(1.dp, AscendanceColors.BorderDark)
        ) {
            Column(modifier = Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                Text("USB & DSP Event Log", color = AscendanceColors.TextSecondary, fontSize = 12.sp, fontWeight = FontWeight.Medium)
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
