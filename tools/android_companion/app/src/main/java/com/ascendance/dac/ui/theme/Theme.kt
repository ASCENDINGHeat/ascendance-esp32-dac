package com.ascendance.dac.ui.theme

import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color

object AscendanceColors {
    val BgDark = Color(0xFF0B0E14)
    val CardDark = Color(0xFF161B22)
    val SurfaceElevated = Color(0xFF1E242E)
    val BorderDark = Color(0xFF30363D)
    val AccentBlue = Color(0xFF58A6FF)
    val AccentGreen = Color(0xFF3FB950)
    val AccentAmber = Color(0xFFD29922)
    val AccentRed = Color(0xFFF85149)
    val AccentPurple = Color(0xFFBC8CFF)
    val TextPrimary = Color(0xFFE6EDF3)
    val TextSecondary = Color(0xFF8B949E)
}

private val DarkPalette = darkColorScheme(
    primary = AscendanceColors.AccentBlue,
    onPrimary = Color(0xFF071324),
    secondary = AscendanceColors.AccentGreen,
    onSecondary = Color(0xFF041C0A),
    tertiary = AscendanceColors.AccentAmber,
    background = AscendanceColors.BgDark,
    onBackground = AscendanceColors.TextPrimary,
    surface = AscendanceColors.CardDark,
    onSurface = AscendanceColors.TextPrimary,
    surfaceVariant = AscendanceColors.SurfaceElevated,
    onSurfaceVariant = AscendanceColors.TextSecondary,
    outline = AscendanceColors.BorderDark,
    error = AscendanceColors.AccentRed,
    onError = Color.White
)

@Composable
fun AscendanceTheme(content: @Composable () -> Unit) {
    MaterialTheme(
        colorScheme = DarkPalette,
        content = content
    )
}
