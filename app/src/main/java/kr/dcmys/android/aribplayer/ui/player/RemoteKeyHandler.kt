package kr.dcmys.android.aribplayer.ui.player

import android.os.SystemClock
import android.view.KeyEvent
import kr.dcmys.android.aribplayer.PlaybackState
import kr.dcmys.android.aribplayer.PlayerScreenDestination
import kr.dcmys.android.aribplayer.PlayerUiState
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch

interface RemotePlaybackController {
    val state: PlayerUiState

    fun togglePlayback()
    fun play()
    fun pause()
    fun seekTo(ms: Long)
    fun toggleSubtitles()
    fun toggleDiagnostics()
    fun closePlayer()
    fun notifyControlsInteraction()
}

internal data class RemoteKeyInput(
    val action: Int,
    val keyCode: Int,
    val repeatCount: Int,
    val flags: Int = 0,
)

class RemoteKeyHandler(
    private val viewModel: RemotePlaybackController,
    private val chromeState: () -> PlayerChromeState?,
    private val onStop: (Long, Long) -> Unit,
    private val scope: CoroutineScope,
    private val clock: () -> Long = SystemClock::uptimeMillis,
) {
    var seekStepMs: Long = 30_000L

    private val lastDownAt = mutableMapOf<Int, Long>()
    private val heldSinceAt = mutableMapOf<Int, Long>()
    private var pendingTargetMs: Long? = null
    private var pendingHoldConfirmed = false
    private var lastSeekIssuedAt: Long = clock()
    private var lastIssuedTargetMs: Long? = null
    private var seekFlushJob: Job? = null
    private var heldFlushJob: Job? = null
    private var clearFeedbackJob: Job? = null
    private var activeChrome: PlayerChromeState? = null

    /** Returns true when the key was consumed. Call from Activity.dispatchKeyEvent BEFORE super. */
    fun onKeyEvent(event: KeyEvent, touchMode: Boolean): Boolean = onKeyInput(
        RemoteKeyInput(
            action = event.action,
            keyCode = event.keyCode,
            repeatCount = event.repeatCount,
            flags = event.flags,
        ),
        touchMode,
    )

    internal fun onKeyInput(input: RemoteKeyInput, touchMode: Boolean): Boolean =
        handle(input.action, input.keyCode, input.repeatCount, touchMode)

    internal fun handle(
        action: Int,
        keyCode: Int,
        repeatCount: Int,
        touchMode: Boolean,
    ): Boolean {
        if (touchMode && action != KeyEvent.ACTION_DOWN) return false
        if (touchMode && !isTouchAllowedKey(keyCode)) return false

        val state = viewModel.state
        val chrome = chromeState() ?: return false
        resetForChromeIfNeeded(chrome)

        val now = clock()
        val seekKey = isSeekKey(keyCode)
        if (action == KeyEvent.ACTION_UP) {
            val heldForMs = heldSinceAt[keyCode]?.let { now - it } ?: 0L
            if (seekKey) {
                heldFlushJob?.cancel()
                heldFlushJob = null
            }
            heldSinceAt.remove(keyCode)
            if (seekKey && heldForMs > HELD_SEEK_THRESHOLD_MS) {
                flush(chrome)
            }
            if (seekKey && pendingTargetMs == null) {
                lastIssuedTargetMs = null
            }
            return false
        }
        if (action != KeyEvent.ACTION_DOWN) return false

        val previousDownAt = lastDownAt[keyCode]
        val held = repeatCount > 0 ||
            (previousDownAt != null && now - previousDownAt < REPEAT_WINDOW_MS)
        lastDownAt[keyCode] = now
        if (seekKey) heldSinceAt.putIfAbsent(keyCode, now)

        val context = PlayerKeyContext(
            touchMode = touchMode,
            controlsVisible = chrome.controlsVisible,
            popupOpen = chrome.popupOpen,
            playbackState = state.playbackState,
            isSeekable = state.isSeekable,
            durationMs = state.durationMs,
            hasSubtitles = state.hasSubtitles,
            focusedControl = chrome.focusedControl,
        )
        val binding = playerKeyBindingFor(keyCode, context, seekStepMs)
        if (binding == null) {
            // Compose normally re-arms the chrome timeout in its key preview handler. Keep that
            // timer alive even when no Compose node currently owns focus, while leaving navigation
            // and activation to Compose/framework focus handling.
            if (!touchMode && chrome.controlsVisible && !chrome.popupOpen &&
                keyCode in visibleNavigationKeys
            ) {
                chrome.recordInteraction(showControls = false)
            }
            return false
        }
        if (state.playbackState == PlaybackState.ERROR && binding.action.isTransportAction()) {
            return false
        }
        if (binding.repeat == RepeatPolicy.FirstDownOnly && repeatCount > 0) {
            if (!touchMode) {
                when (binding.action) {
                    PlayerKeyAction.PlayPause,
                    PlayerKeyAction.Play,
                    PlayerKeyAction.Pause,
                    -> chrome.recordInteraction(showControls = false)
                    else -> Unit
                }
            }
            return true
        }

        return when (val keyAction = binding.action) {
            PlayerKeyAction.RevealControls -> {
                chrome.showControls()
                true
            }
            PlayerKeyAction.ToggleSettings -> {
                if (chrome.popupOpen) chrome.dismissSettings() else chrome.openSettings()
                true
            }
            PlayerKeyAction.ToggleDiagnostics -> {
                viewModel.toggleDiagnostics()
                chrome.recordInteraction(showControls = true)
                true
            }
            PlayerKeyAction.ToggleCaptions -> {
                viewModel.toggleSubtitles()
                chrome.recordInteraction(showControls = true)
                true
            }
            PlayerKeyAction.PlayPause -> {
                viewModel.togglePlayback()
                viewModel.notifyControlsInteraction()
                true
            }
            PlayerKeyAction.Play -> {
                if (!state.isPlaying) viewModel.play()
                viewModel.notifyControlsInteraction()
                true
            }
            PlayerKeyAction.Pause -> {
                if (state.isPlaying) viewModel.pause()
                viewModel.notifyControlsInteraction()
                true
            }
            PlayerKeyAction.Stop -> {
                val positionMs = state.positionMs
                val durationMs = state.durationMs
                release()
                onStop(positionMs, durationMs)
                true
            }
            is PlayerKeyAction.SeekRelative -> {
                val directionalDpad = keyCode == KeyEvent.KEYCODE_DPAD_LEFT ||
                    keyCode == KeyEvent.KEYCODE_DPAD_RIGHT
                if (directionalDpad && (chrome.controlsVisible || chrome.popupOpen)) return false
                if (!state.isSeekable || state.durationMs <= 0L) return false
                val heldLong = held &&
                    heldSinceAt[keyCode]?.let { now - it > HELD_SEEK_THRESHOLD_MS } == true
                val holdConfirmed = repeatCount > 0 || heldLong
                val adjustedDelta = if (heldLong) {
                    doubleDelta(keyAction.ms)
                } else {
                    keyAction.ms
                }
                val base = pendingTargetMs ?: state.positionMs
                val target = safeAdd(base, adjustedDelta)
                    .coerceIn(0L, state.durationMs)
                updatePendingTarget(chrome, state, target, now, holdConfirmed)
                true
            }
            is PlayerKeyAction.SeekPercent -> {
                if (!state.isSeekable || state.durationMs <= 0L) return false
                val target = percentTarget(state.durationMs, keyAction.n)
                updatePendingTarget(chrome, state, target, now)
                true
            }
            PlayerKeyAction.PassThrough -> false
        }
    }

    fun release() {
        seekFlushJob?.cancel()
        seekFlushJob = null
        heldFlushJob?.cancel()
        heldFlushJob = null
        clearFeedbackJob?.cancel()
        clearFeedbackJob = null
        (activeChrome ?: chromeState())?.clearSeekFeedback()
        pendingTargetMs = null
        pendingHoldConfirmed = false
        lastIssuedTargetMs = null
        heldSinceAt.clear()
        lastDownAt.clear()
        activeChrome = null
    }

    private fun resetForChromeIfNeeded(chrome: PlayerChromeState) {
        if (activeChrome === chrome) return
        seekFlushJob?.cancel()
        seekFlushJob = null
        heldFlushJob?.cancel()
        heldFlushJob = null
        clearFeedbackJob?.cancel()
        clearFeedbackJob = null
        pendingTargetMs = null
        pendingHoldConfirmed = false
        lastIssuedTargetMs = null
        lastSeekIssuedAt = clock()
        heldSinceAt.clear()
        lastDownAt.clear()
        activeChrome = chrome
    }

    private fun updatePendingTarget(
        chrome: PlayerChromeState,
        state: PlayerUiState,
        target: Long,
        now: Long,
        holdConfirmed: Boolean = false,
    ) {
        val startsNewBurst = pendingTargetMs == null
        pendingTargetMs = target
        if (startsNewBurst) lastIssuedTargetMs = null
        pendingHoldConfirmed = pendingHoldConfirmed || holdConfirmed
        clearFeedbackJob?.cancel()
        clearFeedbackJob = scope.launch {
            delay(SEEK_FEEDBACK_DURATION_MS)
            if (chromeState() === chrome) chrome.clearSeekFeedback()
        }
        chrome.showSeekFeedback(
            deltaMs = safeSubtract(target, state.positionMs),
            targetMs = target,
        )
        chrome.recordInteraction(showControls = false)
        if (holdConfirmed) issueSeekIfAllowed(target, now)
        if (holdConfirmed) scheduleHeldFlush(chrome)
        seekFlushJob?.cancel()
        seekFlushJob = scope.launch {
            delay(SEEK_FLUSH_DELAY_MS)
            flush(chrome)
        }
    }

    private fun scheduleHeldFlush(chrome: PlayerChromeState) {
        if (heldFlushJob?.isActive == true) return
        val remainingMs = (lastSeekIssuedAt + SEEK_ISSUE_INTERVAL_MS - clock())
            .coerceAtLeast(0L)
        heldFlushJob = scope.launch {
            delay(remainingMs)
            heldFlushJob = null
            flush(chrome, force = true)
        }
    }

    private fun issueSeekIfAllowed(target: Long, now: Long) {
        if (now - lastSeekIssuedAt < SEEK_ISSUE_INTERVAL_MS) return
        viewModel.seekTo(target)
        lastSeekIssuedAt = now
        lastIssuedTargetMs = target
    }

    private fun flush(chrome: PlayerChromeState, force: Boolean = false) {
        val target = pendingTargetMs ?: return
        val state = viewModel.state
        if (
            chromeState() !== chrome ||
            state.screen != PlayerScreenDestination.PLAYER ||
            state.playbackState == PlaybackState.ERROR ||
            !state.isSeekable ||
            state.durationMs <= 0L
        ) {
            release()
            return
        }

        val now = clock()
        if (target != lastIssuedTargetMs) {
            if (pendingHoldConfirmed && !force &&
                now - lastSeekIssuedAt < SEEK_ISSUE_INTERVAL_MS
            ) {
                scheduleHeldFlush(chrome)
                return
            }
            viewModel.seekTo(target)
            lastSeekIssuedAt = now
            lastIssuedTargetMs = target
        }
        pendingTargetMs = null
        pendingHoldConfirmed = false
        lastIssuedTargetMs = null
    }

    private fun isTouchAllowedKey(keyCode: Int): Boolean =
        playerKeyBindings(seekStepMs).any { binding ->
            keyCode in binding.keys && binding.touch == TouchPolicy.AlsoInTouch
        }

    private fun isSeekKey(keyCode: Int): Boolean =
        playerKeyBindings(seekStepMs).any { binding ->
            keyCode in binding.keys && binding.action is PlayerKeyAction.SeekRelative
        }

    private fun doubleDelta(delta: Long): Long = when {
        delta > 0L && delta > Long.MAX_VALUE / 2L -> Long.MAX_VALUE
        delta < 0L && delta < Long.MIN_VALUE / 2L -> Long.MIN_VALUE
        else -> delta * 2L
    }

    private fun safeAdd(left: Long, right: Long): Long = when {
        right > 0L && left > Long.MAX_VALUE - right -> Long.MAX_VALUE
        right < 0L && left < Long.MIN_VALUE - right -> Long.MIN_VALUE
        else -> left + right
    }

    private fun safeSubtract(left: Long, right: Long): Long = when {
        right < 0L && left > Long.MAX_VALUE + right -> Long.MAX_VALUE
        right > 0L && left < Long.MIN_VALUE + right -> Long.MIN_VALUE
        else -> left - right
    }

    private fun percentTarget(durationMs: Long, number: Int): Long {
        val tenth = durationMs / 10L
        val remainder = durationMs % 10L
        return (tenth * number + remainder * number / 10L).coerceIn(0L, durationMs)
    }

    private companion object {
        const val REPEAT_WINDOW_MS = 400L
        const val HELD_SEEK_THRESHOLD_MS = 1_000L
        const val SEEK_ISSUE_INTERVAL_MS = 250L
        const val SEEK_FLUSH_DELAY_MS = 300L
        const val SEEK_FEEDBACK_DURATION_MS = 800L

        val visibleNavigationKeys = setOf(
            KeyEvent.KEYCODE_DPAD_UP,
            KeyEvent.KEYCODE_DPAD_DOWN,
            KeyEvent.KEYCODE_DPAD_LEFT,
            KeyEvent.KEYCODE_DPAD_RIGHT,
            KeyEvent.KEYCODE_DPAD_CENTER,
            KeyEvent.KEYCODE_ENTER,
            KeyEvent.KEYCODE_BACK,
        )
    }
}
