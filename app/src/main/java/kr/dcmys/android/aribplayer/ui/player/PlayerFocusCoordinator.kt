package kr.dcmys.android.aribplayer.ui.player

import androidx.compose.runtime.Stable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.focus.FocusRequester

@Stable
internal class PlayerFocusRequesters {
    val root = FocusRequester()
    val replay = FocusRequester()
    val playPause = FocusRequester()
    val forward = FocusRequester()
    val timeBar = FocusRequester()
    val info = FocusRequester()
    val captions = FocusRequester()
    val settings = FocusRequester()

    operator fun get(control: PlayerControl): FocusRequester = when (control) {
        PlayerControl.Replay -> replay
        PlayerControl.PlayPause -> playPause
        PlayerControl.Forward -> forward
        PlayerControl.TimeBar -> timeBar
        PlayerControl.Info -> info
        PlayerControl.Captions -> captions
        PlayerControl.Settings -> settings
    }
}

@Stable
internal class PlayerFocusCoordinator(
    private val chromeState: PlayerChromeState,
) {
    val requesters = PlayerFocusRequesters()

    internal val focusedControl: PlayerControl?
        get() = chromeState.focusedControl

    internal val pendingTarget: PlayerControl?
        get() = pendingIntent?.target

    internal val pendingRevision: Long
        get() = pendingRevisionState

    private var pendingIntent by mutableStateOf<FocusIntent?>(null)
    private var pendingRevisionState by mutableLongStateOf(0L)
    private var nextGeneration = 0L
    private var lastFocusedControl: PlayerControl? = null
    private var latestCaps: FocusCaps? = null
    private var wasVisible = false
    private var wasPopupOpen = false
    private var fallbackTarget: FallbackTarget? = null

    /** Observes chrome lifecycle transitions and creates focus intents where needed. */
    internal fun observe(
        controlsVisible: Boolean,
        popupOpen: Boolean,
        touchMode: Boolean,
        caps: FocusCaps,
    ) {
        latestCaps = caps
        if (!controlsVisible) {
            chromeState.focusedControl = null
            lastFocusedControl = null
            clearPending()
        } else {
            recoverFocus(caps)
            if (!wasVisible && !popupOpen && !touchMode) {
                requestDefaultFocus()
            } else if (
                wasVisible &&
                !popupOpen &&
                !touchMode &&
                chromeState.focusedControl == null &&
                pendingIntent == null
            ) {
                requestPending(
                    target = defaultTarget(caps),
                    reason = FocusIntentReason.Recovery,
                )
            }
            if (wasPopupOpen && !popupOpen) {
                requestSettingsRestore()
            }
        }

        wasVisible = controlsVisible
        wasPopupOpen = popupOpen
    }

    /** Creates the stock hidden-to-visible intent: play/pause, not the last focused control. */
    internal fun requestDefaultFocus() {
        setPending(PlayerControl.PlayPause, FocusIntentReason.Default)
    }

    /** Creates the explicit gear restoration intent after a separate Popup window closes. */
    internal fun requestSettingsRestore() {
        setPending(PlayerControl.Settings, FocusIntentReason.PopupRestore)
    }

    /** Receives actual Compose focus state from the chrome controls. */
    internal fun onFocusChanged(control: PlayerControl, isFocused: Boolean) {
        if (isFocused) {
            val previous = chromeState.focusedControl
            val wasPreviouslyFocused = previous == control || lastFocusedControl == control
            chromeState.focusedControl = control
            lastFocusedControl = control
            val intent = pendingIntent
            if (
                intent != null &&
                control != intent.target &&
                !isTemporaryFallback(control, intent) &&
                intent.reason != FocusIntentReason.PopupRestore &&
                !wasPreviouslyFocused
            ) {
                clearPending()
            }
        } else if (chromeState.focusedControl == control) {
            chromeState.focusedControl = null
        }
    }

    /** Rehomes focus when the currently focused control is disabled or removed. */
    internal fun recoverFocus(caps: FocusCaps) {
        latestCaps = caps
        val controls = available(caps)
        val focused = chromeState.focusedControl ?: lastFocusedControl
        if (focused == null || focused in controls) return

        // A popup-close restore is explicit and must win over stale host focus.
        if (pendingIntent?.reason == FocusIntentReason.PopupRestore) return
        chromeState.focusedControl = null
        requestPending(fallbackFor(focused, caps), FocusIntentReason.Recovery)
    }

    /**
     * Runs the current intent. A target unavailable during PREPARING is requested through the
     * temporary Settings fallback while its PlayPause intent remains pending for READY.
     */
    internal suspend fun runPending(
        caps: FocusCaps,
        windowFocused: Boolean,
        touchMode: Boolean,
        awaitNextFrame: suspend () -> Unit,
        maxFrames: Int = 30,
    ): Boolean {
        val intent = pendingIntent ?: return false
        if (!windowFocused) return false
        if (touchMode && intent.reason != FocusIntentReason.PopupRestore) return false

        val controls = available(caps)
        val target = intent.target.takeIf { it in controls } ?: defaultTarget(caps)
        val generation = intent.generation
        if (target == intent.target && focusedControl == target) {
            clearPendingIf(generation)
            return true
        }
        if (
            target != intent.target &&
            fallbackTarget?.generation == generation &&
            fallbackTarget?.target == target &&
            focusedControl == target
        ) {
            return true
        }

        if (target != intent.target) {
            fallbackTarget = FallbackTarget(generation, target)
        }
        val acknowledged = awaitFocusAcknowledgement(
            requestFocus = { requesters[target].requestFocus() },
            isFocused = { focusedControl == target },
            isCurrent = { isPending(generation) && windowFocused },
            awaitNextFrame = awaitNextFrame,
            maxFrames = maxFrames,
        )
        if (!acknowledged || !isPending(generation)) return acknowledged

        if (target == intent.target) {
            clearPendingIf(generation)
        } else {
            fallbackTarget = FallbackTarget(generation, target)
        }
        return true
    }

    /** The root is a hidden-state sink and does not participate in PlayerControl reporting. */
    internal fun requestRootFocus() {
        runCatching { requesters.root.requestFocus() }
    }

    private fun requestPending(target: PlayerControl, reason: FocusIntentReason) {
        val current = pendingIntent
        if (current?.target == target && current.reason == reason) return
        pendingIntent = FocusIntent(
            target = target,
            reason = reason,
            generation = nextGeneration(),
        )
        fallbackTarget = null
        pendingRevisionState++
    }

    private fun setPending(target: PlayerControl, reason: FocusIntentReason) {
        requestPending(target, reason)
    }

    private fun clearPending() {
        if (pendingIntent == null && fallbackTarget == null) return
        pendingIntent = null
        fallbackTarget = null
        pendingRevisionState++
    }

    private fun clearPendingIf(generation: Long) {
        if (pendingIntent?.generation == generation) clearPending()
    }

    private fun isPending(generation: Long): Boolean = pendingIntent?.generation == generation

    private fun isTemporaryFallback(
        control: PlayerControl,
        intent: FocusIntent,
    ): Boolean {
        if (intent.reason != FocusIntentReason.Default || intent.target != PlayerControl.PlayPause) {
            return false
        }
        val caps = latestCaps ?: return false
        return (
            fallbackTarget?.generation == intent.generation && fallbackTarget?.target == control
        ) || (intent.target !in available(caps) && defaultTarget(caps) == control)
    }

    private fun nextGeneration(): Long {
        nextGeneration = if (nextGeneration == Long.MAX_VALUE) 1L else nextGeneration + 1L
        return nextGeneration
    }

    private data class FocusIntent(
        val target: PlayerControl,
        val reason: FocusIntentReason,
        val generation: Long,
    )

    private data class FallbackTarget(
        val generation: Long,
        val target: PlayerControl,
    )

    private enum class FocusIntentReason {
        Default,
        Recovery,
        PopupRestore,
    }
}
