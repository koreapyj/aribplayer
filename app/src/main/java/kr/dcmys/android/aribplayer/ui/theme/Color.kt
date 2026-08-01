package kr.dcmys.android.aribplayer.ui.theme

import androidx.compose.ui.graphics.Color

val AppDarkColorScheme = androidx.compose.material3.darkColorScheme(
    primary = Color(0xFF6EA8FE),
    onPrimary = Color(0xFF002E6B),
    background = Color(0xFF0B0D10),
    onBackground = Color(0xFFE6E9ED),
    surface = Color(0xFF12151A),
    onSurface = Color(0xFFE6E9ED),
    surfaceVariant = Color(0xFF1D2229),
    onSurfaceVariant = Color(0xFFA8B0BA),
    outline = Color(0xFF3A4149),
    error = Color(0xFFF2B8B5),
    onError = Color(0xFF601410),
    errorContainer = Color(0xFF8C1D18),
    onErrorContainer = Color(0xFFF9DEDC),
)

object PlayerColors {
    val Scrim = Color(0x98000000)
    val BottomBar = Color(0xB0000000)
    val Popup = Color(0xB3000000)
    val Played = Color.White
    val Buffered = Color.White.copy(alpha = 0.8f)
    val Unplayed = Color.White.copy(alpha = 0.2f)
    val SecondaryText = Color.White.copy(alpha = 0.7f)
    val ErrorPill = Color(0x80808080)
    val FocusRing = Color.White
    val Disabled = Color.White.copy(alpha = 0.33f)
}
