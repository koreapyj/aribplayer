package kr.dcmys.android.aribplayer.ui.player

import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.FastForward
import androidx.compose.material.icons.filled.FastRewind
import androidx.compose.material3.Icon
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import kr.dcmys.android.aribplayer.ui.theme.PlayerColors

@Composable
internal fun SeekFeedbackOverlay(
    feedback: SeekFeedback?,
    durationMs: Long,
    modifier: Modifier = Modifier,
) {
    var lastFeedback by remember { mutableStateOf<SeekFeedback?>(null) }

    LaunchedEffect(feedback) {
        feedback?.let { lastFeedback = it }
    }

    AnimatedVisibility(
        visible = feedback != null,
        enter = fadeIn(),
        exit = fadeOut(),
        modifier = modifier,
    ) {
        lastFeedback?.let { current ->
            Column(
                horizontalAlignment = Alignment.CenterHorizontally,
                modifier = Modifier
                    .background(PlayerColors.Popup, RoundedCornerShape(8.dp))
                    .padding(horizontal = 20.dp, vertical = 14.dp),
            ) {
                Row(
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.Center,
                ) {
                    Icon(
                        imageVector = if (current.deltaMs < 0L) {
                            Icons.Default.FastRewind
                        } else {
                            Icons.Default.FastForward
                        },
                        contentDescription = null,
                        tint = Color.White,
                    )
                    Spacer(Modifier.width(8.dp))
                    Text(
                        text = formatSeekDelta(current.deltaMs),
                        color = Color.White,
                        fontSize = 20.sp,
                    )
                }
                Text(
                    text = "${current.targetMs.formatPlaybackTime()} / ${durationMs.formatPlaybackTime()}",
                    color = Color.White,
                    fontSize = 14.sp,
                )
            }
        }
    }
}

private fun formatSeekDelta(deltaMs: Long): String {
    val sign = if (deltaMs < 0L) "−" else "+"
    val magnitudeMs = deltaMs.absoluteValueOrZero()
    val totalSeconds = magnitudeMs / 1_000L
    return if (totalSeconds >= 60L) {
        val minutes = totalSeconds / 60L
        val seconds = totalSeconds % 60L
        "$sign$minutes:${seconds.toString().padStart(2, '0')}"
    } else {
        "$sign${totalSeconds}s"
    }
}

private fun Long.absoluteValueOrZero(): Long = if (this < 0L) -this else this
