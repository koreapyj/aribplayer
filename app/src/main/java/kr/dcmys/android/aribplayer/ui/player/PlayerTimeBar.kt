package kr.dcmys.android.aribplayer.ui.player

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.focusable
import androidx.compose.foundation.gestures.awaitEachGesture
import androidx.compose.foundation.gestures.awaitFirstDown
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusProperties
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.focus.onFocusChanged
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.input.key.Key
import androidx.compose.ui.input.key.KeyEventType
import androidx.compose.ui.input.key.key
import androidx.compose.ui.input.key.onPreviewKeyEvent
import androidx.compose.ui.input.key.type
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.ProgressBarRangeInfo
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.progressBarRangeInfo
import androidx.compose.ui.semantics.semantics
import kr.dcmys.android.aribplayer.R
import kr.dcmys.android.aribplayer.ui.theme.PlayerColors
import kr.dcmys.android.aribplayer.ui.theme.PlayerDims
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlin.math.roundToLong

@Composable
internal fun PlayerTimeBar(
    positionMs: Long,
    durationMs: Long,
    bufferedPositionMs: Long?,
    enabled: Boolean,
    chromeState: PlayerChromeState,
    focusRequester: FocusRequester,
    upFocusRequester: FocusRequester,
    downFocusRequester: FocusRequester,
    onSeek: (Long) -> Unit,
    onInteraction: () -> Unit,
    modifier: Modifier = Modifier,
) {
    val density = LocalDensity.current
    val scope = rememberCoroutineScope()
    val horizontalInsetPx = with(density) { (PlayerDims.ScrubberDraggedSize / 2).toPx() }
    var focused by remember { mutableStateOf(false) }
    var delayedCommitJob by remember { mutableStateOf<Job?>(null) }
    val safeDuration = durationMs.coerceAtLeast(0L)
    val displayedPosition = (chromeState.previewPositionMs ?: positionMs)
        .coerceIn(0L, safeDuration.coerceAtLeast(0L))
    val description = stringResource(R.string.player_seek_bar_description)

    fun commitPreview() {
        val preview = chromeState.previewPositionMs ?: return
        delayedCommitJob?.cancel()
        delayedCommitJob = null
        chromeState.previewPositionMs = null
        chromeState.scrubbing = false
        onInteraction()
        onSeek(preview.coerceIn(0L, safeDuration))
    }

    fun scheduleDelayedCommit() {
        delayedCommitJob?.cancel()
        delayedCommitJob = scope.launch {
            delay(DPAD_COMMIT_DELAY_MS)
            commitPreview()
        }
    }

    DisposableEffect(Unit) {
        onDispose { delayedCommitJob?.cancel() }
    }

    Canvas(
        modifier = modifier
            .fillMaxWidth()
            .height(PlayerDims.TimeBarTouchHeight)
            .focusRequester(focusRequester)
            .focusProperties {
                up = upFocusRequester
                down = downFocusRequester
                canFocus = enabled
            }
            .onFocusChanged { focusState ->
                val lostFocus = focused && !focusState.isFocused
                focused = focusState.isFocused
                if (lostFocus && !chromeState.scrubbing) commitPreview()
                if (focusState.isFocused) onInteraction()
            }
            .onPreviewKeyEvent { event ->
                if (!enabled || event.type != KeyEventType.KeyDown) return@onPreviewKeyEvent false
                when (event.key) {
                    Key.DirectionLeft, Key.DirectionRight -> {
                        val direction = if (event.key == Key.DirectionLeft) -1L else 1L
                        val step = (safeDuration / 20L).coerceAtLeast(1L)
                        val base = chromeState.previewPositionMs ?: positionMs
                        chromeState.previewPositionMs = (base + direction * step)
                            .coerceIn(0L, safeDuration)
                        onInteraction()
                        scheduleDelayedCommit()
                        true
                    }
                    Key.DirectionCenter, Key.Enter -> {
                        if (chromeState.previewPositionMs != null) {
                            commitPreview()
                            true
                        } else {
                            false
                        }
                    }
                    else -> false
                }
            }
            .pointerInput(enabled, safeDuration, horizontalInsetPx) {
                if (!enabled || safeDuration <= 0L) return@pointerInput
                awaitEachGesture {
                    val down = awaitFirstDown(requireUnconsumed = false)
                    delayedCommitJob?.cancel()
                    delayedCommitJob = null

                    fun positionFor(x: Float): Long {
                        val trackWidth = (size.width - horizontalInsetPx * 2f).coerceAtLeast(1f)
                        val fraction = ((x - horizontalInsetPx) / trackWidth).coerceIn(0f, 1f)
                        return (fraction.toDouble() * safeDuration.toDouble()).roundToLong()
                            .coerceIn(0L, safeDuration)
                    }

                    chromeState.scrubbing = true
                    chromeState.previewPositionMs = positionFor(down.position.x)
                    onInteraction()
                    down.consume()
                    var released = false
                    while (true) {
                        val event = awaitPointerEvent()
                        val change = event.changes.firstOrNull { it.id == down.id } ?: break
                        chromeState.previewPositionMs = positionFor(change.position.x)
                        onInteraction()
                        if (!change.pressed) {
                            released = true
                            change.consume()
                            break
                        }
                        change.consume()
                    }
                    if (released) {
                        commitPreview()
                    } else {
                        chromeState.previewPositionMs = null
                        chromeState.scrubbing = false
                    }
                }
            }
            .semantics {
                contentDescription = description
                progressBarRangeInfo = ProgressBarRangeInfo(
                    current = displayedPosition.toFloat(),
                    range = 0f..safeDuration.coerceAtLeast(1L).toFloat(),
                )
            }
            .focusable(enabled = enabled),
    ) {
        val inset = horizontalInsetPx.coerceAtMost(size.width / 2f)
        val trackStart = inset
        val trackEnd = (size.width - inset).coerceAtLeast(trackStart)
        val trackWidth = (trackEnd - trackStart).coerceAtLeast(0f)
        val centerY = size.height / 2f
        val strokeWidth = PlayerDims.TrackThickness.toPx()

        drawLine(
            color = PlayerColors.Unplayed,
            start = Offset(trackStart, centerY),
            end = Offset(trackEnd, centerY),
            strokeWidth = strokeWidth,
            cap = StrokeCap.Square,
        )

        bufferedPositionMs?.takeIf { safeDuration > 0L }?.let { buffered ->
            val bufferedX = trackStart + trackWidth *
                (buffered.coerceIn(0L, safeDuration).toDouble() / safeDuration.toDouble()).toFloat()
            drawLine(
                color = PlayerColors.Buffered,
                start = Offset(trackStart, centerY),
                end = Offset(bufferedX, centerY),
                strokeWidth = strokeWidth,
                cap = StrokeCap.Square,
            )
        }

        if (safeDuration > 0L) {
            val playedX = trackStart + trackWidth *
                (displayedPosition.toDouble() / safeDuration.toDouble()).toFloat()
            drawLine(
                color = PlayerColors.Played,
                start = Offset(trackStart, centerY),
                end = Offset(playedX, centerY),
                strokeWidth = strokeWidth,
                cap = StrokeCap.Square,
            )
            if (enabled) {
                val scrubberSize = if (chromeState.scrubbing || focused) {
                    PlayerDims.ScrubberDraggedSize
                } else {
                    PlayerDims.ScrubberSize
                }
                drawCircle(
                    color = PlayerColors.Played,
                    radius = scrubberSize.toPx() / 2f,
                    center = Offset(playedX, centerY),
                )
            }
        }
    }
}

private const val DPAD_COMMIT_DELAY_MS = 1_000L
