package kr.dcmys.android.aribplayer

import android.view.SurfaceHolder
import android.view.SurfaceView
import androidx.activity.compose.BackHandler
import androidx.activity.compose.LocalActivity
import androidx.compose.foundation.background
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.viewinterop.AndroidView
import androidx.lifecycle.viewmodel.compose.viewModel
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import kr.dcmys.android.aribplayer.data.PlayerPreferences
import kr.dcmys.android.aribplayer.ui.player.PlayerChrome
import kr.dcmys.android.aribplayer.ui.player.PlayerOverlays
import kr.dcmys.android.aribplayer.ui.player.rememberPlayerChromeState

@Composable
fun PlayerScreen(
    viewModel: PlayerViewModel = viewModel(),
    seekStepMs: Long,
    controlsTimeoutMs: Long,
    preferences: PlayerPreferences,
    onSetDefaultVideoMode: (Int) -> Unit,
    onSetSeekStepMs: (Long) -> Unit,
    onSetDiagnosticsEnabled: (Boolean) -> Unit,
    onClosePlayer: () -> Unit,
) {
    val state by viewModel.uiState.collectAsState()
    val chrome = rememberPlayerChromeState()

    PlayerSystemBars(controlsVisible = chrome.controlsVisible)

    BackHandler {
        if (!chrome.handleBack()) {
            viewModel.closePlayer()
            onClosePlayer()
        }
    }
    KeepScreenOnWhilePlaying(state.isPlaying)

    Box(modifier = Modifier.fillMaxSize().background(Color.Black)) {
        AndroidView(
            factory = { context ->
                SurfaceView(context).also { surfaceView ->
                    surfaceView.holder.addCallback(object : SurfaceHolder.Callback {
                        override fun surfaceCreated(holder: SurfaceHolder) {
                            viewModel.setSurface(holder.surface)
                        }

                        override fun surfaceChanged(
                            holder: SurfaceHolder,
                            format: Int,
                            width: Int,
                            height: Int
                        ) {
                            viewModel.setSurface(holder.surface)
                        }

                        override fun surfaceDestroyed(holder: SurfaceHolder) {
                            viewModel.setSurface(null)
                        }
                    })
                }
            },
            update = { surfaceView ->
                if (surfaceView.holder.surface.isValid) {
                    viewModel.setSurface(surfaceView.holder.surface)
                }
            },
            modifier = Modifier.fillMaxSize()
        )

        Box(
            modifier = Modifier
                .fillMaxSize()
                .pointerInput(chrome) {
                    detectTapGestures { chrome.onVideoTap() }
                }
        )

        PlayerChrome(
            state = state,
            chromeState = chrome,
            preferences = preferences,
            seekStepMs = seekStepMs,
            controlsTimeoutMs = controlsTimeoutMs,
            interactionEvents = viewModel.controlsInteraction,
            onTogglePlayback = viewModel::togglePlayback,
            onSeekTo = viewModel::seekTo,
            onSetVideoMode = viewModel::setVideoMode,
            onSelectAudioTrack = viewModel::selectAudioTrack,
            onToggleSubtitles = viewModel::toggleSubtitles,
            onToggleDiagnostics = viewModel::toggleDiagnostics,
            onSetDefaultVideoMode = onSetDefaultVideoMode,
            onSetSeekStepMs = onSetSeekStepMs,
            onSetDiagnosticsEnabled = onSetDiagnosticsEnabled,
        )

        PlayerOverlays(state = state)
    }
}

@Composable
private fun PlayerSystemBars(controlsVisible: Boolean) {
    val view = LocalView.current
    val window = LocalActivity.current?.window
    val insetsController = remember(window, view) {
        window?.let { WindowInsetsControllerCompat(it, view) }
    }

    LaunchedEffect(controlsVisible, insetsController) {
        val controller = insetsController ?: return@LaunchedEffect
        if (controlsVisible) {
            controller.show(WindowInsetsCompat.Type.systemBars())
        } else {
            controller.systemBarsBehavior =
                WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
            controller.hide(WindowInsetsCompat.Type.systemBars())
        }
    }

    DisposableEffect(insetsController) {
        onDispose {
            insetsController?.show(WindowInsetsCompat.Type.systemBars())
        }
    }
}

@Composable
private fun KeepScreenOnWhilePlaying(isPlaying: Boolean) {
    val view = LocalView.current
    DisposableEffect(isPlaying) {
        view.keepScreenOn = isPlaying
        onDispose { view.keepScreenOn = false }
    }
}
