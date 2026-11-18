package com.ascendance.dac.ui.screens

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.layout.Arrangement
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
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.outlined.BookmarkAdd
import androidx.compose.material.icons.outlined.Delete
import androidx.compose.material.icons.outlined.Download
import androidx.compose.material.icons.outlined.Edit
import androidx.compose.material.icons.outlined.FolderOpen
import androidx.compose.material.icons.outlined.PlayArrow
import androidx.compose.material.icons.outlined.Save
import androidx.compose.material.icons.outlined.UploadFile
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.FilterChip
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
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
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.ascendance.dac.model.HardwarePreset
import com.ascendance.dac.model.LocalPreset
import com.ascendance.dac.ui.DacUiState
import com.ascendance.dac.ui.theme.AscendanceColors
import java.util.Locale

@Composable
fun PresetsTab(
    state: DacUiState,
    onSelectHardwarePreset: (Int) -> Unit,
    onSaveToNvs: () -> Unit,
    onResetFlat: () -> Unit,
    onImportFile: () -> Unit,
    onExportApoTxt: () -> Unit,
    onExportJson: () -> Unit,
    onSaveLocalPreset: (String) -> Unit,
    onApplyLocalPreset: (LocalPreset, Boolean) -> Unit,
    onRenameLocalPreset: (String, String) -> Unit,
    onDeleteLocalPreset: (String) -> Unit,
    modifier: Modifier = Modifier
) {
    var showNewPresetDialog by remember { mutableStateOf(false) }
    var newPresetName by remember { mutableStateOf("") }
    var renamingPreset by remember { mutableStateOf<LocalPreset?>(null) }
    var renameText by remember { mutableStateOf("") }

    Column(
        modifier = modifier
            .fillMaxWidth()
            .verticalScroll(rememberScrollState()),
        verticalArrangement = Arrangement.spacedBy(10.dp)
    ) {
        // Hardware On-Device Preset Slots + NVS Save
        Surface(
            color = AscendanceColors.CardDark,
            shape = RoundedCornerShape(12.dp),
            border = BorderStroke(1.dp, AscendanceColors.BorderDark)
        ) {
            Column(modifier = Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text("DAC Hardware Presets", color = AscendanceColors.TextSecondary, fontSize = 12.sp, fontWeight = FontWeight.Medium)
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.spacedBy(6.dp)
                ) {
                    HardwarePreset.entries.take(2).forEach { hp ->
                        FilterChip(
                            selected = state.activePresetSlot == hp.slot,
                            onClick = { onSelectHardwarePreset(hp.slot) },
                            label = { Text(hp.label, fontSize = 11.sp) },
                            modifier = Modifier.weight(1f)
                        )
                    }
                }
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.spacedBy(6.dp)
                ) {
                    HardwarePreset.entries.drop(2).forEach { hp ->
                        FilterChip(
                            selected = state.activePresetSlot == hp.slot,
                            onClick = { onSelectHardwarePreset(hp.slot) },
                            label = { Text(hp.label, fontSize = 11.sp) },
                            modifier = Modifier.weight(1f)
                        )
                    }
                }
                Row(modifier = Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    Button(
                        onClick = onSaveToNvs,
                        colors = ButtonDefaults.buttonColors(containerColor = AscendanceColors.AccentGreen),
                        modifier = Modifier.weight(1f)
                    ) {
                        Icon(Icons.Outlined.Save, contentDescription = null, modifier = Modifier.size(16.dp))
                        Spacer(Modifier.width(6.dp))
                        Text("Save to DAC NVS", fontSize = 12.sp)
                    }
                    OutlinedButton(onClick = onResetFlat, modifier = Modifier.weight(0.75f)) {
                        Text("Reset Flat", fontSize = 12.sp)
                    }
                }
            }
        }

        // Equalizer APO / AutoEQ Import & Export (SAF)
        Surface(
            color = AscendanceColors.CardDark,
            shape = RoundedCornerShape(12.dp),
            border = BorderStroke(1.dp, AscendanceColors.BorderDark)
        ) {
            Column(modifier = Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text("AutoEQ / Equalizer APO (.txt & .json)", color = AscendanceColors.TextSecondary, fontSize = 12.sp, fontWeight = FontWeight.Medium)
                Row(modifier = Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    OutlinedButton(onClick = onImportFile, modifier = Modifier.weight(1f)) {
                        Icon(Icons.Outlined.UploadFile, contentDescription = null, modifier = Modifier.size(16.dp))
                        Spacer(Modifier.width(4.dp))
                        Text("Import", fontSize = 12.sp)
                    }
                    OutlinedButton(onClick = onExportApoTxt, modifier = Modifier.weight(1f)) {
                        Icon(Icons.Outlined.Download, contentDescription = null, modifier = Modifier.size(16.dp))
                        Spacer(Modifier.width(4.dp))
                        Text("Export APO", fontSize = 12.sp)
                    }
                    OutlinedButton(onClick = onExportJson, modifier = Modifier.weight(1f)) {
                        Icon(Icons.Outlined.FolderOpen, contentDescription = null, modifier = Modifier.size(16.dp))
                        Spacer(Modifier.width(4.dp))
                        Text("JSON", fontSize = 12.sp)
                    }
                }
            }
        }

        // Unlimited Local Phone Preset Library
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
                    Text(
                        "Phone Preset Library (${state.localPresets.size})",
                        color = AscendanceColors.TextSecondary,
                        fontSize = 12.sp,
                        fontWeight = FontWeight.Medium
                    )
                    Button(
                        onClick = {
                            newPresetName = "Headphone ${state.localPresets.size + 1}"
                            showNewPresetDialog = true
                        },
                        contentPadding = PaddingValues(horizontal = 10.dp, vertical = 4.dp),
                        modifier = Modifier.height(32.dp)
                    ) {
                        Icon(Icons.Outlined.BookmarkAdd, contentDescription = null, modifier = Modifier.size(15.dp))
                        Spacer(Modifier.width(4.dp))
                        Text("Save Current", fontSize = 11.sp)
                    }
                }

                if (state.localPresets.isEmpty()) {
                    Text(
                        "No local profiles saved yet. Import an AutoEQ file or dial in a custom curve and tap 'Save Current'.",
                        color = AscendanceColors.TextSecondary,
                        fontSize = 12.sp
                    )
                } else {
                    state.localPresets.forEach { preset ->
                        Surface(
                            color = AscendanceColors.SurfaceElevated,
                            shape = RoundedCornerShape(10.dp),
                            border = BorderStroke(1.dp, AscendanceColors.BorderDark)
                        ) {
                            Column(modifier = Modifier.padding(10.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                                Row(
                                    modifier = Modifier.fillMaxWidth(),
                                    horizontalArrangement = Arrangement.SpaceBetween,
                                    verticalAlignment = Alignment.CenterVertically
                                ) {
                                    Column(modifier = Modifier.weight(1f)) {
                                        Text(preset.name, color = AscendanceColors.TextPrimary, fontWeight = FontWeight.SemiBold, fontSize = 13.sp)
                                        Text(
                                            text = "${if (preset.linkedStereo) "Stereo" else "Dual-Mono"} • ${preset.leftBands.size} bands • Preamp ${
                                                String.format(Locale.US, "%+.1fdB", preset.preampDb)
                                            }",
                                            color = AscendanceColors.TextSecondary,
                                            fontSize = 11.sp,
                                            fontFamily = FontFamily.Monospace
                                        )
                                    }
                                    Row {
                                        IconButton(
                                            onClick = {
                                                renamingPreset = preset
                                                renameText = preset.name
                                            },
                                            modifier = Modifier.size(30.dp)
                                        ) {
                                            Icon(Icons.Outlined.Edit, contentDescription = "Rename", modifier = Modifier.size(16.dp))
                                        }
                                        IconButton(
                                            onClick = { onDeleteLocalPreset(preset.id) },
                                            modifier = Modifier.size(30.dp)
                                        ) {
                                            Icon(Icons.Outlined.Delete, contentDescription = "Delete", tint = AscendanceColors.AccentRed, modifier = Modifier.size(16.dp))
                                        }
                                    }
                                }
                                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                                    OutlinedButton(
                                        onClick = { onApplyLocalPreset(preset, false) },
                                        contentPadding = PaddingValues(horizontal = 10.dp, vertical = 2.dp),
                                        modifier = Modifier.height(30.dp).weight(1f)
                                    ) {
                                        Icon(Icons.Outlined.PlayArrow, contentDescription = null, modifier = Modifier.size(14.dp))
                                        Spacer(Modifier.width(4.dp))
                                        Text("Audition Live", fontSize = 11.sp)
                                    }
                                    OutlinedButton(
                                        onClick = { onApplyLocalPreset(preset, true) },
                                        enabled = state.usbInfo.connected,
                                        contentPadding = PaddingValues(horizontal = 10.dp, vertical = 2.dp),
                                        modifier = Modifier.height(30.dp).weight(1f)
                                    ) {
                                        Icon(Icons.Outlined.Save, contentDescription = null, modifier = Modifier.size(14.dp))
                                        Spacer(Modifier.width(4.dp))
                                        Text("Load & Save NVS", fontSize = 11.sp)
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    if (showNewPresetDialog) {
        AlertDialog(
            onDismissRequest = { showNewPresetDialog = false },
            title = { Text("Save Profile to Phone Library", fontSize = 15.sp) },
            text = {
                OutlinedTextField(
                    value = newPresetName,
                    onValueChange = { newPresetName = it },
                    label = { Text("Profile Name (e.g. HD600 Harman)") },
                    singleLine = true,
                    modifier = Modifier.fillMaxWidth()
                )
            },
            confirmButton = {
                TextButton(onClick = {
                    onSaveLocalPreset(newPresetName)
                    showNewPresetDialog = false
                }) { Text("Save") }
            },
            dismissButton = {
                TextButton(onClick = { showNewPresetDialog = false }) { Text("Cancel") }
            }
        )
    }

    val targetRename = renamingPreset
    if (targetRename != null) {
        AlertDialog(
            onDismissRequest = { renamingPreset = null },
            title = { Text("Rename Profile", fontSize = 15.sp) },
            text = {
                OutlinedTextField(
                    value = renameText,
                    onValueChange = { renameText = it },
                    singleLine = true,
                    modifier = Modifier.fillMaxWidth()
                )
            },
            confirmButton = {
                TextButton(onClick = {
                    onRenameLocalPreset(targetRename.id, renameText)
                    renamingPreset = null
                }) { Text("Rename") }
            },
            dismissButton = {
                TextButton(onClick = { renamingPreset = null }) { Text("Cancel") }
            }
        )
    }
}
