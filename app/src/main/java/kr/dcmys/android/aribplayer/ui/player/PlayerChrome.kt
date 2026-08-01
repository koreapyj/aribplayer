package kr.dcmys.android.aribplayer.ui.player

import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.core.tween
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.slideInVertically
import androidx.compose.animation.slideOutVertically
import androidx.compose.foundation.background
import androidx.compose.foundation.focusable
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
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
import androidx.compose.ui.input.key.Key
import androidx.compose.ui.input.key.KeyEventType
import androidx.compose.ui.input.key.key
import androidx.compose.ui.input.key.onPreviewKeyEvent
import androidx.compose.ui.input.key.type
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.unit.dp
import kr.dcmys.android.aribplayer.AudioTrackUi
import kr.dcmys.android.aribplayer.PlaybackState
import kr.dcmys.android.aribplayer.PlayerUiState
import kr.dcmys.android.aribplayer.data.PlayerPreferences
import kr.dcmys.android.aribplayer.ui.theme.PlayerColors
import kr.dcmys.android.aribplayer.ui.theme.PlayerDims
import kotlinx.coroutines.android.awaitFrame
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.Flow

@Composable
fun PlayerChrome(
    state: PlayerUiState,
    chromeState: PlayerChromeState,
    preferences: PlayerPreferences,
    seekStepMs: Long,
    controlsTimeoutMs: Long,
    interactionEvents: Flow<Unit>,
    onTogglePlayback: () -> Unit,
    onSeekTo: (Long) -> Unit,
    onSetVideoMode: (Int) -> Unit,
    onSelectAudioTrack: (streamIndex: Int, dualMonoMode: Int) -> Unit,
    onToggleSubtitles: () -> Unit,
    onToggleDiagnostics: () -> Unit,
    onSetDefaultVideoMode: (Int) -> Unit,
    onSetSeekStepMs: (Long) -> Unit,
    onSetDiagnosticsEnabled: (Boolean) -> Unit,
    modifier: Modifier = Modifier,
) {
    val focusRequesters = rememberPlayerFocusRequesters()
    val view = LocalView.current
    val density = LocalDensity.current
    val hideTranslationPx = with(density) { PlayerDims.HideTranslation.roundToPx() }
    val playbackEnabled = state.playbackState != PlaybackState.PREPARING &&
        state.playbackState != PlaybackState.ERROR
    val seekEnabled = state.isSeekable && state.durationMs > 0L
    val previewPosition = chromeState.previewPositionMs ?: state.positionMs
    var requestPlayFocus by remember { mutableStateOf(!view.isInTouchMode) }

    LaunchedEffect(interactionEvents) {
        interactionEvents.collect {
            requestPlayFocus = true
            chromeState.showControls()
        }
    }

    LaunchedEffect(chromeState.controlsVisible, playbackEnabled, requestPlayFocus) {
        if (!chromeState.controlsVisible) {
            awaitFrame()
            focusRequesters.root.requestFocus()
        } else if (requestPlayFocus && playbackEnabled) {
            awaitFrame()
            focusRequesters.playPause.requestFocus()
            requestPlayFocus = false
        }
    }

    // Deliberately keyed only to the values that affect timeout eligibility/reset.
    LaunchedEffect(
        state.isPlaying,
        chromeState.controlsVisible,
        chromeState.popupOpen,
        chromeState.scrubbing,
        controlsTimeoutMs,
        chromeState.interactionRevision,
    ) {
        if (
            state.isPlaying &&
            chromeState.controlsVisible &&
            !chromeState.popupOpen &&
            !chromeState.scrubbing &&
            controlsTimeoutMs > 0L
        ) {
            delay(controlsTimeoutMs)
            chromeState.hideControls()
        }
    }

    Box(
        modifier = modifier
            .fillMaxSize()
            .focusRequester(focusRequesters.root)
            .focusProperties { canFocus = !chromeState.controlsVisible }
            .onPreviewKeyEvent { event ->
                if (event.type != KeyEventType.KeyDown) return@onPreviewKeyEvent false
                if (chromeState.controlsVisible) {
                    chromeState.recordInteraction()
                    return@onPreviewKeyEvent false
                }
                if (event.key in dpadKeys) {
                    requestPlayFocus = true
                    chromeState.showControls()
                    true
                } else {
                    false
                }
            }
            .focusable(enabled = !chromeState.controlsVisible),
    ) {
        AnimatedVisibility(
            visible = chromeState.controlsVisible,
            enter = fadeIn(tween(PlayerDims.AnimMs)),
            exit = fadeOut(tween(PlayerDims.AnimMs)),
        ) {
            Box(Modifier.fillMaxSize().background(PlayerColors.Scrim))
        }

        AnimatedVisibility(
            visible = chromeState.controlsVisible,
            modifier = Modifier.align(Alignment.Center),
            enter = fadeIn(tween(PlayerDims.AnimMs)),
            exit = fadeOut(tween(PlayerDims.AnimMs)),
        ) {
            CenterControls(
                isPlaying = state.isPlaying,
                playbackEnabled = playbackEnabled,
                seekEnabled = seekEnabled,
                seekStepMs = seekStepMs,
                focusRequesters = focusRequesters,
                onReplay = {
                    onSeekTo((state.positionMs - seekStepMs).coerceIn(0L, state.durationMs))
                },
                onTogglePlayback = onTogglePlayback,
                onForward = {
                    onSeekTo((state.positionMs + seekStepMs).coerceIn(0L, state.durationMs))
                },
                onInteraction = chromeState::recordInteraction,
            )
        }

        AnimatedVisibility(
            visible = chromeState.controlsVisible,
            modifier = Modifier.align(Alignment.BottomCenter),
            enter = fadeIn(tween(PlayerDims.AnimMs)) + slideInVertically(
                animationSpec = tween(PlayerDims.AnimMs),
                initialOffsetY = { hideTranslationPx },
            ),
            exit = fadeOut(tween(PlayerDims.AnimMs)) + slideOutVertically(
                animationSpec = tween(PlayerDims.AnimMs),
                targetOffsetY = { hideTranslationPx },
            ),
        ) {
            Box(
                modifier = Modifier
                    .fillMaxWidth()
                    .height(PlayerDims.BottomBarHeight + PlayerDims.TimeBarTouchHeight),
            ) {
                Box(
                    modifier = Modifier
                        .fillMaxWidth()
                        .height(PlayerDims.BottomBarHeight)
                        .align(Alignment.BottomCenter)
                        .background(PlayerColors.BottomBar),
                ) {
                    BottomBar(
                        positionMs = previewPosition,
                        durationMs = state.durationMs,
                        diagnosticsVisible = state.diagnosticsVisible,
                        hasSubtitles = state.hasSubtitles,
                        subtitlesEnabled = state.subtitlesEnabled,
                        selectedVideoMode = state.selectedVideoMode,
                        tracks = state.tracks,
                        selectedTrackKey = state.selectedTrackKey,
                        preferences = preferences,
                        chromeState = chromeState,
                        focusRequesters = focusRequesters,
                        onToggleDiagnostics = onToggleDiagnostics,
                        onToggleSubtitles = onToggleSubtitles,
                        onSetVideoMode = onSetVideoMode,
                        onSelectAudioTrack = onSelectAudioTrack,
                        onSetDefaultVideoMode = onSetDefaultVideoMode,
                        onSetSeekStepMs = onSetSeekStepMs,
                        onSetDiagnosticsEnabled = onSetDiagnosticsEnabled,
                        onInteraction = chromeState::recordInteraction,
                    )
                }
                PlayerTimeBar(
                    positionMs = state.positionMs,
                    durationMs = state.durationMs,
                    bufferedPositionMs = null,
                    enabled = seekEnabled,
                    chromeState = chromeState,
                    focusRequester = focusRequesters.timeBar,
                    upFocusRequester = focusRequesters.playPause,
                    downFocusRequester = focusRequesters.info,
                    onSeek = onSeekTo,
                    onInteraction = chromeState::recordInteraction,
                    modifier = Modifier
                        .align(Alignment.BottomCenter)
                        .padding(
                            bottom = PlayerDims.TimeBarBottomOffset - PlayerDims.TimeBarTouchHeight / 2,
                            start = 12.dp,
                            end = 12.dp,
                        ),
                )
            }
        }
    }
}

private val dpadKeys = setOf(
    Key.DirectionUp,
    Key.DirectionDown,
    Key.DirectionLeft,
    Key.DirectionRight,
    Key.DirectionCenter,
)
