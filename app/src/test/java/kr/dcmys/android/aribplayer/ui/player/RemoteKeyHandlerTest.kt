package kr.dcmys.android.aribplayer.ui.player

import android.view.KeyEvent
import kr.dcmys.android.aribplayer.PlaybackState
import kr.dcmys.android.aribplayer.PlayerScreenDestination
import kr.dcmys.android.aribplayer.PlayerUiState
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.test.advanceTimeBy
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class RemoteKeyHandlerTest {
    @Test
    fun rapidRightPressesAreCoalescedIntoOneTrailingSeek() = runTest {
        val chrome = hiddenChrome()
        val controller = FakeController(baseState(positionMs = 10_000L))
        var now = 0L
        val handler = RemoteKeyHandler(
            viewModel = controller,
            chromeState = { chrome },
            onStop = { _, _ -> },
            scope = this,
            clock = { now },
        ).also { it.seekStepMs = 1_000L }

        assertTrue(handler.handle(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_DPAD_RIGHT, 0, false))
        assertEquals(11_000L, chrome.seekFeedback?.targetMs)
        advanceTimeBy(80L)
        now = 80L
        assertTrue(handler.handle(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_DPAD_RIGHT, 0, false))
        assertEquals(12_000L, chrome.seekFeedback?.targetMs)
        advanceTimeBy(80L)
        now = 160L
        assertTrue(handler.handle(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_DPAD_RIGHT, 0, false))
        assertEquals(13_000L, chrome.seekFeedback?.targetMs)

        advanceTimeBy(299L)
        assertTrue(controller.seeks.isEmpty())
        advanceTimeBy(2L)

        assertEquals(listOf(13_000L), controller.seeks)
        handler.release()
    }

    @Test
    fun idleBeforeRapidBurstStillWaitsForTrailingSeek() = runTest {
        val chrome = hiddenChrome()
        val controller = FakeController(baseState(positionMs = 10_000L))
        var now = 0L
        val handler = RemoteKeyHandler(
            viewModel = controller,
            chromeState = { chrome },
            onStop = { _, _ -> },
            scope = this,
            clock = { now },
        ).also { it.seekStepMs = 1_000L }

        advanceTimeBy(1_000L)
        now = 1_000L
        assertTrue(handler.handle(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_DPAD_RIGHT, 0, false))
        advanceTimeBy(80L)
        now = 1_080L
        assertTrue(handler.handle(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_DPAD_RIGHT, 0, false))
        advanceTimeBy(80L)
        now = 1_160L
        assertTrue(handler.handle(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_DPAD_RIGHT, 0, false))

        advanceTimeBy(299L)
        assertTrue(controller.seeks.isEmpty())
        advanceTimeBy(1L)
        runCurrent()

        assertEquals(listOf(13_000L), controller.seeks)
        handler.release()
    }

    @Test
    fun trailingSeekIsDroppedWhenPlaybackEntersErrorBeforeFlush() = runTest {
        val chrome = hiddenChrome()
        val controller = FakeController(baseState(positionMs = 10_000L))
        val handler = handler(this, controller, chrome)

        assertTrue(handler.handle(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_DPAD_RIGHT, 0, false))
        controller.state = controller.state.copy(playbackState = PlaybackState.ERROR)

        advanceTimeBy(299L)
        assertTrue(controller.seeks.isEmpty())
        advanceTimeBy(1L)
        runCurrent()

        assertTrue(controller.seeks.isEmpty())
        assertTrue(chrome.seekFeedback == null)
        handler.release()
    }

    @Test
    fun heldSeekFlushIsDroppedWhenPlaybackEntersErrorBeforeFlush() = runTest {
        val chrome = hiddenChrome()
        val controller = FakeController(baseState(positionMs = 10_000L))
        val handler = handler(this, controller, chrome)

        assertTrue(handler.handle(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_DPAD_RIGHT, 0, false))
        assertTrue(handler.handle(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_DPAD_RIGHT, 1, false))
        controller.state = controller.state.copy(playbackState = PlaybackState.ERROR)

        advanceTimeBy(249L)
        assertTrue(controller.seeks.isEmpty())
        advanceTimeBy(1L)
        runCurrent()

        assertTrue(controller.seeks.isEmpty())
        assertTrue(chrome.seekFeedback == null)
        controller.state = baseState(positionMs = 10_000L)
        advanceTimeBy(50L)
        runCurrent()
        assertTrue(controller.seeks.isEmpty())
        assertTrue(chrome.seekFeedback == null)
        handler.release()
    }

    @Test
    fun trailingSeekIsDroppedForNonPlayerAndUnseekableStates() = runTest {
        val invalidStates = listOf(
            "library" to baseState().copy(screen = PlayerScreenDestination.LIBRARY),
            "nonseekable" to baseState().copy(isSeekable = false),
            "zero-duration" to baseState().copy(durationMs = 0L),
        )

        for ((label, invalidState) in invalidStates) {
            val chrome = hiddenChrome()
            val controller = FakeController(baseState(positionMs = 10_000L))
            val handler = handler(this, controller, chrome)

            assertTrue(handler.handle(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_DPAD_RIGHT, 0, false))
            controller.state = invalidState

            advanceTimeBy(299L)
            assertTrue("$label seek ran early", controller.seeks.isEmpty())
            advanceTimeBy(1L)
            runCurrent()

            assertTrue("$label emitted a seek", controller.seeks.isEmpty())
            assertTrue("$label left feedback visible", chrome.seekFeedback == null)
            handler.release()
        }
    }

    @Test
    fun aLaterSeekBackToPreviousTargetIsNotSuppressed() = runTest {
        val chrome = hiddenChrome()
        val controller = FakeController(baseState(positionMs = 100_000L))
        val handler = handler(this, controller, chrome).also { it.seekStepMs = 1_000L }

        assertTrue(handler.handle(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_DPAD_RIGHT, 0, false))
        advanceTimeBy(301L)
        assertEquals(listOf(101_000L), controller.seeks)

        controller.state = controller.state.copy(positionMs = 102_000L)
        assertTrue(handler.handle(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_DPAD_LEFT, 0, false))
        advanceTimeBy(301L)

        assertEquals(listOf(101_000L, 101_000L), controller.seeks)
        handler.release()
    }

    @Test
    fun feedbackClears800MsAfterLastPress() = runTest {
        val chrome = hiddenChrome()
        val controller = FakeController(baseState(positionMs = 10_000L))
        var now = 0L
        val handler = RemoteKeyHandler(
            viewModel = controller,
            chromeState = { chrome },
            onStop = { _, _ -> },
            scope = this,
            clock = { now },
        ).also { it.seekStepMs = 1_000L }

        assertTrue(handler.handle(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_DPAD_RIGHT, 0, false))
        advanceTimeBy(100L)
        now = 100L
        assertTrue(handler.handle(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_DPAD_RIGHT, 0, false))
        assertTrue(chrome.seekFeedback != null)

        advanceTimeBy(799L)
        assertTrue(chrome.seekFeedback != null)
        advanceTimeBy(1L)
        runCurrent()
        assertTrue(chrome.seekFeedback == null)
        handler.release()
    }

    @Test
    fun leftIsLeftForComposeWhenControlsAreVisible() = runTest {
        val chrome = PlayerChromeState()
        val controller = FakeController(baseState())
        val handler = handler(this, controller, chrome)

        assertFalse(handler.handle(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_DPAD_LEFT, 0, false))
        assertTrue(controller.seeks.isEmpty())
        handler.release()
    }

    @Test
    fun rewindIsConsumedWhenControlsAreVisible() = runTest {
        val chrome = PlayerChromeState()
        val controller = FakeController(baseState(positionMs = 100_000L))
        val handler = handler(this, controller, chrome)

        assertTrue(handler.handle(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_MEDIA_REWIND, 0, false))
        advanceTimeBy(301L)

        assertEquals(listOf(70_000L), controller.seeks)
        handler.release()
    }

    @Test
    fun numberFiveSeeksToHalfwayPoint() = runTest {
        val chrome = hiddenChrome()
        val controller = FakeController(baseState(durationMs = 100_001L))
        val handler = handler(this, controller, chrome)

        assertTrue(handler.handle(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_5, 0, false))
        advanceTimeBy(301L)

        assertEquals(listOf(50_000L), controller.seeks)
        handler.release()
    }

    @Test
    fun menuOpensAndDismissesSettings() = runTest {
        val chrome = PlayerChromeState()
        val controller = FakeController(baseState())
        val handler = handler(this, controller, chrome)

        assertTrue(handler.handle(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_MENU, 0, false))
        assertTrue(chrome.popupOpen)
        assertTrue(handler.handle(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_MENU, 0, false))
        assertFalse(chrome.popupOpen)
        handler.release()
    }

    @Test
    fun heldMediaPlayPauseTogglesOnlyOnce() = runTest {
        val chrome = hiddenChrome()
        val controller = FakeController(baseState())
        val handler = handler(this, controller, chrome)

        assertTrue(handler.handle(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_MEDIA_PLAY_PAUSE, 0, true))
        assertTrue(handler.handle(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_MEDIA_PLAY_PAUSE, 1, true))
        assertTrue(handler.handle(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_MEDIA_PLAY_PAUSE, 2, true))

        assertEquals(PlaybackState.PAUSED, controller.state.playbackState)
        assertEquals(1, controller.togglePlaybackCalls)
        handler.release()
    }

    @Test
    fun heldMenuOpensOnlyOnce() = runTest {
        val chrome = PlayerChromeState()
        val controller = FakeController(baseState())
        val handler = handler(this, controller, chrome)

        assertTrue(handler.handle(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_MENU, 0, false))
        val interactionRevisionAfterOpen = chrome.interactionRevision
        assertTrue(handler.handle(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_MENU, 1, false))
        assertTrue(handler.handle(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_MENU, 2, false))

        assertTrue(chrome.popupOpen)
        assertEquals(interactionRevisionAfterOpen, chrome.interactionRevision)
        handler.release()
    }

    @Test
    fun stopSnapshotsPositionAndDurationBeforeTeardown() = runTest {
        val chrome = hiddenChrome()
        val controller = FakeController(baseState(positionMs = 123_000L, durationMs = 456_000L))
        var stopSnapshot: Pair<Long, Long>? = null
        val handler = RemoteKeyHandler(
            viewModel = controller,
            chromeState = { chrome },
            onStop = { positionMs, durationMs -> stopSnapshot = positionMs to durationMs },
            scope = this,
            clock = { 0L },
        )

        assertTrue(handler.handle(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_DPAD_RIGHT, 0, false))
        assertTrue(chrome.seekFeedback != null)
        assertTrue(handler.handle(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_MEDIA_STOP, 0, false))

        assertEquals(123_000L to 456_000L, stopSnapshot)
        assertTrue(chrome.seekFeedback == null)
        assertTrue(controller.seeks.isEmpty())
        handler.release()
    }

    @Test
    fun backIsNeverConsumed() = runTest {
        val chrome = hiddenChrome()
        val controller = FakeController(baseState())
        val handler = handler(this, controller, chrome)

        assertFalse(handler.handle(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_BACK, 0, false))
        handler.release()
    }

    @Test
    fun touchModeIgnoresNewKeysButStillHandlesLegacyMediaKey() = runTest {
        val chrome = hiddenChrome()
        val controller = FakeController(baseState(positionMs = 100_000L))
        val handler = handler(this, controller, chrome)

        assertFalse(
            handler.handle(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_MEDIA_FAST_FORWARD, 0, true),
        )
        assertFalse(handler.handle(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_MENU, 0, true))
        assertFalse(handler.handle(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_5, 0, true))
        assertFalse(handler.handle(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_DPAD_CENTER, 0, true))
        assertTrue(
            handler.handle(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_MEDIA_PLAY_PAUSE, 0, true),
        )
        assertTrue(
            handler.handle(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_MEDIA_PLAY_PAUSE, 1, true),
        )
        assertTrue(
            handler.handle(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_MEDIA_PLAY_PAUSE, 2, true),
        )

        assertEquals(PlaybackState.PAUSED, controller.state.playbackState)
        assertTrue(controller.seeks.isEmpty())
        assertFalse(chrome.popupOpen)
        handler.release()
    }

    private fun handler(
        scope: CoroutineScope,
        controller: FakeController,
        chrome: PlayerChromeState,
    ): RemoteKeyHandler = RemoteKeyHandler(
        viewModel = controller,
        chromeState = { chrome },
        onStop = { _, _ -> },
        scope = scope,
        clock = { 0L },
    ).also { it.seekStepMs = 30_000L }

    private fun hiddenChrome(): PlayerChromeState = PlayerChromeState().also {
        it.hideControls()
    }

    private fun baseState(
        positionMs: Long = 100_000L,
        durationMs: Long = 300_000L,
    ): PlayerUiState = PlayerUiState(
        screen = PlayerScreenDestination.PLAYER,
        playbackState = PlaybackState.PLAYING,
        positionMs = positionMs,
        durationMs = durationMs,
        isSeekable = true,
    )

    private class FakeController(initialState: PlayerUiState) : RemotePlaybackController {
        override var state: PlayerUiState = initialState
        var togglePlaybackCalls = 0
        val seeks = mutableListOf<Long>()

        override fun togglePlayback() {
            togglePlaybackCalls++
            state = state.copy(
                playbackState = if (state.isPlaying) PlaybackState.PAUSED else PlaybackState.PLAYING,
            )
        }

        override fun play() {
            state = state.copy(playbackState = PlaybackState.PLAYING)
        }

        override fun pause() {
            state = state.copy(playbackState = PlaybackState.PAUSED)
        }

        override fun seekTo(ms: Long) {
            seeks += ms
        }

        override fun toggleSubtitles() = Unit

        override fun toggleDiagnostics() = Unit

        override fun closePlayer() = Unit

        override fun notifyControlsInteraction() = Unit
    }
}
