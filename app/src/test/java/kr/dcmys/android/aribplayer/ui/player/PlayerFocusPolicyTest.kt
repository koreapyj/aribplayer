package kr.dcmys.android.aribplayer.ui.player

import kr.dcmys.android.aribplayer.PlaybackState
import java.util.ArrayDeque
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class PlayerFocusPolicyTest {
    @Test
    fun defaultTargetUsesPlayPauseOnlyForPlaybackStates() {
        val playbackStates = setOf(
            PlaybackState.READY,
            PlaybackState.PLAYING,
            PlaybackState.PAUSED,
            PlaybackState.ENDED,
        )
        val nonPlaybackStates = setOf(
            PlaybackState.IDLE,
            PlaybackState.PREPARING,
            PlaybackState.ERROR,
        )

        playbackStates.forEach { state ->
            assertEquals(PlayerControl.PlayPause, defaultTarget(caps(state)))
        }
        nonPlaybackStates.forEach { state ->
            assertEquals(PlayerControl.Settings, defaultTarget(caps(state)))
        }
    }

    @Test
    fun availabilityMatchesRenderedPlaybackAndSeekControls() {
        val unavailable = setOf(PlayerControl.Info, PlayerControl.Settings)
        listOf(PlaybackState.IDLE, PlaybackState.PREPARING, PlaybackState.ERROR).forEach { state ->
            assertEquals(unavailable, available(caps(state, seekable = true)))
        }

        val playbackWithoutSeek = setOf(PlayerControl.PlayPause, PlayerControl.Info, PlayerControl.Settings)
        listOf(
            PlaybackState.READY,
            PlaybackState.PLAYING,
            PlaybackState.PAUSED,
            PlaybackState.ENDED,
        ).forEach { state ->
            assertEquals(playbackWithoutSeek, available(caps(state, seekable = false)))
            assertEquals(playbackWithoutSeek, available(caps(state, durationMs = 0L)))
        }

        val allControls = PlayerControl.entries.toSet()
        assertEquals(allControls, available(caps(PlaybackState.PLAYING, hasSubtitles = true)))
    }

    @Test
    fun captionsAreAvailableOnlyWhenPresent() {
        val withoutCaptions = available(caps(PlaybackState.PLAYING, hasSubtitles = false))
        val withCaptions = available(caps(PlaybackState.PLAYING, hasSubtitles = true))

        assertFalse(PlayerControl.Captions in withoutCaptions)
        assertTrue(PlayerControl.Captions in withCaptions)
    }

    @Test
    fun fallbackKeepsNavigationOnTheRenderedGraph() {
        val playable = caps(PlaybackState.PLAYING, hasSubtitles = true)
        assertEquals(PlayerControl.PlayPause, fallbackFor(PlayerControl.Replay, playable))
        assertEquals(PlayerControl.PlayPause, fallbackFor(PlayerControl.Forward, playable))
        assertEquals(PlayerControl.PlayPause, fallbackFor(PlayerControl.TimeBar, playable))
        assertEquals(PlayerControl.Captions, fallbackFor(PlayerControl.Info, playable))
        assertEquals(PlayerControl.Settings, fallbackFor(PlayerControl.Captions, playable))
        assertEquals(PlayerControl.Info, fallbackFor(PlayerControl.Settings, playable))

        val preparing = caps(PlaybackState.PREPARING, hasSubtitles = false)
        assertEquals(PlayerControl.Settings, fallbackFor(PlayerControl.Replay, preparing))
        assertEquals(PlayerControl.Settings, fallbackFor(PlayerControl.Forward, preparing))
        assertEquals(PlayerControl.Settings, fallbackFor(PlayerControl.TimeBar, preparing))
        assertEquals(PlayerControl.Settings, fallbackFor(PlayerControl.PlayPause, preparing))
    }

    @Test
    fun everyAvailableControlIsReachableFromTheDefaultTarget() {
        val states = PlaybackState.entries
        val seekability = listOf(false, true)
        val subtitles = listOf(false, true)

        states.forEach { state ->
            seekability.forEach { seekable ->
                subtitles.forEach { hasSubtitles ->
                    val focusCaps = caps(state, seekable = seekable, hasSubtitles = hasSubtitles)
                    val expectedDefault = if (
                        state in setOf(
                            PlaybackState.READY,
                            PlaybackState.PLAYING,
                            PlaybackState.PAUSED,
                            PlaybackState.ENDED,
                        )
                    ) {
                        PlayerControl.PlayPause
                    } else {
                        PlayerControl.Settings
                    }
                    assertEquals(expectedDefault, defaultTarget(focusCaps))
                    val reachable = reachableFrom(defaultTarget(focusCaps), focusCaps)
                    assertEquals(available(focusCaps), reachable)
                }
            }
        }
    }

    @Test
    fun rootIsNeverAnAvailableOrVisibleControl() {
        assertTrue(PlayerControl.entries.none { it.name == "Root" })
    }

    @Test
    fun coordinatorKeepsPreparingPlayPauseIntentThroughSettingsFallback() = runTest {
        val chrome = PlayerChromeState()
        val coordinator = PlayerFocusCoordinator(chrome)
        val preparing = caps(PlaybackState.PREPARING)
        coordinator.observe(
            controlsVisible = true,
            popupOpen = false,
            touchMode = false,
            caps = preparing,
        )

        assertEquals(PlayerControl.PlayPause, coordinator.pendingTarget)
        var frame = 0
        assertTrue(
            coordinator.runPending(
                caps = preparing,
                windowFocused = true,
                touchMode = false,
                awaitNextFrame = {
                    frame++
                    if (frame == 2) coordinator.onFocusChanged(PlayerControl.Settings, true)
                },
                maxFrames = 3,
            ),
        )
        assertEquals(PlayerControl.Settings, coordinator.focusedControl)
        assertEquals(PlayerControl.PlayPause, coordinator.pendingTarget)

        val ready = caps(PlaybackState.READY)
        coordinator.observe(
            controlsVisible = true,
            popupOpen = false,
            touchMode = false,
            caps = ready,
        )
        assertTrue(
            coordinator.runPending(
                caps = ready,
                windowFocused = true,
                touchMode = false,
                awaitNextFrame = { coordinator.onFocusChanged(PlayerControl.PlayPause, true) },
                maxFrames = 2,
            ),
        )
        assertEquals(PlayerControl.PlayPause, coordinator.focusedControl)
        assertEquals(null, coordinator.pendingTarget)
    }

    @Test
    fun coordinatorTimeoutKeepsPendingIntentForTheNextTrigger() = runTest {
        val chrome = PlayerChromeState()
        val coordinator = PlayerFocusCoordinator(chrome)
        val ready = caps(PlaybackState.READY)
        coordinator.observe(true, false, false, ready)

        assertFalse(
            coordinator.runPending(
                caps = ready,
                windowFocused = true,
                touchMode = false,
                awaitNextFrame = {},
                maxFrames = 2,
            ),
        )
        assertEquals(PlayerControl.PlayPause, coordinator.pendingTarget)
    }

    @Test
    fun coordinatorGenerationChangeCancelsStaleLoop() = runTest {
        val chrome = PlayerChromeState()
        val coordinator = PlayerFocusCoordinator(chrome)
        val ready = caps(PlaybackState.READY)
        coordinator.observe(true, false, false, ready)

        assertFalse(
            coordinator.runPending(
                caps = ready,
                windowFocused = true,
                touchMode = false,
                awaitNextFrame = { coordinator.requestSettingsRestore() },
                maxFrames = 3,
            ),
        )
        assertEquals(PlayerControl.Settings, coordinator.pendingTarget)
    }

    @Test
    fun coordinatorClearsTemporaryIntentOnlyAfterActualFocusMovement() = runTest {
        val chrome = PlayerChromeState()
        val coordinator = PlayerFocusCoordinator(chrome)
        val preparing = caps(PlaybackState.PREPARING)
        coordinator.observe(true, false, false, preparing)
        coordinator.runPending(
            caps = preparing,
            windowFocused = true,
            touchMode = false,
            awaitNextFrame = { coordinator.onFocusChanged(PlayerControl.Settings, true) },
            maxFrames = 1,
        )
        assertEquals(PlayerControl.PlayPause, coordinator.pendingTarget)

        coordinator.onFocusChanged(PlayerControl.Info, true)
        assertEquals(null, coordinator.pendingTarget)
    }

    @Test
    fun coordinatorPopupRestoreOverridesStaleHostFocus() = runTest {
        val chrome = PlayerChromeState()
        val coordinator = PlayerFocusCoordinator(chrome)
        val ready = caps(PlaybackState.READY)
        coordinator.onFocusChanged(PlayerControl.PlayPause, true)
        coordinator.observe(true, false, false, ready)
        coordinator.observe(true, true, false, ready)
        coordinator.observe(true, false, false, ready)

        assertEquals(PlayerControl.Settings, coordinator.pendingTarget)
        assertFalse(
            coordinator.runPending(
                caps = ready,
                windowFocused = true,
                touchMode = false,
                awaitNextFrame = { coordinator.onFocusChanged(PlayerControl.PlayPause, true) },
                maxFrames = 1,
            ),
        )
        assertEquals(PlayerControl.Settings, coordinator.pendingTarget)
    }

    @Test
    fun coordinatorRearmsDefaultWhenActualFocusIsNull() = runTest {
        val chrome = PlayerChromeState()
        val coordinator = PlayerFocusCoordinator(chrome)
        val ready = caps(PlaybackState.READY)
        coordinator.observe(true, false, false, ready)
        coordinator.runPending(
            caps = ready,
            windowFocused = true,
            touchMode = false,
            awaitNextFrame = { coordinator.onFocusChanged(PlayerControl.PlayPause, true) },
            maxFrames = 1,
        )
        coordinator.onFocusChanged(PlayerControl.PlayPause, false)
        coordinator.observe(true, false, false, ready)

        assertEquals(PlayerControl.PlayPause, coordinator.pendingTarget)
    }

    @Test
    fun acknowledgementRequiresActualFocusInsteadOfUnitRequestSuccess() = runTest {
        var requestCount = 0
        var frameCount = 0
        val acknowledged = awaitFocusAcknowledgement(
            requestFocus = { requestCount++ },
            isFocused = { false },
            awaitNextFrame = { frameCount++ },
            maxFrames = 3,
        )

        assertFalse(acknowledged)
        assertEquals(3, requestCount)
        assertEquals(3, frameCount)
    }

    @Test
    fun acknowledgementSucceedsWhenFocusReportsOnALaterRetry() = runTest {
        var requestCount = 0
        var focused = false
        val acknowledged = awaitFocusAcknowledgement(
            requestFocus = {
                requestCount++
                focused = requestCount == 2
            },
            isFocused = { focused },
            awaitNextFrame = {},
            maxFrames = 3,
        )

        assertTrue(acknowledged)
        assertEquals(2, requestCount)
    }

    @Test
    fun generationInvalidationStopsBeforeAStaleRequest() = runTest {
        var requestCount = 0
        var current = true
        val acknowledged = awaitFocusAcknowledgement(
            requestFocus = {
                requestCount++
                current = false
            },
            isFocused = { false },
            isCurrent = { current },
            awaitNextFrame = {},
            maxFrames = 3,
        )

        assertFalse(acknowledged)
        assertEquals(1, requestCount)
    }

    @Test
    fun requestAttachmentErrorsAreRetriedButCancellationPropagates() = runTest {
        var requestCount = 0
        var focused = false
        val acknowledged = awaitFocusAcknowledgement(
            requestFocus = {
                requestCount++
                if (requestCount == 1) throw IllegalStateException("not attached")
                focused = true
            },
            isFocused = { focused },
            awaitNextFrame = {},
            maxFrames = 3,
        )
        assertTrue(acknowledged)
        assertEquals(2, requestCount)

        val cancellation = runCatching {
            awaitFocusAcknowledgement(
                requestFocus = {},
                isFocused = { false },
                awaitNextFrame = { throw CancellationException("cancelled") },
            )
        }.exceptionOrNull()
        assertTrue(cancellation is CancellationException)
    }

    private fun caps(
        playbackState: PlaybackState,
        seekable: Boolean = true,
        durationMs: Long = 300_000L,
        hasSubtitles: Boolean = false,
    ): FocusCaps = FocusCaps(
        playbackState = playbackState,
        isSeekable = seekable,
        durationMs = durationMs,
        hasSubtitles = hasSubtitles,
    )

    private fun reachableFrom(start: PlayerControl, caps: FocusCaps): Set<PlayerControl> {
        val allowed = available(caps)
        val visited = linkedSetOf<PlayerControl>()
        val queue = ArrayDeque<PlayerControl>().apply { add(start) }
        while (queue.isNotEmpty()) {
            val control = queue.removeFirst()
            if (control !in allowed || !visited.add(control)) continue
            neighbors(control, allowed).forEach(queue::addLast)
        }
        return visited
    }

    private fun neighbors(control: PlayerControl, allowed: Set<PlayerControl>): Set<PlayerControl> = when (control) {
        PlayerControl.Replay -> setOf(PlayerControl.PlayPause)
        PlayerControl.PlayPause -> setOf(
            PlayerControl.Replay,
            PlayerControl.Forward,
            if (PlayerControl.TimeBar in allowed) PlayerControl.TimeBar else PlayerControl.Info,
        )
        PlayerControl.Forward -> setOf(PlayerControl.PlayPause)
        PlayerControl.TimeBar -> setOf(PlayerControl.PlayPause, PlayerControl.Info)
        PlayerControl.Info -> setOf(
            if (PlayerControl.TimeBar in allowed) PlayerControl.TimeBar else PlayerControl.PlayPause,
            if (PlayerControl.Captions in allowed) PlayerControl.Captions else PlayerControl.Settings,
        )
        PlayerControl.Captions -> setOf(PlayerControl.Info, PlayerControl.Settings)
        PlayerControl.Settings -> setOf(
            if (PlayerControl.Captions in allowed) PlayerControl.Captions else PlayerControl.Info,
        )
    }
}
