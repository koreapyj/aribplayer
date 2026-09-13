package kr.dcmys.android.aribplayer.ui.components

import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.tween
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.composed
import androidx.compose.ui.draw.scale
import androidx.compose.ui.focus.onFocusChanged
import androidx.compose.ui.graphics.Shape
import androidx.compose.ui.input.InputMode
import androidx.compose.ui.platform.LocalInputModeManager
import androidx.compose.ui.unit.dp
import kr.dcmys.android.aribplayer.ui.theme.PlayerColors
import kr.dcmys.android.aribplayer.ui.theme.PlayerDims

fun Modifier.tvFocusRing(
    shape: Shape = RoundedCornerShape(4.dp),
    touchModeRing: Boolean = true,
): Modifier = composed {
    var isFocused by remember { mutableStateOf(false) }
    val isTouchMode = LocalInputModeManager.current.inputMode == InputMode.Touch
    val focusScale by animateFloatAsState(
        targetValue = if (isFocused && !isTouchMode) PlayerDims.FocusScale else 1f,
        animationSpec = tween(PlayerDims.FocusAnimMs),
        label = "tvFocusScale",
    )
    val focusDecoration = when {
        !isFocused -> Modifier
        !isTouchMode -> Modifier
            .background(PlayerColors.FocusFill, shape)
            .border(PlayerDims.FocusRing, PlayerColors.FocusRing, shape)
        touchModeRing -> Modifier.border(PlayerDims.FocusRing, PlayerColors.FocusRing, shape)
        else -> Modifier
    }

    onFocusChanged { isFocused = it.isFocused }
        .scale(if (isTouchMode) 1f else focusScale)
        .then(focusDecoration)
}
