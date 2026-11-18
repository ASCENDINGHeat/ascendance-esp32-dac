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
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.outlined.CompareArrows
import androidx.compose.material.icons.outlined.Equalizer
import androidx.compose.material.icons.outlined.FolderOpen
import androidx.compose.material.icons.outlined.Save
import androidx.compose.material.icons.outlined.Tune
import androidx.compose.material.icons.outlined.Usb
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.Icon
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
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.ascendance.dac.ui.DacViewModel
import com.ascendance.dac.ui.components.PeqCanvasCard
import com.ascendance.dac.ui.screens.DeviceTab
import com.ascendance.dac.ui.screens.EqTab
import com.ascendance.dac.ui.screens.MatrixTab
import com.ascendance.dac.ui.screens.PresetsTab
import com.ascendance.dac.ui.theme.AscendanceColors
import com.ascendance.dac.ui.theme.AscendanceTheme
import java.util.Locale

private enum class CompanionTab(val label: String, val icon: ImageVector) {
    EQ("EQ", Icons.Outlined.Equalizer),
    MATRIX("Matrix", Icons.Outlined.Tune),
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
            AscendanceTheme {
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
    val context = androidx.compose.ui.platform.LocalContext.current
    var selectedTabIdx by rememberSaveable { mutableIntStateOf(0) }
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

    Scaffold(
        containerColor = AscendanceColors.BgDark,
        snackbarHost = { SnackbarHost(snackbarHostState) },
        topBar = {
            TopAppBar(
                colors = TopAppBarDefaults.topAppBarColors(
                    containerColor = AscendanceColors.BgDark,
                    titleContentColor = AscendanceColors.TextPrimary
                ),
                title = {
                    Column {
                        Text("ASCENDANCE DAC", fontSize = 16.sp, fontWeight = FontWeight.Bold)
                        val sub = if (state.usbInfo.connected) {
                            String.format(Locale.US, "USB HID • %.1fk 24b", state.sampleRateHz / 1000.0f)
                        } else {
                            "Offline Demo Mode"
                        }
                        Text(
                            text = sub,
                            fontSize = 11.sp,
                            fontFamily = FontFamily.Monospace,
                            color = if (state.usbInfo.connected) AscendanceColors.AccentGreen else AscendanceColors.TextSecondary
                        )
                    }
                },
                actions = {
                    // Instant A/B Bypass chip
                    FilledTonalButton(
                        onClick = { viewModel.toggleAbBypass() },
                        contentPadding = PaddingValues(horizontal = 10.dp, vertical = 2.dp),
                        colors = ButtonDefaults.filledTonalButtonColors(
                            containerColor = if (state.abBypass) AscendanceColors.AccentAmber else AscendanceColors.SurfaceElevated,
                            contentColor = if (state.abBypass) AscendanceColors.BgDark else AscendanceColors.TextPrimary
                        ),
                        modifier = Modifier.height(32.dp)
                    ) {
                        Icon(Icons.Outlined.CompareArrows, contentDescription = null, modifier = Modifier.size(14.dp))
                        Spacer(Modifier.width(4.dp))
                        Text(if (state.abBypass) "A/B: BYP" else "A/B", fontSize = 11.sp, fontWeight = FontWeight.Bold)
                    }
                    Spacer(Modifier.width(6.dp))
                    // Quick Connect or Save NVS
                    if (state.usbInfo.connected) {
                        FilledTonalButton(
                            onClick = { viewModel.saveToHardwareNvs() },
                            contentPadding = PaddingValues(horizontal = 10.dp, vertical = 2.dp),
                            colors = ButtonDefaults.filledTonalButtonColors(
                                containerColor = AscendanceColors.AccentGreen.copy(alpha = 0.22f),
                                contentColor = AscendanceColors.AccentGreen
                            ),
                            modifier = Modifier.height(32.dp)
                        ) {
                            Icon(Icons.Outlined.Save, contentDescription = null, modifier = Modifier.size(14.dp))
                            Spacer(Modifier.width(4.dp))
                            Text("NVS", fontSize = 11.sp, fontWeight = FontWeight.Bold)
                        }
                    } else {
                        FilledTonalButton(
                            onClick = { viewModel.connectUsb() },
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
            NavigationBar(containerColor = AscendanceColors.CardDark) {
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
            // Sticky Frequency Response / Phase / Group Delay Canvas
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

            // Active Tab Content
            Box(modifier = Modifier.weight(1f)) {
                when (CompanionTab.entries[selectedTabIdx]) {
                    CompanionTab.EQ -> EqTab(
                        state = state,
                        onSelectChannel = viewModel::selectEditorChannel,
                        onSelectBand = viewModel::selectBand,
                        onUpdateBand = { idx, b -> viewModel.updateBand(idx, b) },
                        onAddBand = { viewModel.addBand() },
                        onRemoveBand = viewModel::removeBand,
                        onUpdatePreamp = viewModel::updatePreamp
                    )
                    CompanionTab.MATRIX -> MatrixTab(
                        mixer = state.mixer,
                        dynamics = state.dynamics,
                        sampleRateHz = state.sampleRateHz,
                        onUpdateMixer = viewModel::updateMixer,
                        onUpdateDynamics = viewModel::updateDynamics,
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
                        onSetPowerConfig = viewModel::updatePowerConfig
                    )
                }
            }
        }
    }
}
