package com.ascendance.dac.ui.screens

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.ascendance.dac.model.DspProConfig
import com.ascendance.dac.model.DynamicsConfig
import com.ascendance.dac.model.HarmonicsConfig
import com.ascendance.dac.model.MixerConfig

@Composable
fun ToneTab(
    mixer: MixerConfig,
    dynamics: DynamicsConfig,
    harmonics: HarmonicsConfig,
    dspPro: DspProConfig,
    sampleRateHz: Int,
    onUpdateMixer: (MixerConfig) -> Unit,
    onUpdateDynamics: (DynamicsConfig) -> Unit,
    onUpdateHarmonics: (HarmonicsConfig) -> Unit,
    onUpdateDspPro: (DspProConfig) -> Unit,
    onUploadFirFile: () -> Unit,
    onClearFir: () -> Unit,
    modifier: Modifier = Modifier
) {
    Column(
        modifier = modifier
            .fillMaxWidth()
            .verticalScroll(rememberScrollState()),
        verticalArrangement = Arrangement.spacedBy(10.dp)
    ) {
        ToneHarmonicsCard(
            harmonics = harmonics,
            dspPro = dspPro,
            onUpdateHarmonics = onUpdateHarmonics,
            onUpdateDspPro = onUpdateDspPro
        )
        ToneDynamicsCard(
            dynamics = dynamics,
            dspPro = dspPro,
            onUpdateDynamics = onUpdateDynamics,
            onUpdateDspPro = onUpdateDspPro
        )
        ToneMatrixCrossfeedCard(
            mixer = mixer,
            onUpdateMixer = onUpdateMixer
        )
        ToneFirCard(
            dynamics = dynamics,
            sampleRateHz = sampleRateHz,
            onUpdateDynamics = onUpdateDynamics,
            onUploadFirFile = onUploadFirFile,
            onClearFir = onClearFir
        )
    }
}

@Composable
internal fun ToneSectionHeader(
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
