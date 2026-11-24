package com.ascendance.dac

import android.content.Intent
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.contract.ActivityResultContracts
import androidx.activity.viewModels
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.outlined.ArrowDropDown
import androidx.compose.material.icons.outlined.Check
import androidx.compose.material.icons.outlined.CompareArrows
import androidx.compose.material.icons.outlined.Equalizer
import androidx.compose.material.icons.outlined.FolderOpen
import androidx.compose.material.icons.outlined.Save
import androidx.compose.material.icons.outlined.Tune
import androidx.compose.material.icons.outlined.Usb
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.NavigationBar
import androidx.compose.material3.NavigationBarItem
import androidx.compose.material3.Scaffold
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.SnackbarHostState
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.hapticfeedback.HapticFeedbackType
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalHapticFeedback
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.ascendance.dac.model.DspDefaults
import com.ascendance.dac.ui.DacViewModel
import com.ascendance.dac.ui.components.PeqCanvasCard
import com.ascendance.dac.ui.screens.DeviceTab
import com.ascendance.dac.ui.screens.EqTab
import com.ascendance.dac.ui.screens.PresetsTab
import com.ascendance.dac.ui.screens.ToneTab
import com.ascendance.dac.ui.theme.AscendanceColors
import com.ascendance.dac.ui.theme.AscendanceTheme
import java.util.Locale

private enum class CompanionTab(val label: String, val icon: ImageVector) {
    EQ("EQ", Icons.Outlined.Equalizer),
    TONE("Tone", Icons.Outlined.Tune),
    PRESETS("Presets", Icons.Outlined.FolderOpen),
    DEVICE("Device", Icons.Outlined.Usb)
}

class MainActivity : ComponentActivity() {

    private val viewModel: DacViewModel by viewModels()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        viewModel.handleNewIntent(intent)

        setContent {
            val state by viewModel.uiState.collectAsState()
            AscendanceTheme(themeMode = state.themeMode) {
                AscendanceCompanionApp(viewModel = viewModel)
            }
        }
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        viewModel.handleNewIntent(intent)
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun AscendanceCompanionApp(viewModel: DacViewModel) {
    val state by viewModel.uiState.collectAsState()
    val context = LocalContext.current
    val haptics = LocalHapticFeedback.current
    val colors = MaterialTheme.colorScheme
    var selectedTabIdx by rememberSaveable { mutableIntStateOf(0) }
    var showSlotMenu by remember { mutableStateOf(false) }
    val snackbarHostState = remember { SnackbarHostState() }

    val importLauncher = rememberLauncherForActivityResult(
        contract = ActivityResultContracts.OpenDocument()
    ) { uri ->
        if (uri != null) {
            val text = context.contentResolver.openInputStream(uri)?.bufferedReader()?.use { it.readText() }
            if (text != null) {
                val name = uri.lastPathSegment?.substringAfterLast('/') ?: "profile"
                viewModel.importProfileText(text, name)
            }
        }
    }

    val firLauncher = rememberLauncherForActivityResult(
        contract = ActivityResultContracts.OpenDocument()
    ) { uri ->
        if (uri != null) {
            val bytes = context.contentResolver.openInputStream(uri)?.use { it.readBytes() }
            if (bytes != null) {
                val name = uri.lastPathSegment?.substringAfterLast('/') ?: "impulse.txt"
                viewModel.uploadFirFile(bytes, name)
            }
        }
    }

    val exportApoLauncher = rememberLauncherForActivityResult(
        contract = ActivityResultContracts.CreateDocument("text/plain")
    ) { uri ->
        if (uri != null) {
            context.contentResolver.openOutputStream(uri)?.bufferedWriter()?.use {
                it.write(viewModel.exportCurrentApoText())
            }
        }
    }

    val exportJsonLauncher = rememberLauncherForActivityResult(
        contract = ActivityResultContracts.CreateDocument("application/json")
    ) { uri ->
        if (uri != null) {
            context.contentResolver.openOutputStream(uri)?.bufferedWriter()?.use {
                it.write(viewModel.exportCurrentJsonSnapshot())
            }
        }
    }

    LaunchedEffect(state.statusBanner) {
        val msg = state.statusBanner
        if (msg != null) {
            snackbarHostState.showSnackbar(msg)
            viewModel.dismissBanner()
        }
    }

    val currentTab = CompanionTab.entries[selectedTabIdx]
    val activeSlotLabel = DspDefaults.formatSlotLabel(
        state.activePresetSlot,
        state.slotNames.getOrNull(state.activePresetSlot)
    )

    Scaffold(
        containerColor = colors.background,
        snackbarHost = { SnackbarHost(snackbarHostState) },
        topBar = {
            TopAppBar(
                colors = TopAppBarDefaults.topAppBarColors(
                    containerColor = colors.background,
                    titleContentColor = colors.onBackground
                ),
                title = {
                    Column(verticalArrangement = Arrangement.spacedBy(2.dp)) {
                        Text(
                            text = "ASCENDANCE DAC",
                            fontSize = 15.sp,
                            fontWeight = FontWeight.Bold,
                            maxLines = 1,
                            overflow = TextOverflow.Ellipsis
                        )
                        Row(
                            verticalAlignment = Alignment.CenterVertically,
                            horizontalArrangement = Arrangement.spacedBy(6.dp)
                        ) {
                            val sub = if (state.usbInfo.connected) {
                                String.format(Locale.US, "%.1fk 24b", state.sampleRateHz / 1000.0f)
                            } else {
                                "Offline"
                            }
                            Text(
                                text = sub,
                                fontSize = 11.sp,
                                fontFamily = FontFamily.Monospace,
                                color = if (state.usbInfo.connected) AscendanceColors.AccentGreen else colors.onSurfaceVariant,
                                maxLines = 1
                            )
                            Box {
                                Surface(
                                    color = colors.surfaceVariant,
                                    shape = RoundedCornerShape(6.dp),
                                    border = BorderStroke(1.dp, colors.outline),
                                    modifier = Modifier
                                        .clip(RoundedCornerShape(6.dp))
                                        .clickable { showSlotMenu = true }
                                ) {
                                    Row(
                                        modifier = Modifier.padding(horizontal = 6.dp, vertical = 1.dp),
                                        verticalAlignment = Alignment.CenterVertically
                                    ) {
                                        Text(
                                            text = activeSlotLabel,
                                            fontSize = 10.sp,
                                            fontFamily = FontFamily.Monospace,
                                            color = colors.primary,
                                            maxLines = 1,
                                            overflow = TextOverflow.Ellipsis,
                                            modifier = Modifier.widthIn(max = 96.dp)
                                        )
                                        Icon(
                                            imageVector = Icons.Outlined.ArrowDropDown,
                                            contentDescription = "Switch Preset Slot",
                                            tint = colors.primary,
                                            modifier = Modifier.size(14.dp)
                                        )
                                    }
                                }
                                DropdownMenu(
                                    expanded = showSlotMenu,
                                    onDismissRequest = { showSlotMenu = false }
                                ) {
                                    (0 until DspDefaults.USER_SLOT_COUNT).forEach { slot ->
                                        val lbl = DspDefaults.formatSlotLabel(slot, state.slotNames.getOrNull(slot))
                                        val isCurrent = slot == state.activePresetSlot
                                        DropdownMenuItem(
                                            text = {
                                                Text(
                                                    text = "Slot ${slot + 1} • $lbl",
                                                    fontWeight = if (isCurrent) FontWeight.Bold else FontWeight.Normal,
                                                    color = if (isCurrent) colors.primary else colors.onSurface
                                                )
                                            },
                                            trailingIcon = if (isCurrent) {
                                                {
                                                    Icon(
                                                        imageVector = Icons.Outlined.Check,
                                                        contentDescription = null,
                                                        tint = colors.primary,
                                                        modifier = Modifier.size(16.dp)
                                                    )
                                                }
                                            } else null,
                                            onClick = {
                                                showSlotMenu = false
                                                haptics.performHapticFeedback(HapticFeedbackType.TextHandleMove)
                                                viewModel.selectHardwarePreset(slot)
                                            }
                                        )
                                    }
                                }
                            }
                        }
                    }
                },
                actions = {
                    // Instant A/B Bypass chip
                    FilledTonalButton(
                        onClick = {
                            haptics.performHapticFeedback(HapticFeedbackType.LongPress)
                            viewModel.toggleAbBypass()
                        },
                        contentPadding = PaddingValues(horizontal = 10.dp, vertical = 2.dp),
                        colors = ButtonDefaults.filledTonalButtonColors(
                            containerColor = if (state.abBypass) AscendanceColors.AccentAmber else colors.surfaceVariant,
                            contentColor = if (state.abBypass) colors.background else colors.onSurface
                        ),
                        modifier = Modifier.height(32.dp)
                    ) {
                        Icon(Icons.Outlined.CompareArrows, contentDescription = null, modifier = Modifier.size(14.dp))
                        Spacer(Modifier.width(4.dp))
                        Text(if (state.abBypass) "BYP" else "A/B", fontSize = 11.sp, fontWeight = FontWeight.Bold)
                    }
                    Spacer(Modifier.width(6.dp))
                    // Quick Connect or Save to DAC (with amber unsaved-changes badge)
                    if (state.usbInfo.connected) {
                        val saveAccent = if (state.hasUnsavedChanges) AscendanceColors.AccentAmber else AscendanceColors.AccentGreen
                        FilledTonalButton(
                            onClick = {
                                haptics.performHapticFeedback(HapticFeedbackType.LongPress)
                                viewModel.saveToHardwareNvs()
                            },
                            contentPadding = PaddingValues(horizontal = 10.dp, vertical = 2.dp),
                            colors = ButtonDefaults.filledTonalButtonColors(
                                containerColor = saveAccent.copy(alpha = 0.22f),
                                contentColor = saveAccent
                            ),
                            modifier = Modifier.height(32.dp)
                        ) {
                            Icon(Icons.Outlined.Save, contentDescription = null, modifier = Modifier.size(14.dp))
                            Spacer(Modifier.width(4.dp))
                            Text("Save", fontSize = 11.sp, fontWeight = FontWeight.Bold)
                            if (state.hasUnsavedChanges) {
                                Spacer(Modifier.width(4.dp))
                                Box(
                                    modifier = Modifier
                                        .size(6.dp)
                                        .background(AscendanceColors.AccentAmber, CircleShape)
                                )
                            }
                        }
                    } else {
                        FilledTonalButton(
                            onClick = {
                                haptics.performHapticFeedback(HapticFeedbackType.TextHandleMove)
                                viewModel.connectUsb()
                            },
                            contentPadding = PaddingValues(horizontal = 10.dp, vertical = 2.dp),
                            colors = ButtonDefaults.filledTonalButtonColors(
                                containerColor = AscendanceColors.AccentBlue.copy(alpha = 0.22f),
                                contentColor = AscendanceColors.AccentBlue
                            ),
                            modifier = Modifier.height(32.dp)
                        ) {
                            Icon(Icons.Outlined.Usb, contentDescription = null, modifier = Modifier.size(14.dp))
                            Spacer(Modifier.width(4.dp))
                            Text("Connect", fontSize = 11.sp, fontWeight = FontWeight.Bold)
                        }
                    }
                    Spacer(Modifier.width(8.dp))
                }
            )
        },
        bottomBar = {
            NavigationBar(containerColor = colors.surface) {
                CompanionTab.entries.forEachIndexed { idx, tab ->
                    NavigationBarItem(
                        selected = selectedTabIdx == idx,
                        onClick = { selectedTabIdx = idx },
                        icon = { Icon(tab.icon, contentDescription = tab.label) },
                        label = { Text(tab.label, fontSize = 11.sp) }
                    )
                }
            }
        }
    ) { innerPadding ->
        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(innerPadding)
                .padding(horizontal = 12.dp, vertical = 6.dp),
            verticalArrangement = Arrangement.spacedBy(10.dp)
        ) {
            // Frequency Response / Phase / Group Delay Canvas (EQ tab only)
            if (currentTab == CompanionTab.EQ) {
                PeqCanvasCard(
                    bands = state.activeBands,
                    selectedBandIdx = state.selectedBandIdx,
                    preampDb = state.preampDb,
                    abBypass = state.abBypass,
                    sampleRateHz = state.sampleRateHz,
                    showPhase = state.showPhase,
                    showGroupDelay = state.showGroupDelay,
                    onSelectBand = viewModel::selectBand,
                    onUpdateBand = { idx, band -> viewModel.updateBand(idx, band) },
                    onAddBandAt = { freq, gain -> viewModel.addBand(freq, gain) },
                    onTogglePhase = viewModel::togglePhase,
                    onToggleGroupDelay = viewModel::toggleGroupDelay
                )
            }

            // Active Tab Content
            Box(modifier = Modifier.weight(1f)) {
                when (currentTab) {
                    CompanionTab.EQ -> EqTab(
                        state = state,
                        onSelectChannel = viewModel::selectEditorChannel,
                        onSelectBand = viewModel::selectBand,
                        onUpdateBand = { idx, b -> viewModel.updateBand(idx, b) },
                        onAddBand = { viewModel.addBand() },
                        onRemoveBand = viewModel::removeBand,
                        onUpdatePreamp = viewModel::updatePreamp
                    )
                    CompanionTab.TONE -> ToneTab(
                        mixer = state.mixer,
                        dynamics = state.dynamics,
                        harmonics = state.harmonics,
                        sampleRateHz = state.sampleRateHz,
                        onUpdateMixer = viewModel::updateMixer,
                        onUpdateDynamics = viewModel::updateDynamics,
                        onUpdateHarmonics = viewModel::updateHarmonics,
                        onUploadFirFile = { firLauncher.launch(arrayOf("text/plain", "text/csv", "audio/wav", "audio/x-wav", "*/*")) },
                        onClearFir = viewModel::clearFirImpulse
                    )
                    CompanionTab.PRESETS -> PresetsTab(
                        state = state,
                        onSelectHardwarePreset = viewModel::selectHardwarePreset,
                        onRenameHardwareSlot = viewModel::renameActiveSlot,
                        onFactoryResetSlot = viewModel::factoryResetActiveSlot,
                        onApplyStarterCurve = viewModel::applyStarterCurve,
                        onSaveToNvs = viewModel::saveToHardwareNvs,
                        onResetFlat = viewModel::resetActiveChannelFlat,
                        onImportFile = { importLauncher.launch(arrayOf("text/plain", "application/json", "*/*")) },
                        onExportApoTxt = { exportApoLauncher.launch("ascendance_peq_apo.txt") },
                        onExportJson = { exportJsonLauncher.launch("ascendance_peq_snapshot.json") },
                        onSaveLocalPreset = viewModel::saveCurrentAsLocalPreset,
                        onApplyLocalPreset = viewModel::applyLocalPreset,
                        onRenameLocalPreset = viewModel::renameLocalPreset,
                        onDeleteLocalPreset = viewModel::deleteLocalPreset
                    )
                    CompanionTab.DEVICE -> DeviceTab(
                        state = state,
                        onConnectUsb = viewModel::connectUsb,
                        onDisconnectUsb = viewModel::disconnectUsb,
                        onSyncFromHardware = viewModel::syncFromHardware,
                        onPushDraftLive = { viewModel.pushCurrentStateToHardware(commitToNvs = false) },
                        onPushDraftAndSave = { viewModel.pushCurrentStateToHardware(commitToNvs = true) },
                        onToggleOfflineDemo = viewModel::setOfflineDemoMode,
                        onSetOledView = { v -> viewModel.updateDynamics(state.dynamics.copy(oledView = v)) },
                        onSetPowerConfig = viewModel::updatePowerConfig,
                        onSelectThemeMode = viewModel::setThemeMode
                    )
                }
            }
        }
    }
}
