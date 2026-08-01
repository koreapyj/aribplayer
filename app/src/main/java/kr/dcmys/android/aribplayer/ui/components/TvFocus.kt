package kr.dcmys.android.aribplayer.ui.components

import androidx.compose.foundation.border
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.composed
import androidx.compose.ui.focus.onFocusChanged
import androidx.compose.ui.graphics.Shape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.ui.unit.dp
import kr.dcmys.android.aribplayer.ui.theme.PlayerColors
import kr.dcmys.android.aribplayer.ui.theme.PlayerDims

fun Modifier.tvFocusRing(shape: Shape = RoundedCornerShape(4.dp)): Modifier = composed {
    var isFocused by remember { mutableStateOf(false) }
    onFocusChanged { isFocused = it.isFocused }
        .then(
            if (isFocused) {
                Modifier.border(PlayerDims.FocusRing, PlayerColors.FocusRing, shape)
            } else {
                Modifier
            },
        )
}
