package kr.dcmys.android.aribplayer.ui.player

import android.view.KeyEvent
import kr.dcmys.android.aribplayer.PlaybackState
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class PlayerKeyMapTest {
    @Test
    fun hiddenBaselineRevealsAndSeeksOnlyWhenChromeIsHidden() {
        val hidden = context(touchMode = false, controlsVisible = false, popupOpen = false)
        val visible = hidden.copy(controlsVisible = true)
        val popup = hidden.copy(popupOpen = true)

        for (keyCode in listOf(
            KeyEvent.KEYCODE_DPAD_UP,
            KeyEvent.KEYCODE_DPAD_DOWN,
            KeyEvent.KEYCODE_DPAD_CENTER,
            KeyEvent.KEYCODE_ENTER,
        )) {
            assertEquals(
                PlayerKeyAction.RevealControls,
                playerKeyBindingFor(keyCode, hidden, SEEK_STEP_MS)?.action,
            )
            assertNull(playerKeyBindingFor(keyCode, visible, SEEK_STEP_MS))
            assertNull(playerKeyBindingFor(keyCode, popup, SEEK_STEP_MS))
        }

        assertEquals(
            PlayerKeyAction.SeekRelative(-SEEK_STEP_MS),
            playerKeyBindingFor(KeyEvent.KEYCODE_DPAD_LEFT, hidden, SEEK_STEP_MS)?.action,
        )
        assertEquals(
            PlayerKeyAction.SeekRelative(SEEK_STEP_MS),
            playerKeyBindingFor(KeyEvent.KEYCODE_DPAD_RIGHT, hidden, SEEK_STEP_MS)?.action,
        )
        assertNull(
            playerKeyBindingFor(KeyEvent.KEYCODE_DPAD_LEFT, visible, SEEK_STEP_MS),
        )
        assertNull(
            playerKeyBindingFor(KeyEvent.KEYCODE_DPAD_RIGHT, popup, SEEK_STEP_MS),
        )
    }

    @Test
    fun acceleratorAliasesKeepTheir0205Actions() {
        val context = context(touchMode = false, controlsVisible = true, popupOpen = false)
        assertActionFor(
            context,
            PlayerKeyAction.ToggleSettings,
            KeyEvent.KEYCODE_MENU,
            KeyEvent.KEYCODE_SETTINGS,
        )
        assertActionFor(
            context,
            PlayerKeyAction.ToggleDiagnostics,
            KeyEvent.KEYCODE_INFO,
            KeyEvent.KEYCODE_PROG_RED,
        )
        assertActionFor(
            context,
            PlayerKeyAction.ToggleCaptions,
            KeyEvent.KEYCODE_CAPTIONS,
            KeyEvent.KEYCODE_PROG_GREEN,
        )
        assertActionFor(
            context,
            PlayerKeyAction.PlayPause,
            KeyEvent.KEYCODE_MEDIA_PLAY_PAUSE,
        )
        assertActionFor(
            context,
            PlayerKeyAction.Play,
            KeyEvent.KEYCODE_MEDIA_PLAY,
        )
        assertActionFor(
            context,
            PlayerKeyAction.Pause,
            KeyEvent.KEYCODE_MEDIA_PAUSE,
        )
        assertActionFor(
            context,
            PlayerKeyAction.Stop,
            KeyEvent.KEYCODE_MEDIA_STOP,
        )
        assertActionFor(
            context,
            PlayerKeyAction.SeekRelative(-SEEK_STEP_MS),
            KeyEvent.KEYCODE_MEDIA_REWIND,
        )
        assertActionFor(
            context,
            PlayerKeyAction.SeekRelative(SEEK_STEP_MS),
            KeyEvent.KEYCODE_MEDIA_FAST_FORWARD,
        )
        assertActionFor(
            context,
            PlayerKeyAction.SeekRelative(-FIXED_MEDIA_SEEK_MS),
            KeyEvent.KEYCODE_MEDIA_PREVIOUS,
            KeyEvent.KEYCODE_MEDIA_SKIP_BACKWARD,
        )
        assertActionFor(
            context,
            PlayerKeyAction.SeekRelative(FIXED_MEDIA_SEEK_MS),
            KeyEvent.KEYCODE_MEDIA_NEXT,
            KeyEvent.KEYCODE_MEDIA_SKIP_FORWARD,
        )
        for (number in 0..9) {
            assertEquals(
                PlayerKeyAction.SeekPercent(number),
                playerKeyBindingFor(
                    KeyEvent.KEYCODE_0 + number,
                    context,
                    SEEK_STEP_MS,
                )?.action,
            )
        }
    }

    @Test
    fun touchModeOnlyAllowsTheThreeLegacyMediaKeys() {
        val touchContext = context(touchMode = true, controlsVisible = true, popupOpen = false)
        val legacyMediaKeys = setOf(
            KeyEvent.KEYCODE_MEDIA_PLAY_PAUSE,
            KeyEvent.KEYCODE_MEDIA_PLAY,
            KeyEvent.KEYCODE_MEDIA_PAUSE,
        )
        val allKeys = playerKeyBindings(SEEK_STEP_MS).flatMap { it.keys }.toSet()

        for (keyCode in legacyMediaKeys) {
            assertTrue(playerKeyBindingFor(keyCode, touchContext, SEEK_STEP_MS) != null)
        }
        for (keyCode in allKeys - legacyMediaKeys) {
            assertNull(playerKeyBindingFor(keyCode, touchContext, SEEK_STEP_MS))
        }
    }

    @Test
    fun seekBindingsRepeatAndAcceleratorsAreForwardedToPopup() {
        val bindings = playerKeyBindings(SEEK_STEP_MS)
        val seekBindings = bindings.filter { it.action is PlayerKeyAction.SeekRelative }
        assertTrue(seekBindings.isNotEmpty())
        assertTrue(seekBindings.all { it.repeat == RepeatPolicy.Repeating })
        val expectedUnion = bindings
            .filter { it.popup == PopupPolicy.Forward }
            .flatMap { it.keys }
            .toSet()
        assertEquals(expectedUnion, popupForwardKeys())

        val expectedPopupKeys = setOf(
            KeyEvent.KEYCODE_MENU,
            KeyEvent.KEYCODE_SETTINGS,
            KeyEvent.KEYCODE_MEDIA_PLAY_PAUSE,
            KeyEvent.KEYCODE_MEDIA_PLAY,
            KeyEvent.KEYCODE_MEDIA_PAUSE,
            KeyEvent.KEYCODE_MEDIA_STOP,
            KeyEvent.KEYCODE_MEDIA_REWIND,
            KeyEvent.KEYCODE_MEDIA_FAST_FORWARD,
            KeyEvent.KEYCODE_MEDIA_PREVIOUS,
            KeyEvent.KEYCODE_MEDIA_NEXT,
            KeyEvent.KEYCODE_MEDIA_SKIP_BACKWARD,
            KeyEvent.KEYCODE_MEDIA_SKIP_FORWARD,
            KeyEvent.KEYCODE_INFO,
            KeyEvent.KEYCODE_PROG_RED,
            KeyEvent.KEYCODE_CAPTIONS,
            KeyEvent.KEYCODE_PROG_GREEN,
        ) + (KeyEvent.KEYCODE_0..KeyEvent.KEYCODE_9).toSet()
        assertEquals(expectedPopupKeys, popupForwardKeys())
        assertFalse(KeyEvent.KEYCODE_DPAD_UP in popupForwardKeys())
        assertFalse(KeyEvent.KEYCODE_DPAD_DOWN in popupForwardKeys())
        assertFalse(KeyEvent.KEYCODE_DPAD_LEFT in popupForwardKeys())
        assertFalse(KeyEvent.KEYCODE_DPAD_RIGHT in popupForwardKeys())
        assertFalse(KeyEvent.KEYCODE_DPAD_CENTER in popupForwardKeys())
        assertFalse(KeyEvent.KEYCODE_ENTER in popupForwardKeys())
        assertFalse(KeyEvent.KEYCODE_BACK in popupForwardKeys())
    }

    @Test
    fun everyBindingHasExpectedEligibilityAcrossChromeAndTouchModes() {
        val bindings = playerKeyBindings(SEEK_STEP_MS)
        val contexts = listOf(
            "hidden-key" to context(touchMode = false, controlsVisible = false, popupOpen = false),
            "visible-key" to context(touchMode = false, controlsVisible = true, popupOpen = false),
            "popup-key" to context(touchMode = false, controlsVisible = true, popupOpen = true),
            "hidden-touch" to context(touchMode = true, controlsVisible = false, popupOpen = false),
            "visible-touch" to context(touchMode = true, controlsVisible = true, popupOpen = false),
            "popup-touch" to context(touchMode = true, controlsVisible = true, popupOpen = true),
        )

        for (binding in bindings) {
            for ((label, context) in contexts) {
                for (keyCode in binding.keys) {
                    val expected = binding.whenMatches(context) &&
                        (binding.touch == TouchPolicy.AlsoInTouch || !context.touchMode)
                    val actual = bindings.firstOrNull { it.matches(keyCode, context) }
                    assertEquals(
                        "$label key $keyCode eligibility",
                        binding.takeIf { expected },
                        actual,
                    )
                }
            }
        }
    }

    @Test
    fun noBindingClaimsAFrameworkGamepadButton() {
        val buttonCodes = setOf(
            KeyEvent.KEYCODE_BUTTON_A,
            KeyEvent.KEYCODE_BUTTON_B,
            KeyEvent.KEYCODE_BUTTON_C,
            KeyEvent.KEYCODE_BUTTON_X,
            KeyEvent.KEYCODE_BUTTON_Y,
            KeyEvent.KEYCODE_BUTTON_Z,
            KeyEvent.KEYCODE_BUTTON_L1,
            KeyEvent.KEYCODE_BUTTON_R1,
            KeyEvent.KEYCODE_BUTTON_L2,
            KeyEvent.KEYCODE_BUTTON_R2,
            KeyEvent.KEYCODE_BUTTON_START,
            KeyEvent.KEYCODE_BUTTON_SELECT,
            KeyEvent.KEYCODE_BUTTON_MODE,
            KeyEvent.KEYCODE_BUTTON_THUMBL,
            KeyEvent.KEYCODE_BUTTON_THUMBR,
            KeyEvent.KEYCODE_BUTTON_1,
            KeyEvent.KEYCODE_BUTTON_2,
            KeyEvent.KEYCODE_BUTTON_3,
            KeyEvent.KEYCODE_BUTTON_4,
            KeyEvent.KEYCODE_BUTTON_5,
            KeyEvent.KEYCODE_BUTTON_6,
            KeyEvent.KEYCODE_BUTTON_7,
            KeyEvent.KEYCODE_BUTTON_8,
            KeyEvent.KEYCODE_BUTTON_9,
            KeyEvent.KEYCODE_BUTTON_10,
            KeyEvent.KEYCODE_BUTTON_11,
            KeyEvent.KEYCODE_BUTTON_12,
            KeyEvent.KEYCODE_BUTTON_13,
            KeyEvent.KEYCODE_BUTTON_14,
            KeyEvent.KEYCODE_BUTTON_15,
            KeyEvent.KEYCODE_BUTTON_16,
        )
        val contexts = listOf(
            context(touchMode = false, controlsVisible = false, popupOpen = false),
            context(touchMode = false, controlsVisible = true, popupOpen = false),
            context(touchMode = false, controlsVisible = true, popupOpen = true),
            context(touchMode = true, controlsVisible = false, popupOpen = false),
            context(touchMode = true, controlsVisible = true, popupOpen = true),
        )

        for (keyCode in buttonCodes) {
            for (context in contexts) {
                assertNull(playerKeyBindingFor(keyCode, context, SEEK_STEP_MS))
            }
        }
        assertTrue(popupForwardKeys().intersect(buttonCodes).isEmpty())
    }

    @Test
    fun noTwoBindingsClaimTheSameKey() {
        val owners = mutableMapOf<Int, KeyBinding>()
        for (binding in playerKeyBindings(SEEK_STEP_MS)) {
            for (keyCode in binding.keys) {
                assertTrue(
                    "key $keyCode is claimed by more than one binding",
                    owners.put(keyCode, binding) == null,
                )
            }
        }
    }

    private fun assertActionFor(
        context: PlayerKeyContext,
        expected: PlayerKeyAction,
        vararg keyCodes: Int,
    ) {
        for (keyCode in keyCodes) {
            assertEquals(
                expected,
                playerKeyBindingFor(keyCode, context, SEEK_STEP_MS)?.action,
            )
        }
    }

    private fun context(
        touchMode: Boolean,
        controlsVisible: Boolean,
        popupOpen: Boolean,
    ) = PlayerKeyContext(
        touchMode = touchMode,
        controlsVisible = controlsVisible,
        popupOpen = popupOpen,
        playbackState = PlaybackState.PLAYING,
        isSeekable = true,
        durationMs = 300_000L,
        hasSubtitles = true,
        focusedControl = null,
    )

    private companion object {
        const val SEEK_STEP_MS = 30_000L
        const val FIXED_MEDIA_SEEK_MS = 60_000L
    }
}
