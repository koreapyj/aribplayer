package kr.dcmys.android.aribplayer.ui.player

import kr.dcmys.android.aribplayer.PlaybackState
import kotlinx.coroutines.CancellationException

internal enum class PlayerControl {
    Replay,
    PlayPause,
    Forward,
    TimeBar,
    Info,
    Captions,
    Settings,
}

internal data class FocusCaps(
    val playbackState: PlaybackState,
    val isSeekable: Boolean,
    val durationMs: Long,
    val hasSubtitles: Boolean,
)

private val playbackEnabledStates = setOf(
    PlaybackState.READY,
    PlaybackState.PLAYING,
    PlaybackState.PAUSED,
    PlaybackState.ENDED,
)

private fun FocusCaps.playbackEnabled(): Boolean = playbackState in playbackEnabledStates

private fun FocusCaps.seekEnabled(): Boolean =
    playbackEnabled() && isSeekable && durationMs > 0L

internal fun available(caps: FocusCaps): Set<PlayerControl> = buildSet {
    if (caps.playbackEnabled()) add(PlayerControl.PlayPause)
    if (caps.seekEnabled()) {
        add(PlayerControl.Replay)
        add(PlayerControl.Forward)
        add(PlayerControl.TimeBar)
    }
    add(PlayerControl.Info)
    if (caps.hasSubtitles) add(PlayerControl.Captions)
    add(PlayerControl.Settings)
}

internal fun defaultTarget(caps: FocusCaps): PlayerControl =
    if (caps.playbackEnabled()) PlayerControl.PlayPause else PlayerControl.Settings

internal fun fallbackFor(lost: PlayerControl, caps: FocusCaps): PlayerControl {
    val controls = available(caps)
    val preferred = when (lost) {
        PlayerControl.Replay,
        PlayerControl.Forward,
        PlayerControl.TimeBar,
        -> PlayerControl.PlayPause
        PlayerControl.PlayPause -> PlayerControl.Settings
        PlayerControl.Info -> if (PlayerControl.Captions in controls) {
            PlayerControl.Captions
        } else {
            PlayerControl.Settings
        }
        PlayerControl.Captions -> PlayerControl.Settings
        PlayerControl.Settings -> PlayerControl.Info
    }
    return preferred.takeIf { it in controls } ?: defaultTarget(caps).let {
        it.takeIf { target -> target in controls } ?: PlayerControl.Settings
    }
}

/**
 * Requests focus only after a composition frame and waits for a real focus acknowledgement.
 *
 * Compose's [androidx.compose.ui.focus.FocusRequester.requestFocus] returns Unit, so the
 * request itself is not evidence that focus moved. The caller can invalidate the operation
 * through [isCurrent] when a newer intent or a cancelled composition effect supersedes it.
 */
internal suspend fun awaitFocusAcknowledgement(
    requestFocus: () -> Unit,
    isFocused: () -> Boolean,
    isCurrent: () -> Boolean = { true },
    awaitNextFrame: suspend () -> Unit,
    maxFrames: Int = 30,
): Boolean {
    if (maxFrames <= 0) return false
    repeat(maxFrames) {
        if (!isCurrent()) return false
        awaitNextFrame()
        if (!isCurrent()) return false
        try {
            requestFocus()
        } catch (cancellation: CancellationException) {
            throw cancellation
        } catch (_: IllegalStateException) {
            // The requester may not be attached yet; retry on the next frame.
        }
        if (!isCurrent()) return false
        if (isFocused()) return true
    }
    return false
}
