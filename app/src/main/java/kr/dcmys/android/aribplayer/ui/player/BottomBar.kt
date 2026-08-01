package kr.dcmys.android.aribplayer.ui.player

import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Info
import androidx.compose.material.icons.filled.Settings
import androidx.compose.material.icons.filled.Subtitles
import androidx.compose.material.icons.filled.SubtitlesOff
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.focus.focusProperties
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import kr.dcmys.android.aribplayer.AudioTrackUi
import kr.dcmys.android.aribplayer.R
import kr.dcmys.android.aribplayer.data.PlayerPreferences
import kr.dcmys.android.aribplayer.ui.components.tvFocusRing
import kr.dcmys.android.aribplayer.ui.theme.PlayerColors
import kr.dcmys.android.aribplayer.ui.theme.PlayerDims
import kr.dcmys.android.aribplayer.ui.theme.PlayerTextStyles
import kotlinx.coroutines.android.awaitFrame

@Composable
internal fun BottomBar(
    positionMs: Long,
    durationMs: Long,
    diagnosticsVisible: Boolean,
    hasSubtitles: Boolean,
    subtitlesEnabled: Boolean,
    selectedVideoMode: Int,
    tracks: List<AudioTrackUi>,
    selectedTrackKey: String?,
    preferences: PlayerPreferences,
    chromeState: PlayerChromeState,
    focusRequesters: PlayerFocusRequesters,
    onToggleDiagnostics: () -> Unit,
    onToggleSubtitles: () -> Unit,
    onSetVideoMode: (Int) -> Unit,
    onSelectAudioTrack: (streamIndex: Int, dualMonoMode: Int) -> Unit,
    onSetDefaultVideoMode: (Int) -> Unit,
    onSetSeekStepMs: (Long) -> Unit,
    onSetDiagnosticsEnabled: (Boolean) -> Unit,
    onInteraction: () -> Unit,
    modifier: Modifier = Modifier,
) {
    val captionsOrSettings = if (hasSubtitles) focusRequesters.captions else focusRequesters.settings
    var popupWasOpen by remember { mutableStateOf(false) }

    LaunchedEffect(chromeState.popupOpen) {
        if (chromeState.popupOpen) {
            popupWasOpen = true
        } else if (popupWasOpen) {
            popupWasOpen = false
            awaitFrame()
            focusRequesters.settings.requestFocus()
            onInteraction()
        }
    }

    Row(
        modifier = modifier
            .fillMaxWidth()
            .height(PlayerDims.BottomBarHeight)
            .padding(horizontal = 4.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(
            text = positionMs.formatPlaybackTime(),
            color = Color.White,
            style = PlayerTextStyles.TimeText,
            modifier = Modifier.padding(horizontal = 4.dp),
        )
        Text(
            text = "·",
            color = Color.White,
            style = PlayerTextStyles.TimeText,
            modifier = Modifier.padding(horizontal = 4.dp),
        )
        Text(
            text = durationMs.formatPlaybackTime(),
            color = PlayerColors.SecondaryText,
            style = PlayerTextStyles.TimeText,
            modifier = Modifier.padding(horizontal = 4.dp),
        )
        Spacer(Modifier.weight(1f))
        BottomActionButton(
            icon = Icons.Filled.Info,
            contentDescription = stringResource(R.string.player_info_description),
            selected = diagnosticsVisible,
            modifier = Modifier
                .focusRequester(focusRequesters.info)
                .focusProperties {
                    up = focusRequesters.timeBar
                    right = captionsOrSettings
                },
            onClick = {
                onInteraction()
                onToggleDiagnostics()
            },
        )
        if (hasSubtitles) {
            BottomActionButton(
                icon = if (subtitlesEnabled) Icons.Filled.Subtitles else Icons.Filled.SubtitlesOff,
                contentDescription = stringResource(
                    if (subtitlesEnabled) {
                        R.string.player_cc_enabled_description
                    } else {
                        R.string.player_cc_disabled_description
                    },
                ),
                selected = subtitlesEnabled,
                modifier = Modifier
                    .focusRequester(focusRequesters.captions)
                    .focusProperties {
                        left = focusRequesters.info
                        right = focusRequesters.settings
                        up = focusRequesters.timeBar
                    },
                onClick = {
                    onInteraction()
                    onToggleSubtitles()
                },
            )
        }
        Box {
            BottomActionButton(
                icon = Icons.Filled.Settings,
                contentDescription = stringResource(R.string.player_settings_description),
                selected = chromeState.popupOpen,
                modifier = Modifier
                    .focusRequester(focusRequesters.settings)
                    .focusProperties {
                        left = if (hasSubtitles) focusRequesters.captions else focusRequesters.info
                        up = focusRequesters.timeBar
                    },
                onClick = {
                    onInteraction()
                    if (chromeState.popupOpen) chromeState.dismissSettings() else chromeState.openSettings()
                },
            )
            chromeState.settingsPage?.let { page ->
                PlayerSettingsPopup(
                    page = page,
                    mainFocusTarget = chromeState.settingsMainFocusTarget,
                    appSettingsFocusTarget = chromeState.appSettingsFocusTarget,
                    selectedVideoMode = selectedVideoMode,
                    tracks = tracks,
                    selectedTrackKey = selectedTrackKey,
                    preferences = preferences,
                    onOpenPage = chromeState::openSettingsPage,
                    onOpenAppSettingsPage = chromeState::openAppSettingsPage,
                    onReturnToMain = chromeState::returnToSettingsMain,
                    onReturnToAppSettings = chromeState::returnToAppSettings,
                    onSelectVideoMode = onSetVideoMode,
                    onSelectAudioTrack = onSelectAudioTrack,
                    onSetDefaultVideoMode = onSetDefaultVideoMode,
                    onSetSeekStepMs = onSetSeekStepMs,
                    onSetDiagnosticsEnabled = onSetDiagnosticsEnabled,
                    onDismiss = chromeState::dismissSettings,
                    onInteraction = onInteraction,
                )
            }
        }
    }
}

@Composable
private fun BottomActionButton(
    icon: ImageVector,
    contentDescription: String,
    selected: Boolean,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
) {
    IconButton(
        onClick = onClick,
        modifier = modifier
            .size(PlayerDims.ActionButton)
            .tvFocusRing(),
    ) {
        Box(
            modifier = Modifier.fillMaxSize().padding(PlayerDims.ActionPadding),
            contentAlignment = Alignment.Center,
        ) {
            Icon(
                imageVector = icon,
                contentDescription = contentDescription,
                tint = if (selected) Color.White else PlayerColors.SecondaryText,
                modifier = Modifier.size(PlayerDims.Glyph),
            )
        }
    }
}

internal fun Long.formatPlaybackTime(): String {
    val totalSeconds = coerceAtLeast(0L) / 1_000L
    val hours = totalSeconds / 3_600L
    val minutes = (totalSeconds % 3_600L) / 60L
    val seconds = totalSeconds % 60L
    return if (hours > 0L) "%d:%02d:%02d".format(hours, minutes, seconds)
    else "%02d:%02d".format(minutes, seconds)
}
