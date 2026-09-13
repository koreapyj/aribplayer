package kr.dcmys.android.aribplayer.ui.player

import android.view.KeyEvent
import kr.dcmys.android.aribplayer.PlaybackState

internal data class PlayerKeyContext(
    val touchMode: Boolean,
    val controlsVisible: Boolean,
    val popupOpen: Boolean,
    val playbackState: PlaybackState,
    val isSeekable: Boolean,
    val durationMs: Long,
    val hasSubtitles: Boolean,
    val focusedControl: PlayerControl?,
)

internal sealed interface PlayerKeyAction {
    data object RevealControls : PlayerKeyAction
    data object ToggleSettings : PlayerKeyAction
    data object ToggleDiagnostics : PlayerKeyAction
    data object ToggleCaptions : PlayerKeyAction
    data object PlayPause : PlayerKeyAction
    data object Play : PlayerKeyAction
    data object Pause : PlayerKeyAction
    data object Stop : PlayerKeyAction
    data object PassThrough : PlayerKeyAction
    data class SeekRelative(val ms: Long) : PlayerKeyAction
    data class SeekPercent(val n: Int) : PlayerKeyAction
}

internal enum class RepeatPolicy {
    FirstDownOnly,
    Repeating,
}

internal enum class PopupPolicy {
    Forward,
    Ignore,
}

internal enum class TouchPolicy {
    KeyModeOnly,
    AlsoInTouch,
}

internal data class KeyBinding(
    val keys: Set<Int>,
    val action: PlayerKeyAction,
    val whenMatches: (PlayerKeyContext) -> Boolean = { true },
    val repeat: RepeatPolicy = RepeatPolicy.FirstDownOnly,
    val popup: PopupPolicy = PopupPolicy.Forward,
    val touch: TouchPolicy = TouchPolicy.KeyModeOnly,
)

internal fun KeyBinding.matches(keyCode: Int, context: PlayerKeyContext): Boolean =
    keyCode in keys &&
        (touch == TouchPolicy.AlsoInTouch || !context.touchMode) &&
        whenMatches(context)

internal fun playerKeyBindingFor(
    keyCode: Int,
    context: PlayerKeyContext,
    seekStepMs: Long,
): KeyBinding? = playerKeyBindings(seekStepMs).firstOrNull { it.matches(keyCode, context) }

internal fun playerKeyBindings(seekStepMs: Long): List<KeyBinding> {
    val step = seekStepMs.coerceAtLeast(0L)
    val controlsHidden: (PlayerKeyContext) -> Boolean = {
        !it.controlsVisible && !it.popupOpen
    }

    return listOf(
        // Hidden chrome follows stock reveal behavior. ENTER is included for framework remotes;
        // popup forwarding deliberately ignores the whole baseline so Compose owns it there.
        KeyBinding(
            keys = setOf(
                KeyEvent.KEYCODE_DPAD_UP,
                KeyEvent.KEYCODE_DPAD_DOWN,
                KeyEvent.KEYCODE_DPAD_CENTER,
                KeyEvent.KEYCODE_ENTER,
            ),
            action = PlayerKeyAction.RevealControls,
            whenMatches = controlsHidden,
            popup = PopupPolicy.Ignore,
        ),
        // Hidden LEFT/RIGHT are the 0.0.5 remote seek exception to stock D-pad behavior.
        KeyBinding(
            keys = setOf(KeyEvent.KEYCODE_DPAD_LEFT),
            action = PlayerKeyAction.SeekRelative(-step),
            whenMatches = controlsHidden,
            repeat = RepeatPolicy.Repeating,
            popup = PopupPolicy.Ignore,
        ),
        KeyBinding(
            keys = setOf(KeyEvent.KEYCODE_DPAD_RIGHT),
            action = PlayerKeyAction.SeekRelative(step),
            whenMatches = controlsHidden,
            repeat = RepeatPolicy.Repeating,
            popup = PopupPolicy.Ignore,
        ),
        KeyBinding(
            keys = setOf(KeyEvent.KEYCODE_MEDIA_PLAY_PAUSE),
            action = PlayerKeyAction.PlayPause,
            touch = TouchPolicy.AlsoInTouch,
        ),
        KeyBinding(
            keys = setOf(KeyEvent.KEYCODE_MEDIA_PLAY),
            action = PlayerKeyAction.Play,
            touch = TouchPolicy.AlsoInTouch,
        ),
        KeyBinding(
            keys = setOf(KeyEvent.KEYCODE_MEDIA_PAUSE),
            action = PlayerKeyAction.Pause,
            touch = TouchPolicy.AlsoInTouch,
        ),
        KeyBinding(
            keys = setOf(KeyEvent.KEYCODE_MENU, KeyEvent.KEYCODE_SETTINGS),
            action = PlayerKeyAction.ToggleSettings,
        ),
        KeyBinding(
            keys = setOf(KeyEvent.KEYCODE_INFO, KeyEvent.KEYCODE_PROG_RED),
            action = PlayerKeyAction.ToggleDiagnostics,
        ),
        KeyBinding(
            keys = setOf(KeyEvent.KEYCODE_CAPTIONS, KeyEvent.KEYCODE_PROG_GREEN),
            action = PlayerKeyAction.ToggleCaptions,
        ),
        KeyBinding(
            keys = setOf(KeyEvent.KEYCODE_MEDIA_STOP),
            action = PlayerKeyAction.Stop,
        ),
        KeyBinding(
            keys = setOf(KeyEvent.KEYCODE_MEDIA_REWIND),
            action = PlayerKeyAction.SeekRelative(-step),
            repeat = RepeatPolicy.Repeating,
        ),
        KeyBinding(
            keys = setOf(KeyEvent.KEYCODE_MEDIA_FAST_FORWARD),
            action = PlayerKeyAction.SeekRelative(step),
            repeat = RepeatPolicy.Repeating,
        ),
        KeyBinding(
            keys = setOf(
                KeyEvent.KEYCODE_MEDIA_PREVIOUS,
                KeyEvent.KEYCODE_MEDIA_SKIP_BACKWARD,
            ),
            action = PlayerKeyAction.SeekRelative(-FIXED_MEDIA_SEEK_MS),
            repeat = RepeatPolicy.Repeating,
        ),
        KeyBinding(
            keys = setOf(
                KeyEvent.KEYCODE_MEDIA_NEXT,
                KeyEvent.KEYCODE_MEDIA_SKIP_FORWARD,
            ),
            action = PlayerKeyAction.SeekRelative(FIXED_MEDIA_SEEK_MS),
            repeat = RepeatPolicy.Repeating,
        ),
        *List(10) { number ->
            KeyBinding(
                keys = setOf(KeyEvent.KEYCODE_0 + number),
                action = PlayerKeyAction.SeekPercent(number),
            )
        }.toTypedArray(),
    )
}

internal fun popupForwardKeys(): Set<Int> = buildSet {
    playerKeyBindings(DEFAULT_SEEK_STEP_MS)
        .filter { it.popup == PopupPolicy.Forward }
        .forEach { addAll(it.keys) }
}

internal fun PlayerKeyAction.isTransportAction(): Boolean = when (this) {
    PlayerKeyAction.PlayPause,
    PlayerKeyAction.Play,
    PlayerKeyAction.Pause,
    PlayerKeyAction.Stop,
    -> true
    is PlayerKeyAction.SeekRelative,
    is PlayerKeyAction.SeekPercent,
    -> true
    PlayerKeyAction.RevealControls,
    PlayerKeyAction.ToggleSettings,
    PlayerKeyAction.ToggleDiagnostics,
    PlayerKeyAction.ToggleCaptions,
    PlayerKeyAction.PassThrough,
    -> false
}

private const val DEFAULT_SEEK_STEP_MS = 30_000L
private const val FIXED_MEDIA_SEEK_MS = 60_000L
