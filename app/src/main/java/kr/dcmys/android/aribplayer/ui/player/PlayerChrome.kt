package kr.dcmys.android.aribplayer.ui.player

import android.view.KeyEvent
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
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusProperties
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.input.key.Key
import androidx.compose.ui.input.key.KeyEventType
import androidx.compose.ui.input.key.key
import androidx.compose.ui.input.key.onPreviewKeyEvent
import androidx.compose.ui.input.key.type
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.platform.LocalWindowInfo
import androidx.compose.ui.unit.dp
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
    onRemoteKeyEvent: ((KeyEvent) -> Boolean)? = null,
) {
    val focusCoordinator = remember(chromeState) { PlayerFocusCoordinator(chromeState) }
    val focusRequesters = focusCoordinator.requesters
    val view = LocalView.current
    val windowFocused = LocalWindowInfo.current.isWindowFocused
    val touchMode = view.isInTouchMode
    val density = LocalDensity.current
    val hideTranslationPx = with(density) { PlayerDims.HideTranslation.roundToPx() }
    val focusCaps = FocusCaps(
        playbackState = state.playbackState,
        isSeekable = state.isSeekable,
        durationMs = state.durationMs,
        hasSubtitles = state.hasSubtitles,
    )
    val availableControls = available(focusCaps)
    val playbackEnabled = PlayerControl.PlayPause in availableControls
    val seekEnabled = PlayerControl.TimeBar in availableControls
    val previewPosition = chromeState.previewPositionMs ?: state.positionMs

    LaunchedEffect(
        chromeState.controlsVisible,
        chromeState.popupOpen,
        chromeState.focusedControl,
        chromeState.interactionRevision,
        focusCoordinator.pendingRevision,
        focusCaps,
        windowFocused,
        touchMode,
    ) {
        focusCoordinator.observe(
            controlsVisible = chromeState.controlsVisible,
            popupOpen = chromeState.popupOpen,
            touchMode = touchMode,
            caps = focusCaps,
        )
        if (!chromeState.controlsVisible) {
            awaitFrame()
            focusCoordinator.requestRootFocus()
            return@LaunchedEffect
        }
        if (chromeState.popupOpen || !windowFocused) return@LaunchedEffect
        focusCoordinator.runPending(
            caps = focusCaps,
            windowFocused = windowFocused,
            touchMode = touchMode,
            awaitNextFrame = { awaitFrame() },
        )
    }

    LaunchedEffect(interactionEvents) {
        interactionEvents.collect {
            chromeState.showControls()
        }
    }

    // Deliberately keyed only to the values that affect timeout eligibility/reset.
    LaunchedEffect(
        state.isPlaying,
        chromeState.controlsVisible,
        chromeState.popupOpen,
        chromeState.scrubbing,
        chromeState.previewPositionMs,
        controlsTimeoutMs,
        chromeState.interactionRevision,
    ) {
        if (
            state.isPlaying &&
            chromeState.controlsVisible &&
            !chromeState.popupOpen &&
            !chromeState.scrubbing &&
            chromeState.previewPositionMs == null &&
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
                if (event.key in hiddenRevealKeys ||
                    (touchMode && event.key in touchModeRevealKeys)
                ) {
                    chromeState.showControls()
                    true
                } else {
                    // In non-touch mode LEFT/RIGHT are not consumed: the activity handler seeks.
                    false
                }
            }
            // Keep the target installed so canFocus is contained here when controls are visible.
            .focusable(),
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
                onFocusChanged = focusCoordinator::onFocusChanged,
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
                        upFocusRequester = when {
                            seekEnabled -> focusRequesters.timeBar
                            playbackEnabled -> focusRequesters.playPause
                            else -> FocusRequester.Cancel
                        },
                        onFocusChanged = focusCoordinator::onFocusChanged,
                        onToggleDiagnostics = onToggleDiagnostics,
                        onToggleSubtitles = onToggleSubtitles,
                        onSetVideoMode = onSetVideoMode,
                        onSelectAudioTrack = onSelectAudioTrack,
                        onSetDefaultVideoMode = onSetDefaultVideoMode,
                        onSetSeekStepMs = onSetSeekStepMs,
                        onSetDiagnosticsEnabled = onSetDiagnosticsEnabled,
                        onInteraction = chromeState::recordInteraction,
                        onRemoteKeyEvent = onRemoteKeyEvent,
                    )
                }
                PlayerTimeBar(
                    positionMs = state.positionMs,
                    durationMs = state.durationMs,
                    bufferedPositionMs = null,
                    enabled = seekEnabled,
                    seekStepMs = seekStepMs,
                    chromeState = chromeState,
                    focusRequester = focusRequesters.timeBar,
                    onFocusChanged = focusCoordinator::onFocusChanged,
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

        SeekFeedbackOverlay(
            feedback = chromeState.seekFeedback,
            durationMs = state.durationMs,
            modifier = Modifier.align(Alignment.Center),
        )
    }
}

private val hiddenRevealKeys = setOf(
    Key.DirectionUp,
    Key.DirectionDown,
    Key.DirectionCenter,
)

private val touchModeRevealKeys = setOf(
    Key.DirectionLeft,
    Key.DirectionRight,
)
