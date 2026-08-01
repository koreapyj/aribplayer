package kr.dcmys.android.aribplayer

import android.content.Context
import android.content.Intent
import android.database.Cursor
import android.net.Uri
import android.provider.OpenableColumns
import android.view.Surface
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import kr.dcmys.android.aribplayer.nativeplayer.PlayerController
import kr.dcmys.android.aribplayer.nativeplayer.PlayerError
import kr.dcmys.android.aribplayer.nativeplayer.PlayerListener
import kr.dcmys.android.aribplayer.nativeplayer.PreparedInfo
import kr.dcmys.android.aribplayer.nativeplayer.VideoMode
import kr.dcmys.android.aribplayer.nativeplayer.VideoSize
import kr.dcmys.android.aribplayer.nativeplayer.PlaybackState as NativePlaybackState
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asSharedFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import org.json.JSONObject
import java.io.File

enum class PlaybackState {
    IDLE,
    PREPARING,
    READY,
    PLAYING,
    PAUSED,
    ENDED,
    ERROR
}

data class AudioTrackUi(
    val streamIndex: Int,
    val dualMonoMode: Int,
    val codec: String,
    val lang: String?,
    val disposition: Int,
    val pmtOrder: Int,
    val role: String?,
    val dualMono: Boolean,
    val title: String?,
) {
    val key: String
        get() = "$streamIndex:$dualMonoMode"

    val displayName: String
        get() = when (dualMonoMode) {
            1 -> "Main voice"
            2 -> "Sub voice"
            else -> listOfNotNull(
                lang?.takeIf { it.isNotBlank() },
                role?.takeIf { it.isNotBlank() },
            ).joinToString(" / ").ifBlank { title?.takeIf { it.isNotBlank() } ?: "Audio #$streamIndex" }
        }
}

data class PlayerUiState(
    val screen: PlayerScreenDestination = PlayerScreenDestination.LIBRARY,
    val displayName: String? = null,
    val playbackState: PlaybackState = PlaybackState.IDLE,
    val positionMs: Long = 0L,
    val durationMs: Long = 0L,
    val isSeekable: Boolean = false,
    val videoWidth: Int = 0,
    val videoHeight: Int = 0,
    val selectedVideoMode: Int = VideoMode.AUTO,
    val effectiveVideoMode: Int = VideoMode.AUTO,
    val tracks: List<AudioTrackUi> = emptyList(),
    val selectedTrackKey: String? = null,
    val filterBackend: String? = null,
    val hasSubtitles: Boolean = false,
    val subtitleStreamCount: Int = 0,
    val subtitleFontOk: Boolean = false,
    val subtitlesEnabled: Boolean = true,
    val decoderName: String? = null,
    val decoderIsHardware: Boolean? = null,
    val statsJson: String? = null,
    val diagnosticsVisible: Boolean = false,
    val errorMessage: String? = null
) {
    val isPlaying: Boolean
        get() = playbackState == PlaybackState.PLAYING
}

enum class PlayerScreenDestination {
    LIBRARY,
    PLAYER
}

class PlayerViewModel : ViewModel() {
    private val _uiState = MutableStateFlow(PlayerUiState())
    val uiState: StateFlow<PlayerUiState> = _uiState.asStateFlow()

    private val _controlsInteraction = MutableSharedFlow<Unit>(extraBufferCapacity = 1)
    val controlsInteraction: SharedFlow<Unit> = _controlsInteraction.asSharedFlow()

    private val listener = object : PlayerListener {
        override fun onPrepared(preparedInfo: PreparedInfo) {
            val parsedTracks = parseAudioTracks(preparedInfo.trackInfo)
            updateState {
                it.copy(
                    playbackState = PlaybackState.READY,
                    durationMs = preparedInfo.durationMs.coerceAtLeast(0L),
                    isSeekable = preparedInfo.isSeekable,
                    tracks = parsedTracks.tracks,
                    selectedTrackKey = parsedTracks.selectedKey,
                    errorMessage = null
                )
            }
        }

        override fun onTracksChanged(trackInfo: String) {
            val parsedTracks = parseAudioTracks(trackInfo)
            updateState {
                it.copy(
                    tracks = parsedTracks.tracks,
                    selectedTrackKey = parsedTracks.selectedKey,
                )
            }
        }

        override fun onVideoSize(videoSize: VideoSize) {
            updateState {
                it.copy(videoWidth = videoSize.width, videoHeight = videoSize.height)
            }
        }

        override fun onState(state: Int) {
            updateState { it.copy(playbackState = state.toUiPlaybackState()) }
        }

        override fun onPositionMs(positionMs: Long) {
            updateState { it.copy(positionMs = positionMs.coerceAtLeast(0L)) }
        }

        override fun onError(error: PlayerError) {
            updateState {
                it.copy(
                    playbackState = PlaybackState.ERROR,
                    errorMessage = error.message ?: "Playback failed (code ${error.code})."
                )
            }
        }

        override fun onDecoderInfo(codecName: String, isHardware: Boolean) {
            updateState {
                it.copy(decoderName = codecName, decoderIsHardware = isHardware)
            }
        }

        override fun onFilterInfo(mode: Int, backend: String) {
            updateState {
                it.copy(effectiveVideoMode = mode, filterBackend = backend)
            }
        }

        override fun onSubtitleInfo(hasSubtitles: Boolean, streamCount: Int, fontOk: Boolean) {
            updateState {
                it.copy(
                    hasSubtitles = hasSubtitles,
                    subtitleStreamCount = streamCount.coerceAtLeast(0),
                    subtitleFontOk = fontOk
                )
            }
        }

        override fun onEndOfStream() {
            updateState { state ->
                state.copy(
                    playbackState = PlaybackState.ENDED,
                    positionMs = state.durationMs.takeIf { it > 0L } ?: state.positionMs
                )
            }
        }
    }

    private val controllerHolder = lazy(LazyThreadSafetyMode.NONE) { PlayerController(listener) }
    private val controller: PlayerController
        get() = controllerHolder.value
    // PlayerDestination composes the SurfaceView before its open effect creates the controller.
    // Keep the latest holder surface so that initial callback is not lost during that interval.
    private var attachedSurface: Surface? = null
    private var initialSubtitlesEnabled = true
    private var captionIgnoreBackground = false
    private var captionForceOutlineText = false
    private var statsPollingJob: Job? = null

    fun openDocument(context: Context, uri: Uri, startPositionMs: Long = 0L): Boolean {
        val safeStartPositionMs = startPositionMs.coerceAtLeast(0L)
        if (uri.scheme == "content") {
            try {
                context.contentResolver.takePersistableUriPermission(
                    uri,
                    Intent.FLAG_GRANT_READ_URI_PERMISSION
                )
            } catch (_: SecurityException) {
                // Some providers grant only a transient read permission.
            } catch (_: IllegalArgumentException) {
                // The provider does not offer persistable grants.
            }
        }

        val displayName = context.queryDisplayName(uri)
        updateState {
            it.copy(
                screen = PlayerScreenDestination.PLAYER,
                displayName = displayName,
                playbackState = PlaybackState.PREPARING,
                positionMs = safeStartPositionMs,
                durationMs = 0L,
                isSeekable = false,
                videoWidth = 0,
                videoHeight = 0,
                selectedVideoMode = VideoMode.AUTO,
                effectiveVideoMode = VideoMode.AUTO,
                tracks = emptyList(),
                selectedTrackKey = null,
                filterBackend = null,
                hasSubtitles = false,
                subtitleStreamCount = 0,
                subtitleFontOk = false,
                subtitlesEnabled = initialSubtitlesEnabled,
                decoderName = null,
                decoderIsHardware = null,
                statsJson = null,
                diagnosticsVisible = false,
                errorMessage = null
            )
        }

        return try {
            context.contentResolver.openFileDescriptor(uri, "r").use { descriptor ->
                checkNotNull(descriptor) { "The selected document could not be opened." }
                if (controllerHolder.isInitialized()) controller.close()
                check(controller.setCaptionStyle(
                    captionIgnoreBackground,
                    captionForceOutlineText,
                )) { "The playback engine could not apply caption preferences." }
                check(controller.open(
                    descriptor.fd,
                    fontPath = context.ensureAribFontPath(),
                    displayName = displayName,
                    startPositionMs = safeStartPositionMs,
                )) {
                    "The playback engine could not open the selected document."
                }
                controller.setVideoMode(VideoMode.AUTO)
                controller.setSubtitlesEnabled(_uiState.value.subtitlesEnabled)
                attachedSurface?.let(controller::setSurface)
                startStatsPolling()
            }
            true
        } catch (error: Exception) {
            showControllerError(error)
            false
        }
    }

    fun setSurface(surface: Surface?) {
        attachedSurface = surface
        if (!controllerHolder.isInitialized()) return
        runCatching { controller.setSurface(surface) }
            .onFailure(::showControllerError)
    }

    fun togglePlayback() {
        if (!controllerHolder.isInitialized()) return
        runCatching {
            if (_uiState.value.isPlaying) {
                controller.pause()
            } else {
                controller.play()
            }
        }.onSuccess { accepted ->
            if (accepted) {
                updateState {
                    it.copy(playbackState = if (it.isPlaying) PlaybackState.PAUSED else PlaybackState.PLAYING)
                }
            }
        }.onFailure(::showControllerError)
    }

    fun notifyControlsInteraction() {
        _controlsInteraction.tryEmit(Unit)
    }

    fun seekTo(positionMs: Long) {
        if (!controllerHolder.isInitialized()) return
        val safePositionMs = positionMs.coerceIn(0L, _uiState.value.durationMs.coerceAtLeast(0L))
        runCatching { controller.seekTo(safePositionMs) }
            .onSuccess { accepted ->
                if (accepted) updateState { it.copy(positionMs = safePositionMs) }
            }
            .onFailure(::showControllerError)
    }

    fun setVideoMode(mode: Int) {
        if (mode !in supportedVideoModes || !controllerHolder.isInitialized()) return
        runCatching { controller.setVideoMode(mode) }
            .onSuccess { accepted ->
                if (accepted) updateState { it.copy(selectedVideoMode = mode) }
            }
            .onFailure(::showControllerError)
    }

    fun selectAudioTrack(streamIndex: Int, dualMonoMode: Int) {
        selectAudioTrack("$streamIndex:$dualMonoMode")
    }

    fun selectAudioTrack(trackKey: String) {
        if (!controllerHolder.isInitialized()) return
        val track = _uiState.value.tracks.firstOrNull { it.key == trackKey } ?: return
        if (_uiState.value.selectedTrackKey == track.key) return
        runCatching { controller.selectAudioTrack(track.streamIndex, track.dualMonoMode) }
            .onSuccess { accepted ->
                if (accepted) updateState { it.copy(selectedTrackKey = track.key) }
            }
            .onFailure(::showControllerError)
    }

    fun prepareSystemCaptionPreferences(
        defaultSubtitlesEnabled: Boolean,
        ignoreBackground: Boolean,
        forceOutlineText: Boolean,
    ) {
        initialSubtitlesEnabled = defaultSubtitlesEnabled
        updateSystemCaptionStyle(ignoreBackground, forceOutlineText)
    }

    fun updateSystemCaptionStyle(ignoreBackground: Boolean, forceOutlineText: Boolean) {
        captionIgnoreBackground = ignoreBackground
        captionForceOutlineText = forceOutlineText
        if (!controllerHolder.isInitialized()) return
        runCatching { controller.setCaptionStyle(ignoreBackground, forceOutlineText) }
            .onFailure(::showControllerError)
    }

    fun toggleSubtitles() {
        setSubtitlesEnabled(!_uiState.value.subtitlesEnabled)
    }

    fun setSubtitlesEnabled(enabled: Boolean) {
        val state = _uiState.value
        if (!state.hasSubtitles || !controllerHolder.isInitialized()) return
        if (state.subtitlesEnabled == enabled) return
        runCatching { controller.setSubtitlesEnabled(enabled) }
            .onSuccess { accepted ->
                if (accepted) updateState { it.copy(subtitlesEnabled = enabled) }
            }
            .onFailure(::showControllerError)
    }

    fun toggleDiagnostics() {
        updateState { it.copy(diagnosticsVisible = !it.diagnosticsVisible) }
    }

    fun closePlayer() {
        statsPollingJob?.cancel()
        statsPollingJob = null
        if (controllerHolder.isInitialized()) runCatching { controller.close() }
        _uiState.value = PlayerUiState()
    }

    override fun onCleared() {
        statsPollingJob?.cancel()
        if (controllerHolder.isInitialized()) controller.release()
        super.onCleared()
    }

    private fun startStatsPolling() {
        statsPollingJob?.cancel()
        statsPollingJob = viewModelScope.launch {
            while (isActive && _uiState.value.screen == PlayerScreenDestination.PLAYER) {
                val statsJson = if (controllerHolder.isInitialized()) {
                    runCatching { controller.getStats() }.getOrNull()
                } else {
                    null
                }
                if (statsJson != null && _uiState.value.screen == PlayerScreenDestination.PLAYER) {
                    updateState { it.copy(statsJson = statsJson) }
                }
                delay(STATS_POLL_INTERVAL_MS)
            }
        }
    }

    private fun showControllerError(error: Throwable) {
        updateState {
            it.copy(
                playbackState = PlaybackState.ERROR,
                errorMessage = error.message ?: "Unable to open the selected document."
            )
        }
    }

    private inline fun updateState(transform: (PlayerUiState) -> PlayerUiState) {
        _uiState.value = transform(_uiState.value)
    }

    private companion object {
        const val STATS_POLL_INTERVAL_MS = 500L
        val supportedVideoModes = setOf(
            VideoMode.OFF,
            VideoMode.AUTO,
            VideoMode.IVTC,
            VideoMode.DEINTERLACE,
        )
    }
}

private data class ParsedAudioTracks(
    val tracks: List<AudioTrackUi>,
    val selectedKey: String?,
)

private fun parseAudioTracks(trackInfo: String): ParsedAudioTracks = runCatching {
    val root = JSONObject(trackInfo)
    val selectedProgramId = root.optInt("selectedProgramId", -1)
    val streams = root.optJSONArray("streams")
    val tracks = buildList {
        if (streams != null) {
            for (index in 0 until streams.length()) {
                val stream = streams.optJSONObject(index) ?: continue
                if (stream.optString("type") != "audio") continue
                if (selectedProgramId >= 0 &&
                    stream.optInt("programId", -1) != selectedProgramId
                ) continue
                val streamIndex = stream.optInt("streamIndex", stream.optInt("index", -1))
                if (streamIndex < 0) continue
                add(
                    AudioTrackUi(
                        streamIndex = streamIndex,
                        dualMonoMode = stream.optInt("dualMonoMode", -1),
                        codec = stream.optString("codec", "unknown"),
                        lang = stream.optNullableString("lang")
                            ?: stream.optNullableString("language"),
                        disposition = stream.optInt("disposition", 0),
                        pmtOrder = stream.optInt("pmtOrder", Int.MAX_VALUE),
                        role = stream.optNullableString("role"),
                        dualMono = stream.optBoolean("dualMono", false),
                        title = stream.optNullableString("title"),
                    ),
                )
            }
        }
    }.sortedWith(compareBy<AudioTrackUi> { it.pmtOrder }.thenBy { it.dualMonoMode })
    val selectedKey = streams?.let {
        (0 until it.length()).firstNotNullOfOrNull { index ->
            val stream = it.optJSONObject(index) ?: return@firstNotNullOfOrNull null
            if (stream.optString("type") != "audio" ||
                !stream.optBoolean("selected", false)
            ) return@firstNotNullOfOrNull null
            val streamIndex = stream.optInt("streamIndex", stream.optInt("index", -1))
            if (streamIndex < 0) null
            else "$streamIndex:${stream.optInt("dualMonoMode", -1)}"
        }
    }
    ParsedAudioTracks(tracks = tracks, selectedKey = selectedKey ?: tracks.firstOrNull()?.key)
}.getOrElse { ParsedAudioTracks(emptyList(), null) }

private fun JSONObject.optNullableString(name: String): String? =
    takeIf { has(name) && !isNull(name) }
        ?.optString(name)
        ?.takeIf { it.isNotBlank() }

private fun Context.ensureAribFontPath(): String? = runCatching {
    val target = File(filesDir, "fonts/WLCMARU2004ARIBU.TTF")
    if (!target.isFile) {
        target.parentFile?.mkdirs()
        assets.open("fonts/WLCMARU2004ARIBU.TTF").use { input ->
            target.outputStream().use { output -> input.copyTo(output) }
        }
    }
    target.absolutePath
}.getOrNull()

private fun Context.queryDisplayName(uri: Uri): String {
    val fallback = uri.lastPathSegment?.substringAfterLast('/') ?: "Selected program"
    return contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)
        ?.use { cursor: Cursor ->
            val nameIndex = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME)
            if (nameIndex >= 0 && cursor.moveToFirst()) cursor.getString(nameIndex) else null
        }
        ?: fallback
}

private fun Int.toUiPlaybackState(): PlaybackState = when (this) {
    NativePlaybackState.OPENING -> PlaybackState.PREPARING
    NativePlaybackState.PREPARED -> PlaybackState.READY
    NativePlaybackState.PLAYING -> PlaybackState.PLAYING
    NativePlaybackState.PAUSED -> PlaybackState.PAUSED
    NativePlaybackState.ENDED -> PlaybackState.ENDED
    NativePlaybackState.ERROR -> PlaybackState.ERROR
    else -> PlaybackState.IDLE
}
