package kr.dcmys.android.aribplayer.ui.player

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Forward10
import androidx.compose.material.icons.filled.Forward30
import androidx.compose.material.icons.filled.PauseCircle
import androidx.compose.material.icons.filled.PlayCircle
import androidx.compose.material.icons.filled.Replay10
import androidx.compose.material.icons.filled.Replay30
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.focus.focusProperties
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.res.pluralStringResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import kr.dcmys.android.aribplayer.R
import kr.dcmys.android.aribplayer.ui.components.tvFocusRing
import kr.dcmys.android.aribplayer.ui.theme.PlayerColors
import kr.dcmys.android.aribplayer.ui.theme.PlayerDims

@Composable
internal fun CenterControls(
    isPlaying: Boolean,
    playbackEnabled: Boolean,
    seekEnabled: Boolean,
    seekStepMs: Long,
    focusRequesters: PlayerFocusRequesters,
    onReplay: () -> Unit,
    onTogglePlayback: () -> Unit,
    onForward: () -> Unit,
    onInteraction: () -> Unit,
    modifier: Modifier = Modifier,
) {
    val seekSeconds = (seekStepMs / 1_000L).coerceAtLeast(1L).toInt()
    val replayDescription = pluralStringResource(
        R.plurals.player_rewind_by_amount_description,
        seekSeconds,
        seekSeconds,
    )
    val forwardDescription = pluralStringResource(
        R.plurals.player_fastforward_by_amount_description,
        seekSeconds,
        seekSeconds,
    )

    Row(
        modifier = modifier,
        horizontalArrangement = Arrangement.spacedBy(24.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        CenterIconButton(
            icon = if (seekStepMs == 10_000L) Icons.Filled.Replay10 else Icons.Filled.Replay30,
            contentDescription = replayDescription,
            enabled = seekEnabled,
            modifier = Modifier
                .focusRequester(focusRequesters.replay)
                .focusProperties {
                    right = focusRequesters.playPause
                    down = focusRequesters.timeBar
                },
            onClick = {
                onInteraction()
                onReplay()
            },
        )
        IconButton(
            onClick = {
                onInteraction()
                onTogglePlayback()
            },
            enabled = playbackEnabled,
            modifier = Modifier
                .size(PlayerDims.PlayPauseButton)
                .focusRequester(focusRequesters.playPause)
                .focusProperties {
                    left = focusRequesters.replay
                    right = focusRequesters.forward
                    down = focusRequesters.timeBar
                    canFocus = playbackEnabled
                }
                .tvFocusRing(CircleShape),
        ) {
            Icon(
                imageVector = if (isPlaying) Icons.Filled.PauseCircle else Icons.Filled.PlayCircle,
                contentDescription = stringResource(
                    if (isPlaying) R.string.player_pause_description else R.string.player_play_description,
                ),
                tint = if (playbackEnabled) Color.White else PlayerColors.Disabled,
                modifier = Modifier.size(PlayerDims.PlayPauseButton),
            )
        }
        CenterIconButton(
            icon = if (seekStepMs == 10_000L) Icons.Filled.Forward10 else Icons.Filled.Forward30,
            contentDescription = forwardDescription,
            enabled = seekEnabled,
            modifier = Modifier
                .focusRequester(focusRequesters.forward)
                .focusProperties {
                    left = focusRequesters.playPause
                    down = focusRequesters.timeBar
                },
            onClick = {
                onInteraction()
                onForward()
            },
        )
    }
}

@Composable
private fun CenterIconButton(
    icon: ImageVector,
    contentDescription: String,
    enabled: Boolean,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
) {
    IconButton(
        onClick = onClick,
        enabled = enabled,
        modifier = modifier
            .size(PlayerDims.ActionButton)
            .focusProperties { canFocus = enabled }
            .tvFocusRing(CircleShape),
    ) {
        Icon(
            imageVector = icon,
            contentDescription = contentDescription,
            tint = if (enabled) Color.White else PlayerColors.Disabled,
            modifier = Modifier.size(32.dp),
        )
    }
}
