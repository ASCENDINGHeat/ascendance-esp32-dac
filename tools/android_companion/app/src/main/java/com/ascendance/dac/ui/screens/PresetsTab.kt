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
import androidx.compose.material.icons.outlined.Bookmarks
import androidx.compose.material.icons.outlined.Delete
import androidx.compose.material.icons.outlined.Download
import androidx.compose.material.icons.outlined.Edit
import androidx.compose.material.icons.outlined.FolderOpen
import androidx.compose.material.icons.outlined.PlayArrow
import androidx.compose.material.icons.outlined.Save
import androidx.compose.material.icons.outlined.Storage
import androidx.compose.material.icons.outlined.UploadFile
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.FilterChip
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
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
import androidx.compose.ui.hapticfeedback.HapticFeedbackType
import androidx.compose.ui.platform.LocalHapticFeedback
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.ascendance.dac.model.DspDefaults
import com.ascendance.dac.model.LocalPreset
import com.ascendance.dac.ui.DacUiState
import com.ascendance.dac.ui.theme.AscendanceColors
import java.util.Locale

@Composable
fun PresetsTab(
    state: DacUiState,
    onSelectHardwarePreset: (Int) -> Unit,
    onRenameHardwareSlot: (String) -> Unit,
    onFactoryResetSlot: () -> Unit,
    onApplyStarterCurve: (String) -> Unit,
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
    val colors = MaterialTheme.colorScheme
    val haptics = LocalHapticFeedback.current
    var showNewPresetDialog by remember { mutableStateOf(false) }
    var newPresetName by remember { mutableStateOf("") }
    var showRenameSlotDialog by remember { mutableStateOf(false) }
    var slotRenameText by remember { mutableStateOf("") }
    var renamingPreset by remember { mutableStateOf<LocalPreset?>(null) }
    var renameText by remember { mutableStateOf("") }

    Column(
        modifier = modifier
            .fillMaxWidth()
            .verticalScroll(rememberScrollState()),
        verticalArrangement = Arrangement.spacedBy(10.dp)
    ) {
        // 8-Slot Editable DAC Presets + Rename / Reset / Save
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
                    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                        Icon(Icons.Outlined.Storage, contentDescription = null, tint = colors.primary, modifier = Modifier.size(16.dp))
                        Text("DAC Presets (1–8)", color = colors.onSurfaceVariant, fontSize = 12.sp, fontWeight = FontWeight.Medium)
                    }
                    Text(
                        text = "Active: Slot ${state.activePresetSlot + 1}",
                        color = colors.primary,
                        fontSize = 11.sp,
                        fontFamily = FontFamily.Monospace
                    )
                }
                Row(modifier = Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                    (0 until 4).forEach { slot ->
                        val lbl = DspDefaults.formatSlotLabel(slot, state.slotNames.getOrNull(slot))
                        FilterChip(
                            selected = state.activePresetSlot == slot,
                            onClick = {
                                haptics.performHapticFeedback(HapticFeedbackType.TextHandleMove)
                                onSelectHardwarePreset(slot)
                            },
                            label = { Text(lbl, fontSize = 10.sp, maxLines = 1, overflow = TextOverflow.Ellipsis) },
                            modifier = Modifier.weight(1f)
                        )
                    }
                }
                Row(modifier = Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                    (4 until 8).forEach { slot ->
                        val lbl = DspDefaults.formatSlotLabel(slot, state.slotNames.getOrNull(slot))
                        FilterChip(
                            selected = state.activePresetSlot == slot,
                            onClick = {
                                haptics.performHapticFeedback(HapticFeedbackType.TextHandleMove)
                                onSelectHardwarePreset(slot)
                            },
                            label = { Text(lbl, fontSize = 10.sp, maxLines = 1, overflow = TextOverflow.Ellipsis) },
                            modifier = Modifier.weight(1f)
                        )
                    }
                }
                Row(modifier = Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    OutlinedButton(
                        onClick = {
                            slotRenameText = state.slotNames.getOrElse(state.activePresetSlot) { "Slot ${state.activePresetSlot + 1}" }
                            showRenameSlotDialog = true
                        },
                        modifier = Modifier.weight(1f)
                    ) {
                        Icon(Icons.Outlined.Edit, contentDescription = null, modifier = Modifier.size(14.dp))
                        Spacer(Modifier.width(4.dp))
                        Text("Rename Slot", fontSize = 11.sp)
                    }
                    OutlinedButton(onClick = onFactoryResetSlot, modifier = Modifier.weight(1f)) {
                        Text("Reset Slot", fontSize = 11.sp)
                    }
                }
                Text("Template:", color = colors.onSurfaceVariant, fontSize = 11.sp)
                Row(modifier = Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                    OutlinedButton(onClick = { onApplyStarterCurve("flat") }, modifier = Modifier.weight(1f)) {
                        Text("Flat", fontSize = 10.sp)
                    }
                    OutlinedButton(onClick = { onApplyStarterCurve("bass") }, modifier = Modifier.weight(1f)) {
                        Text("Bass", fontSize = 10.sp)
                    }
                    OutlinedButton(onClick = { onApplyStarterCurve("harman") }, modifier = Modifier.weight(1f)) {
                        Text("Harman", fontSize = 10.sp)
                    }
                }
                Row(modifier = Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    Button(
                        onClick = {
                            haptics.performHapticFeedback(HapticFeedbackType.LongPress)
                            onSaveToNvs()
                        },
                        colors = ButtonDefaults.buttonColors(containerColor = AscendanceColors.AccentGreen),
                        modifier = Modifier.weight(1f)
                    ) {
                        Icon(Icons.Outlined.Save, contentDescription = null, modifier = Modifier.size(16.dp))
                        Spacer(Modifier.width(6.dp))
                        Text("Save to DAC", fontSize = 12.sp)
                    }
                    OutlinedButton(onClick = onResetFlat, modifier = Modifier.weight(0.75f)) {
                        Text("Flatten Band", fontSize = 11.sp)
                    }
                }
            }
        }

        // Equalizer APO / AutoEQ Import & Export (SAF)
        Surface(
            color = colors.surface,
            shape = RoundedCornerShape(12.dp),
            border = BorderStroke(1.dp, colors.outline)
        ) {
            Column(modifier = Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                    Icon(Icons.Outlined.FolderOpen, contentDescription = null, tint = colors.primary, modifier = Modifier.size(16.dp))
                    Text("AutoEQ / Equalizer APO (.txt & .json)", color = colors.onSurfaceVariant, fontSize = 12.sp, fontWeight = FontWeight.Medium)
                }
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
                    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                        Icon(Icons.Outlined.Bookmarks, contentDescription = null, tint = colors.primary, modifier = Modifier.size(16.dp))
                        Text(
                            "Phone Preset Library (${state.localPresets.size})",
                            color = colors.onSurfaceVariant,
                            fontSize = 12.sp,
                            fontWeight = FontWeight.Medium
                        )
                    }
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
                        color = colors.onSurfaceVariant,
                        fontSize = 12.sp
                    )
                } else {
                    state.localPresets.forEach { preset ->
                        Surface(
                            color = colors.surfaceVariant,
                            shape = RoundedCornerShape(10.dp),
                            border = BorderStroke(1.dp, colors.outline)
                        ) {
                            Column(modifier = Modifier.padding(10.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                                Row(
                                    modifier = Modifier.fillMaxWidth(),
                                    horizontalArrangement = Arrangement.SpaceBetween,
                                    verticalAlignment = Alignment.CenterVertically
                                ) {
                                    Column(modifier = Modifier.weight(1f)) {
                                        Text(preset.name, color = colors.onSurface, fontWeight = FontWeight.SemiBold, fontSize = 13.sp)
                                        Text(
                                            text = "${if (preset.linkedStereo) "Stereo" else "Dual-Mono"} • ${preset.leftBands.size} bands • Preamp ${
                                                String.format(Locale.US, "%+.1fdB", preset.preampDb)
                                            }",
                                            color = colors.onSurfaceVariant,
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
                                        Text("Preview Live", fontSize = 11.sp)
                                    }
                                    OutlinedButton(
                                        onClick = { onApplyLocalPreset(preset, true) },
                                        enabled = state.usbInfo.connected,
                                        contentPadding = PaddingValues(horizontal = 10.dp, vertical = 2.dp),
                                        modifier = Modifier.height(30.dp).weight(1f)
                                    ) {
                                        Icon(Icons.Outlined.Save, contentDescription = null, modifier = Modifier.size(14.dp))
                                        Spacer(Modifier.width(4.dp))
                                        Text("Load & Save", fontSize = 11.sp)
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

    if (showRenameSlotDialog) {
        AlertDialog(
            onDismissRequest = { showRenameSlotDialog = false },
            title = { Text("Rename Slot ${state.activePresetSlot + 1}", fontSize = 15.sp) },
            text = {
                OutlinedTextField(
                    value = slotRenameText,
                    onValueChange = { slotRenameText = it.take(16) },
                    label = { Text("Slot Name (max 16 chars)") },
                    singleLine = true,
                    modifier = Modifier.fillMaxWidth()
                )
            },
            confirmButton = {
                TextButton(onClick = {
                    onRenameHardwareSlot(slotRenameText)
                    showRenameSlotDialog = false
                }) { Text("Save") }
            },
            dismissButton = {
                TextButton(onClick = { showRenameSlotDialog = false }) { Text("Cancel") }
            }
        )
    }
}
