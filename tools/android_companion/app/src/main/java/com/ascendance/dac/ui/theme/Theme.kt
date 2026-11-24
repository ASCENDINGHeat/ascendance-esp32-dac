package com.ascendance.dac.ui.theme

import android.os.Build
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.dynamicDarkColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext

enum class AppThemeMode(val key: String, val label: String) {
    DARK_STUDIO("dark_studio", "Dark Studio"),
    AMOLED_BLACK("amoled_black", "AMOLED"),
    DYNAMIC_YOU("dynamic_you", "Dynamic");

    companion object {
        fun fromKey(key: String?): AppThemeMode =
            entries.firstOrNull { it.key == key } ?: DARK_STUDIO
    }
}

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

private val DarkStudioPalette = darkColorScheme(
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

private val AmoledPalette = darkColorScheme(
    primary = AscendanceColors.AccentBlue,
    onPrimary = Color(0xFF040B16),
    secondary = AscendanceColors.AccentGreen,
    onSecondary = Color(0xFF041C0A),
    tertiary = AscendanceColors.AccentAmber,
    background = Color.Black,
    onBackground = AscendanceColors.TextPrimary,
    surface = Color(0xFF0B0E14),
    onSurface = AscendanceColors.TextPrimary,
    surfaceVariant = Color(0xFF141923),
    onSurfaceVariant = AscendanceColors.TextSecondary,
    outline = Color(0xFF262D38),
    error = AscendanceColors.AccentRed,
    onError = Color.White
)

@Composable
fun AscendanceTheme(
    themeMode: AppThemeMode = AppThemeMode.DARK_STUDIO,
    content: @Composable () -> Unit
) {
    val context = LocalContext.current
    val colorScheme = when (themeMode) {
        AppThemeMode.DARK_STUDIO -> DarkStudioPalette
        AppThemeMode.AMOLED_BLACK -> AmoledPalette
        AppThemeMode.DYNAMIC_YOU -> {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                dynamicDarkColorScheme(context)
            } else {
                DarkStudioPalette
            }
        }
    }

    MaterialTheme(
        colorScheme = colorScheme,
        content = content
    )
}
