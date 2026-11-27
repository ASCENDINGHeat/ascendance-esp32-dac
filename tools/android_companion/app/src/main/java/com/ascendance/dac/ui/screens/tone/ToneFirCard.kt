package com.ascendance.dac.ui.screens

import androidx.compose.animation.AnimatedVisibility
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.outlined.ExpandLess
import androidx.compose.material.icons.outlined.ExpandMore
import androidx.compose.material.icons.outlined.Memory
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Surface
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.hapticfeedback.HapticFeedbackType
import androidx.compose.ui.platform.LocalHapticFeedback
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.ascendance.dac.model.DynamicsConfig

@Composable
internal fun ToneFirCard(
    dynamics: DynamicsConfig,
    sampleRateHz: Int,
    onUpdateDynamics: (DynamicsConfig) -> Unit,
    onUploadFirFile: () -> Unit,
    onClearFir: () -> Unit
) {
    val colors = MaterialTheme.colorScheme
    val haptics = LocalHapticFeedback.current
    var firExpanded by rememberSaveable {
        mutableStateOf(dynamics.firEnabled || dynamics.firTapsL > 0 || dynamics.firTapsR > 0)
    }
    val maxCap = if (sampleRateHz > 48000) 128 else 256

    Surface(
        color = colors.surface,
        shape = RoundedCornerShape(12.dp),
        border = BorderStroke(1.dp, colors.outline)
    ) {
        Column(modifier = Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .clickable { firExpanded = !firExpanded },
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically
            ) {
                Row(
                    verticalAlignment = Alignment.CenterVertically,
                    modifier = Modifier.weight(1f)
                ) {
                    Icon(
                        imageVector = Icons.Outlined.Memory,
                        contentDescription = null,
                        tint = colors.primary,
                        modifier = Modifier.size(16.dp)
                    )
                    Spacer(Modifier.width(6.dp))
                    Column {
                        Text(
                            "FIR Impulse Filter",
                            color = colors.onSurface,
                            fontSize = 13.sp,
                            fontWeight = FontWeight.Medium
                        )
                        Text(
                            "L:${dynamics.firTapsL} / R:${dynamics.firTapsR} taps (max $maxCap @ ${sampleRateHz / 1000}k)",
                            color = colors.onSurfaceVariant,
                            fontSize = 11.sp,
                            fontFamily = FontFamily.Monospace
                        )
                    }
                }
                Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(4.dp)) {
                    Switch(
                        checked = dynamics.firEnabled,
                        onCheckedChange = {
                            haptics.performHapticFeedback(HapticFeedbackType.TextHandleMove)
                            onUpdateDynamics(dynamics.copy(firEnabled = it))
                        }
                    )
                    Icon(
                        imageVector = if (firExpanded) Icons.Outlined.ExpandLess else Icons.Outlined.ExpandMore,
                        contentDescription = if (firExpanded) "Collapse FIR" else "Expand FIR",
                        tint = colors.onSurfaceVariant,
                        modifier = Modifier.size(18.dp)
                    )
                }
            }

            AnimatedVisibility(visible = firExpanded) {
                Row(modifier = Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    OutlinedButton(onClick = onUploadFirFile, modifier = Modifier.weight(1f)) {
                        Text("Upload FIR (.txt/.wav)", fontSize = 11.sp)
                    }
                    OutlinedButton(onClick = onClearFir, modifier = Modifier.weight(0.75f)) {
                        Text("Clear FIR", fontSize = 11.sp)
                    }
                }
            }
        }
    }
}
