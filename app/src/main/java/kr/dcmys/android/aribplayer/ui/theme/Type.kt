package kr.dcmys.android.aribplayer.ui.theme

import androidx.compose.material3.Typography
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.sp

val AppTypography = Typography()

object PlayerTextStyles {
    val TimeText = TextStyle(fontSize = 14.sp, fontWeight = FontWeight.Bold)
    val PopupTitle = TextStyle(fontSize = 14.sp)
    val PopupSubtitle = TextStyle(fontSize = 12.sp)
    val ErrorText = TextStyle(fontSize = 14.sp)
    val Diagnostics = TextStyle(fontSize = 11.sp, fontFamily = FontFamily.Monospace)
}
