package kr.dcmys.android.aribplayer

import android.content.Context
import android.content.Intent
import android.database.Cursor
import android.net.Uri
import android.os.Bundle
import android.provider.OpenableColumns
import android.view.KeyEvent
import android.view.accessibility.CaptioningManager
import android.widget.Toast
import androidx.activity.ComponentActivity
import androidx.activity.SystemBarStyle
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.viewModels
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.SideEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import androidx.core.net.toFile
import androidx.core.net.toUri
import androidx.lifecycle.lifecycleScope
import kr.dcmys.android.aribplayer.data.AppDatabase
import kr.dcmys.android.aribplayer.data.MediaEntry
import kr.dcmys.android.aribplayer.data.MediaRepository
import kr.dcmys.android.aribplayer.data.PlayerPreferences
import kr.dcmys.android.aribplayer.data.PlayerPreferencesStore
import kr.dcmys.android.aribplayer.nativeplayer.VideoMode
import kr.dcmys.android.aribplayer.ui.theme.AppTheme
import kr.dcmys.android.aribplayer.ui.player.PlayerChromeState
import kr.dcmys.android.aribplayer.ui.player.RemoteKeyHandler
import kr.dcmys.android.aribplayer.ui.player.RemotePlaybackController
import kr.dcmys.android.aribplayer.ui.player.rememberPlayerChromeState
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Job
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeoutOrNull
import java.io.File
import java.util.concurrent.atomic.AtomicLong

class MainActivity : ComponentActivity() {
    private val playerViewModel by viewModels<PlayerViewModel>()
    private val remotePlaybackController by lazy {
        object : RemotePlaybackController {
            override val state: PlayerUiState
                get() = playerViewModel.uiState.value

            override fun togglePlayback() = playerViewModel.togglePlayback()

            override fun play() {
                if (!state.isPlaying) playerViewModel.togglePlayback()
            }

            override fun pause() {
                if (state.isPlaying) playerViewModel.togglePlayback()
            }

            override fun seekTo(ms: Long) = playerViewModel.seekTo(ms)

            override fun toggleSubtitles() = playerViewModel.toggleSubtitles()

            override fun toggleDiagnostics() = playerViewModel.toggleDiagnostics()

            override fun closePlayer() = playerViewModel.closePlayer()

            override fun notifyControlsInteraction() = playerViewModel.notifyControlsInteraction()
        }
    }
    private val remoteKeys by lazy {
        RemoteKeyHandler(
            viewModel = remotePlaybackController,
            chromeState = { playerChromeState },
            onStop = { positionMs, durationMs ->
                lastPlaybackPositionMs = positionMs.coerceAtLeast(0L)
                lastPlaybackDurationMs = durationMs.coerceAtLeast(0L)
                remoteStopSnapshot = activePlayerUri?.let { uri ->
                    RemoteStopSnapshot(uri, lastPlaybackPositionMs, lastPlaybackDurationMs)
                }
                playerViewModel.closePlayer()
                finishPlaybackWithResult(
                    returnResult = activePlayerReturnResult,
                    lastPositionMs = lastPlaybackPositionMs,
                    lastDurationMs = lastPlaybackDurationMs,
                    endBy = END_BY_USER,
                )
            },
            scope = lifecycleScope,
        )
    }
    private val repository by lazy {
        MediaRepository(
            context = applicationContext,
            dao = AppDatabase.getInstance(applicationContext).mediaEntryDao(),
            prefs = PlayerPreferencesStore(applicationContext),
        )
    }

    private var incomingOpenRequest by mutableStateOf<IncomingOpenRequest?>(null)
    private var finishInFlight by mutableStateOf(false)
    @Volatile private var playerRouteActive = false
    @Volatile private var playerChromeState: PlayerChromeState? = null
    @Volatile private var activePlayerUri: String? = null
    @Volatile private var activePlayerReturnResult = false
    @Volatile private var remoteStopSnapshot: RemoteStopSnapshot? = null
    private var lastPlaybackPositionMs = 0L
    private var lastPlaybackDurationMs = 0L

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val initialRequest = intent.toIncomingOpenRequest()
        activePlayerReturnResult = initialRequest?.returnResult == true
        incomingOpenRequest = initialRequest
        if (initialRequest == null) finishWithMessage(R.string.player_invalid_open_request)
        enableEdgeToEdge(
            statusBarStyle = SystemBarStyle.dark(android.graphics.Color.TRANSPARENT),
            navigationBarStyle = SystemBarStyle.dark(android.graphics.Color.TRANSPARENT),
        )
        setContent {
            AribPlayerApp(
                activity = this,
                repository = repository,
                playerViewModel = playerViewModel,
                incomingOpenRequest = incomingOpenRequest,
                finishInFlight = finishInFlight,
                onIncomingOpenRequestConsumed = { incomingOpenRequest = null },
                onPlayerRouteChanged = { active, uri, returnResult ->
                    playerRouteActive = active
                    activePlayerUri = uri
                    if (active) activePlayerReturnResult = returnResult
                },
                onChromeStateChanged = { playerChromeState = it },
            )
        }
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        val request = intent.toIncomingOpenRequest()
        remoteStopSnapshot = null
        activePlayerReturnResult = request?.returnResult == true
        incomingOpenRequest = request
        if (request == null) finishWithMessage(R.string.player_invalid_open_request)
    }

    override fun onStop() {
        if (
            !isChangingConfigurations &&
            !finishInFlight &&
            !isFinishing &&
            hasActivePlayerSession()
        ) {
            // persistActivePosition snapshots uiState before closePlayer resets it.
            persistActivePosition()
            playerViewModel.closePlayer()
            finishPlayback()
        }
        super.onStop()
    }

    private fun hasActivePlayerSession(): Boolean {
        val state = playerViewModel.uiState.value
        return playerRouteActive ||
            state.screen == PlayerScreenDestination.PLAYER ||
            state.playbackState.isActiveOrOpening()
    }

    override fun dispatchKeyEvent(event: KeyEvent): Boolean =
        handleRemoteKeyEvent(event) || super.dispatchKeyEvent(event)

    internal fun handleRemoteKeyEvent(event: KeyEvent): Boolean {
        val state = playerViewModel.uiState.value
        return playerChromeState != null &&
            state.screen == PlayerScreenDestination.PLAYER &&
            state.playbackState != PlaybackState.ERROR &&
            remoteKeys.onKeyEvent(event, window.decorView.isInTouchMode)
    }

    fun finishWithMessage(messageRes: Int) {
        if (finishInFlight) return
        finishInFlight = true
        Toast.makeText(this, getString(messageRes), Toast.LENGTH_SHORT).show()
        lifecycleScope.launch {
            delay(FINISH_MESSAGE_DELAY_MS)
            finish()
        }
    }

    fun finishPlayback() {
        finishPlaybackWithResult(
            returnResult = activePlayerReturnResult,
            lastPositionMs = lastPlaybackPositionMs,
            lastDurationMs = lastPlaybackDurationMs,
            endBy = END_BY_USER,
        )
    }

    fun finishPlaybackWithResult(
        returnResult: Boolean,
        lastPositionMs: Long,
        lastDurationMs: Long,
        endBy: String,
    ) {
        if (finishInFlight) return
        finishInFlight = true
        if (returnResult) {
            setResult(
                RESULT_OK,
                Intent()
                    .putExtra("position", lastPositionMs.coerceAtLeast(0L))
                    .putExtra("duration", lastDurationMs.coerceAtLeast(0L))
                    .putExtra("end_by", endBy),
            )
        }
        finish()
    }

    internal fun syncRemoteSeekStepMs(seekStepMs: Long) {
        remoteKeys.seekStepMs = seekStepMs
    }

    internal fun remoteStopSnapshotFor(uri: String): RemoteStopSnapshot? =
        remoteStopSnapshot?.takeIf { it.uriString == uri }

    override fun onDestroy() {
        remoteKeys.release()
        super.onDestroy()
    }

    private fun persistActivePosition() {
        val state = playerViewModel.uiState.value
        if (state.screen != PlayerScreenDestination.PLAYER) return
        persistActivePosition(state.positionMs, state.durationMs)
    }

    private fun persistActivePosition(positionMs: Long, durationMs: Long) {
        val safePositionMs = positionMs.coerceAtLeast(0L)
        val safeDurationMs = durationMs.coerceAtLeast(0L)
        lastPlaybackPositionMs = safePositionMs
        lastPlaybackDurationMs = safeDurationMs
        val uri = activePlayerUri ?: return
        lifecycleScope.launch {
            repository.updatePosition(
                uri = uri,
                positionMs = safePositionMs,
                durationMs = safeDurationMs.takeIf { it > 0L },
                lastOpenedEpochMs = System.currentTimeMillis(),
            )
        }
    }
}

@Composable
private fun AribPlayerApp(
    activity: MainActivity,
    repository: MediaRepository,
    playerViewModel: PlayerViewModel,
    incomingOpenRequest: IncomingOpenRequest?,
    finishInFlight: Boolean,
    onIncomingOpenRequestConsumed: () -> Unit,
    onPlayerRouteChanged: (Boolean, String?, Boolean) -> Unit,
    onChromeStateChanged: (PlayerChromeState?) -> Unit,
) {
    var preferences by remember { mutableStateOf<PlayerPreferences?>(null) }
    val scope = rememberCoroutineScope()
    var session by remember { mutableStateOf<PlayerSession?>(null) }
    var requestJob by remember { mutableStateOf<Job?>(null) }
    var latestRequestGeneration by remember { mutableStateOf(-1L) }
    var openingRequestGeneration by remember { mutableStateOf<Long?>(null) }

    LaunchedEffect(repository) {
        repository.preferences.collect { preferences = it }
    }

    SideEffect {
        onPlayerRouteChanged(
            session != null,
            session?.uriString,
            session?.returnResult == true,
        )
    }

    LaunchedEffect(incomingOpenRequest, preferences) {
        val request = incomingOpenRequest ?: return@LaunchedEffect
        val currentPreferences = preferences ?: return@LaunchedEffect
        latestRequestGeneration = request.generation
        // Keep an explicit opening marker before consuming the request. Consuming it schedules a
        // recomposition before the coroutine below is assigned, so requestJob alone has a gap.
        openingRequestGeneration = request.generation
        onIncomingOpenRequestConsumed()
        requestJob?.cancel()
        requestJob = scope.launch {
            try {
                session = null
                playerViewModel.closePlayer()
                takePersistableReadPermission(activity, request.uri, request.flags)
                if (!isUriReadable(activity, request.uri)) {
                    activity.finishWithMessage(R.string.player_unreadable_open_request)
                    return@launch
                }

                val metadata = activity.queryMediaMetadata(request.uri)
                val existing = repository.get(request.uri.toString())
                val entry = existing?.copy(
                    displayName = metadata.displayName,
                    sizeBytes = metadata.sizeBytes ?: existing.sizeBytes,
                    lastOpenedEpochMs = System.currentTimeMillis(),
                    accessible = true,
                ) ?: MediaEntry(
                    uriString = request.uri.toString(),
                    displayName = metadata.displayName,
                    sizeBytes = metadata.sizeBytes,
                    lastOpenedEpochMs = System.currentTimeMillis(),
                    accessible = true,
                )
                repository.upsert(entry)
                if (request.fromStart) {
                    repository.updatePosition(
                        uri = entry.uriString,
                        positionMs = 0L,
                        durationMs = entry.durationMs,
                        lastOpenedEpochMs = System.currentTimeMillis(),
                    )
                }
                if (latestRequestGeneration != request.generation) return@launch
                val hasExplicitStart = request.positionMs != null || request.fromStart
                val needsResumeChoice = !hasExplicitStart &&
                    entry.resumePositionMs > RESUME_DIALOG_THRESHOLD_MS
                val resumePositionMs = when {
                    request.fromStart -> 0L
                    request.positionMs != null -> request.positionMs.coerceAtLeast(0L)
                    needsResumeChoice -> entry.resumePositionMs
                    else -> 0L
                }
                session = PlayerSession(
                    generation = request.generation,
                    uriString = entry.uriString,
                    displayName = entry.displayName,
                    resumePositionMs = resumePositionMs,
                    openAllowed = !needsResumeChoice,
                    selectedVideoMode = when (entry.videoMode) {
                        UNSET_VIDEO_MODE, VideoMode.AUTO -> currentPreferences.defaultVideoMode
                        else -> VideoMode.normalizeStored(entry.videoMode)
                    },
                    audioTrackKey = entry.audioTrackKey,
                    subtitlesEnabled = entry.subtitlesEnabled,
                    returnResult = request.returnResult,
                )
            } catch (error: CancellationException) {
                throw error
            } catch (_: Exception) {
                activity.finishWithMessage(R.string.player_unreadable_open_request)
            } finally {
                if (openingRequestGeneration == request.generation) {
                    openingRequestGeneration = null
                }
            }
        }
    }

    val activePlayerState by playerViewModel.uiState.collectAsState()

    AppTheme {
        Surface(modifier = Modifier.fillMaxSize(), color = Color.Black) {
            val currentSession = session
            val currentPreferences = preferences
            val requestPending = incomingOpenRequest != null ||
                openingRequestGeneration != null ||
                requestJob?.isActive == true
            val playerSessionActiveOrOpening = currentSession != null ||
                activePlayerState.screen == PlayerScreenDestination.PLAYER ||
                activePlayerState.playbackState.isActiveOrOpening()
            when {
                currentSession != null && currentPreferences != null -> {
                    PlayerDestination(
                        activity = activity,
                        session = currentSession,
                        repository = repository,
                        playerViewModel = playerViewModel,
                        preferences = currentPreferences,
                        onPlaybackClosed = { lastPositionMs, lastDurationMs, endBy ->
                            session = null
                            activity.finishPlaybackWithResult(
                                returnResult = currentSession.returnResult,
                                lastPositionMs = lastPositionMs,
                                lastDurationMs = lastDurationMs,
                                endBy = endBy,
                            )
                        },
                        onChromeStateChanged = onChromeStateChanged,
                        onSetDefaultVideoMode = { mode ->
                            scope.launch { repository.setDefaultVideoMode(mode) }
                        },
                        onSetSeekStepMs = { value ->
                            scope.launch { repository.setSeekStepMs(value) }
                        },
                        onSetDiagnosticsEnabled = { enabled ->
                            scope.launch { repository.setDiagnosticsEnabled(enabled) }
                            if (playerViewModel.uiState.value.diagnosticsVisible != enabled) {
                                playerViewModel.toggleDiagnostics()
                            }
                        },
                    )

                    if (!currentSession.openAllowed) {
                        ResumePlaybackDialog(
                            displayName = currentSession.displayName,
                            positionMs = currentSession.resumePositionMs,
                            onResume = {
                                session = currentSession.copy(openAllowed = true)
                            },
                            onStartOver = {
                                session = currentSession.copy(
                                    openAllowed = true,
                                    resumePositionMs = 0L,
                                )
                            },
                        )
                    }
                }
                playerSessionActiveOrOpening && currentPreferences != null -> {
                    // Defensive continuity: an active/opening native session must never reveal fallback UI.
                    val fallbackChromeState = rememberPlayerChromeState()
                    SideEffect {
                        onChromeStateChanged(fallbackChromeState)
                        activity.syncRemoteSeekStepMs(currentPreferences.seekStepMs)
                    }
                    DisposableEffect(fallbackChromeState) {
                        onDispose { onChromeStateChanged(null) }
                    }
                    PlayerScreen(
                        viewModel = playerViewModel,
                        chromeState = fallbackChromeState,
                        seekStepMs = currentPreferences.seekStepMs,
                        controlsTimeoutMs = currentPreferences.controlsTimeoutMs,
                        preferences = currentPreferences,
                        onSetDefaultVideoMode = { mode ->
                            scope.launch { repository.setDefaultVideoMode(mode) }
                        },
                        onSetSeekStepMs = { value ->
                            scope.launch { repository.setSeekStepMs(value) }
                        },
                        onSetDiagnosticsEnabled = { enabled ->
                            scope.launch { repository.setDiagnosticsEnabled(enabled) }
                            if (playerViewModel.uiState.value.diagnosticsVisible != enabled) {
                                playerViewModel.toggleDiagnostics()
                            }
                        },
                        onClosePlayer = { activity.finishPlayback() },
                        onRemoteKeyEvent = activity::handleRemoteKeyEvent,
                    )
                }
                finishInFlight || requestPending || currentPreferences == null ||
                    playerSessionActiveOrOpening -> {
                    Box(Modifier.fillMaxSize().background(Color.Black))
                }
                else -> OpenFromFileManagerFallback()
            }
        }
    }
}

@Composable
private fun OpenFromFileManagerFallback() {
    Box(
        modifier = Modifier.fillMaxSize().background(Color.Black).padding(24.dp),
        contentAlignment = Alignment.Center,
    ) {
        Text(text = stringResource(R.string.player_open_from_file_manager), color = Color.White)
    }
}

@Composable
private fun ResumePlaybackDialog(
    displayName: String,
    positionMs: Long,
    onResume: () -> Unit,
    onStartOver: () -> Unit,
) {
    AlertDialog(
        onDismissRequest = onStartOver,
        title = { Text(displayName) },
        text = {
            Text(stringResource(R.string.player_resume_prompt, positionMs.formatDialogTime()))
        },
        confirmButton = {
            TextButton(onClick = onResume) {
                Text(stringResource(R.string.player_resume))
            }
        },
        dismissButton = {
            TextButton(onClick = onStartOver) {
                Text(stringResource(R.string.player_start_over))
            }
        },
    )
}

@Composable
private fun PlayerDestination(
    activity: MainActivity,
    session: PlayerSession,
    repository: MediaRepository,
    playerViewModel: PlayerViewModel,
    preferences: PlayerPreferences,
    onPlaybackClosed: (Long, Long, String) -> Unit,
    onChromeStateChanged: (PlayerChromeState?) -> Unit,
    onSetDefaultVideoMode: (Int) -> Unit,
    onSetSeekStepMs: (Long) -> Unit,
    onSetDiagnosticsEnabled: (Boolean) -> Unit,
) {
    val state by playerViewModel.uiState.collectAsState()
    val chromeState = rememberPlayerChromeState()
    SideEffect {
        onChromeStateChanged(chromeState)
        activity.syncRemoteSeekStepMs(preferences.seekStepMs)
    }
    DisposableEffect(chromeState) {
        onDispose { onChromeStateChanged(null) }
    }
    var openIssued by remember(session.generation) { mutableStateOf(false) }
    var playbackStarted by remember(session.generation) { mutableStateOf(false) }
    var audioPreferenceReady by remember(session.generation) { mutableStateOf(false) }
    val lastPlayback = remember(session.generation) { PlaybackSnapshot() }
    val captioningManager = remember(activity) {
        activity.getSystemService(Context.CAPTIONING_SERVICE) as CaptioningManager
    }

    DisposableEffect(captioningManager, session.generation) {
        fun applyPresentationStyle(style: CaptioningManager.CaptionStyle) {
            val presentation = style.toAribPresentationPreferences()
            playerViewModel.updateSystemCaptionStyle(
                ignoreBackground = presentation.ignoreBackground,
                forceOutlineText = presentation.forceOutlineText,
            )
        }

        // ARIB text color is semantic (speaker/effect/emphasis), and typeface is part of the
        // broadcaster-authored presentation, so neither is overridden. fontScale is also masked:
        // independently resizing glyphs breaks the fixed ARIB layout grid and ruby/furigana
        // placement. Only whole-canvas scaling is allowed. Presentational transparent
        // background/window preferences map to ignore_background; outline/drop-shadow edge
        // requests map to force_outline_text. libaribcaption exposes no replacement colors.
        val listener = object : CaptioningManager.CaptioningChangeListener() {
            override fun onEnabledChanged(enabled: Boolean) {
                // isEnabled is a default resolved once when opening; a per-file value and the
                // in-player CC toggle remain authoritative for the active session.
            }

            override fun onUserStyleChanged(userStyle: CaptioningManager.CaptionStyle) {
                applyPresentationStyle(userStyle)
            }

            override fun onFontScaleChanged(fontScale: Float) {
                // Deliberately masked; see fixed-plane/ruby rationale above.
            }
        }
        applyPresentationStyle(captioningManager.userStyle)
        captioningManager.addCaptioningChangeListener(listener)
        onDispose { captioningManager.removeCaptioningChangeListener(listener) }
    }

    if (openIssued && state.screen == PlayerScreenDestination.PLAYER) {
        lastPlayback.positionMs = state.positionMs
        if (state.durationMs > 0L) lastPlayback.durationMs = state.durationMs
        if (state.filterBackend != null) {
            lastPlayback.selectedVideoMode = state.selectedVideoMode
        }
        lastPlayback.selectedTrackKey = state.selectedTrackKey
        lastPlayback.hasSubtitles = state.hasSubtitles
        lastPlayback.subtitlesEnabled = state.subtitlesEnabled
    }

    LaunchedEffect(session.generation, session.openAllowed) {
        if (!session.openAllowed || openIssued) return@LaunchedEffect
        openIssued = true
        val captionPresentation =
            captioningManager.userStyle.toAribPresentationPreferences()
        playerViewModel.prepareSystemCaptionPreferences(
            defaultSubtitlesEnabled = session.subtitlesEnabled ?: captioningManager.isEnabled,
            ignoreBackground = captionPresentation.ignoreBackground,
            forceOutlineText = captionPresentation.forceOutlineText,
        )
        if (!playerViewModel.openDocument(
                activity,
                session.uriString.toUri(),
                startPositionMs = session.resumePositionMs,
                videoMode = session.selectedVideoMode,
            )
        ) {
            activity.finishWithMessage(R.string.player_unreadable_open_request)
            return@LaunchedEffect
        }
        val preferredAudioTrackKey = session.audioTrackKey
        if (preferredAudioTrackKey != null) {
            launch {
                val matchingState = withTimeoutOrNull(AUDIO_TRACK_RESTORE_TIMEOUT_MS) {
                    playerViewModel.uiState.first { uiState ->
                        uiState.tracks.any { it.key == preferredAudioTrackKey }
                    }
                }
                if (matchingState != null) {
                    playerViewModel.selectAudioTrack(preferredAudioTrackKey)
                }
                audioPreferenceReady = true
            }
        } else {
            audioPreferenceReady = true
        }
        session.subtitlesEnabled?.let { preferred ->
            launch {
                playerViewModel.uiState.first { it.hasSubtitles }
                playerViewModel.setSubtitlesEnabled(preferred)
            }
        }
        if (preferences.diagnosticsEnabled && !playerViewModel.uiState.value.diagnosticsVisible) {
            playerViewModel.toggleDiagnostics()
        }
    }

    LaunchedEffect(openIssued, state.playbackState) {
        if (
            openIssued &&
            !playbackStarted &&
            state.playbackState == PlaybackState.READY
        ) {
            playbackStarted = true
            playerViewModel.togglePlayback()
        }
    }

    LaunchedEffect(session.generation, openIssued, session.returnResult, state.playbackState) {
        if (!openIssued || !session.returnResult || state.playbackState != PlaybackState.ENDED) {
            return@LaunchedEffect
        }
        delay(FINISH_MESSAGE_DELAY_MS)
        if (isActive && playerViewModel.uiState.value.playbackState == PlaybackState.ENDED) {
            onPlaybackClosed(
                lastPlayback.positionMs,
                lastPlayback.durationMs ?: 0L,
                END_BY_PLAYBACK_COMPLETION,
            )
        }
    }

    LaunchedEffect(session.uriString, openIssued, audioPreferenceReady, state.selectedTrackKey) {
        val selectedTrackKey = state.selectedTrackKey
        if (openIssued && audioPreferenceReady && selectedTrackKey != null) {
            repository.updateAudioTrack(session.uriString, selectedTrackKey)
        }
    }

    LaunchedEffect(session.uriString, openIssued, state.selectedVideoMode, state.filterBackend) {
        if (openIssued && state.filterBackend != null) {
            repository.updateVideoMode(session.uriString, state.selectedVideoMode)
        }
    }

    LaunchedEffect(session.uriString, openIssued, state.hasSubtitles, state.subtitlesEnabled) {
        if (openIssued && state.hasSubtitles) {
            repository.updateSubtitlesEnabled(session.uriString, state.subtitlesEnabled)
        }
    }

    LaunchedEffect(session.generation, openIssued) {
        if (!openIssued) return@LaunchedEffect
        try {
            while (isActive) {
                delay(POSITION_SAVE_INTERVAL_MS)
                repository.updatePosition(
                    uri = session.uriString,
                    positionMs = lastPlayback.positionMs,
                    durationMs = lastPlayback.durationMs,
                    lastOpenedEpochMs = System.currentTimeMillis(),
                )
            }
        } finally {
            withContext(NonCancellable) {
                // STOP snapshots before closePlayer resets uiState; persist it even on teardown.
                val stopSnapshot = activity.remoteStopSnapshotFor(session.uriString)
                repository.updatePosition(
                    uri = session.uriString,
                    positionMs = stopSnapshot?.positionMs ?: lastPlayback.positionMs,
                    durationMs = if (stopSnapshot != null) {
                        stopSnapshot.durationMs.takeIf { it > 0L }
                    } else {
                        lastPlayback.durationMs
                    },
                    lastOpenedEpochMs = System.currentTimeMillis(),
                )
                lastPlayback.selectedTrackKey?.let { selectedTrackKey ->
                    repository.updateAudioTrack(session.uriString, selectedTrackKey)
                }
                lastPlayback.selectedVideoMode?.let { selectedVideoMode ->
                    repository.updateVideoMode(session.uriString, selectedVideoMode)
                }
                if (lastPlayback.hasSubtitles) {
                    repository.updateSubtitlesEnabled(
                        session.uriString,
                        lastPlayback.subtitlesEnabled,
                    )
                }
            }
        }
    }

    PlayerScreen(
        viewModel = playerViewModel,
        chromeState = chromeState,
        seekStepMs = preferences.seekStepMs,
        controlsTimeoutMs = preferences.controlsTimeoutMs,
        preferences = preferences,
        onSetDefaultVideoMode = onSetDefaultVideoMode,
        onSetSeekStepMs = onSetSeekStepMs,
        onSetDiagnosticsEnabled = onSetDiagnosticsEnabled,
        onClosePlayer = {
            onPlaybackClosed(
                lastPlayback.positionMs,
                lastPlayback.durationMs ?: 0L,
                END_BY_USER,
            )
        },
        onRemoteKeyEvent = activity::handleRemoteKeyEvent,
    )
}

private suspend fun isUriReadable(context: Context, uri: Uri): Boolean = withContext(kotlinx.coroutines.Dispatchers.IO) {
    when (uri.scheme?.lowercase()) {
        "file" -> runCatching { uri.toFile().isFile && uri.toFile().canRead() }.getOrDefault(false)
        "content" -> runCatching {
            context.contentResolver.openFileDescriptor(uri, "r")?.use { true } ?: false
        }.getOrDefault(false)
        null -> File(uri.toString()).let { it.isFile && it.canRead() }
        else -> false
    }
}

private fun takePersistableReadPermission(context: Context, uri: Uri, resultFlags: Int) {
    if (uri.scheme != "content") return
    val readFlag = resultFlags and Intent.FLAG_GRANT_READ_URI_PERMISSION
    if (readFlag == 0) return
    try {
        context.contentResolver.takePersistableUriPermission(uri, readFlag)
    } catch (_: SecurityException) {
        // ACTION_VIEW and some providers grant only transient access.
    } catch (_: IllegalArgumentException) {
        // The provider does not offer persistable grants.
    }
}

private fun Context.queryMediaMetadata(uri: Uri): MediaMetadata {
    if (uri.scheme == "file") {
        val file = runCatching { uri.toFile() }.getOrNull()
        return MediaMetadata(
            displayName = file?.name?.takeIf { it.isNotBlank() }
                ?: uri.lastPathSegment
                ?: getString(R.string.player_selected_program),
            sizeBytes = file?.takeIf { it.isFile }?.length(),
        )
    }

    val fallbackName = uri.lastPathSegment?.substringAfterLast('/')
        ?: getString(R.string.player_selected_program)
    return runCatching {
        contentResolver.query(
            uri,
            arrayOf(OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE),
            null,
            null,
            null,
        )?.use { cursor: Cursor ->
            if (!cursor.moveToFirst()) return@use null
            val nameIndex = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME)
            val sizeIndex = cursor.getColumnIndex(OpenableColumns.SIZE)
            MediaMetadata(
                displayName = if (nameIndex >= 0) cursor.getString(nameIndex) ?: fallbackName else fallbackName,
                sizeBytes = if (sizeIndex >= 0 && !cursor.isNull(sizeIndex)) cursor.getLong(sizeIndex) else null,
            )
        }
    }.getOrNull() ?: MediaMetadata(fallbackName, null)
}

private fun Intent?.toIncomingOpenRequest(): IncomingOpenRequest? {
    if (this?.action != Intent.ACTION_VIEW) return null
    val incomingUri = data ?: return null
    if (incomingUri.scheme != "content" && incomingUri.scheme != "file") return null
    val extras = extras
    return IncomingOpenRequest(
        uri = incomingUri,
        flags = flags,
        generation = requestIds.incrementAndGet(),
        positionMs = (extras?.get(EXTRA_POSITION) as? Number)?.toLong(),
        fromStart = extras?.getBoolean(EXTRA_FROM_START, false) == true,
        returnResult = extras?.getBoolean(EXTRA_RETURN_RESULT, false) == true,
    )
}

private fun PlaybackState.isActiveOrOpening(): Boolean = when (this) {
    PlaybackState.PREPARING,
    PlaybackState.READY,
    PlaybackState.PLAYING,
    PlaybackState.PAUSED,
    PlaybackState.ENDED -> true
    PlaybackState.IDLE,
    PlaybackState.ERROR -> false
}

private fun Long.formatDialogTime(): String {
    val totalSeconds = coerceAtLeast(0L) / 1_000L
    val hours = totalSeconds / 3_600L
    val minutes = (totalSeconds % 3_600L) / 60L
    val seconds = totalSeconds % 60L
    return if (hours > 0L) "%d:%02d:%02d".format(hours, minutes, seconds)
    else "%02d:%02d".format(minutes, seconds)
}

private data class AribPresentationPreferences(
    val ignoreBackground: Boolean,
    val forceOutlineText: Boolean,
)

private fun CaptioningManager.CaptionStyle.toAribPresentationPreferences():
    AribPresentationPreferences {
    val transparentBackground = hasBackgroundColor() &&
        android.graphics.Color.alpha(backgroundColor) == 0
    // The bitmap path has no distinct caption-window primitive, so an explicitly transparent
    // window is folded into the same background-suppression option.
    val transparentWindow = hasWindowColor() &&
        android.graphics.Color.alpha(windowColor) == 0
    val outlined = hasEdgeType() && (
        edgeType == CaptioningManager.CaptionStyle.EDGE_TYPE_OUTLINE ||
            edgeType == CaptioningManager.CaptionStyle.EDGE_TYPE_DROP_SHADOW
        )
    return AribPresentationPreferences(
        ignoreBackground = transparentBackground || transparentWindow,
        forceOutlineText = outlined,
    )
}

private data class IncomingOpenRequest(
    val uri: Uri,
    val flags: Int,
    val generation: Long,
    val positionMs: Long?,
    val fromStart: Boolean,
    val returnResult: Boolean,
)
private data class MediaMetadata(val displayName: String, val sizeBytes: Long?)
private data class PlayerSession(
    val generation: Long,
    val uriString: String,
    val displayName: String,
    val resumePositionMs: Long,
    val openAllowed: Boolean,
    val selectedVideoMode: Int,
    val audioTrackKey: String?,
    val subtitlesEnabled: Boolean?,
    val returnResult: Boolean,
)
internal data class RemoteStopSnapshot(
    val uriString: String,
    val positionMs: Long,
    val durationMs: Long,
)

private class PlaybackSnapshot(
    var positionMs: Long = 0L,
    var durationMs: Long? = null,
    var selectedVideoMode: Int? = null,
    var selectedTrackKey: String? = null,
    var hasSubtitles: Boolean = false,
    var subtitlesEnabled: Boolean = true,
)

private val requestIds = AtomicLong()
private const val RESUME_DIALOG_THRESHOLD_MS = 30_000L
private const val POSITION_SAVE_INTERVAL_MS = 5_000L
private const val AUDIO_TRACK_RESTORE_TIMEOUT_MS = 5_000L
private const val FINISH_MESSAGE_DELAY_MS = 1_500L
private const val UNSET_VIDEO_MODE = -1
private const val EXTRA_POSITION = "position"
private const val EXTRA_FROM_START = "from_start"
private const val EXTRA_RETURN_RESULT = "return_result"
private const val END_BY_USER = "user"
private const val END_BY_PLAYBACK_COMPLETION = "playback_completion"
