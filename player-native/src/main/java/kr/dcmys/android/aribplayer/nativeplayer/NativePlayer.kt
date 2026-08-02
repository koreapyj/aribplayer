package kr.dcmys.android.aribplayer.nativeplayer

import android.os.Handler
import android.os.Looper
import android.view.Surface
import java.util.concurrent.atomic.AtomicReference

/** Numeric playback states reported through [PlayerListener.onPlaybackStateChanged]. */
object PlaybackState {
    const val IDLE = 0
    const val OPENING = 1
    const val PREPARED = 2
    const val PLAYING = 3
    const val PAUSED = 4
    const val ENDED = 5
    const val CLOSED = 6
    const val ERROR = 7
}

/** Video filter modes; AUTO is an app-default request resolved once from stream metadata at open. */
object VideoMode {
    const val OFF = 0
    const val AUTO = 1
    const val IVTC = 2
    const val DEINTERLACE = 3

    /** Keeps the app preference in the supported default-mode set. */
    fun normalizeDefault(mode: Int): Int = when (mode) {
        AUTO, OFF, IVTC, DEINTERLACE -> mode
        else -> AUTO
    }

    /** Converts a stored per-file value to a concrete session mode. */
    fun normalizeStored(mode: Int): Int = when (mode) {
        OFF, IVTC, DEINTERLACE -> mode
        else -> DEINTERLACE
    }
}

/** Information delivered once native demuxing has prepared an opened source. */
data class PreparedInfo(
    val durationMs: Long,
    val trackInfo: String,
    val isSeekable: Boolean,
)

/** Display dimensions and sample aspect ratio reported by the native video decoder. */
data class VideoSize(
    val width: Int,
    val height: Int,
    val pixelAspectRatioNumerator: Int,
    val pixelAspectRatioDenominator: Int,
)

/** An error raised asynchronously by the native player. */
data class PlayerError(
    val code: Int,
    val message: String?,
)

/** Receives asynchronous player events on the Android main thread. */
interface PlayerListener {
    fun onPrepared(preparedInfo: PreparedInfo) = Unit

    fun onTracksChanged(trackInfo: String) = Unit

    fun onVideoSize(videoSize: VideoSize) = Unit

    fun onState(state: Int) = Unit

    fun onPositionMs(positionMs: Long) = Unit

    fun onSeekResult(requestId: Long, positionMs: Long, success: Boolean) = Unit

    fun onError(error: PlayerError) = Unit

    fun onDecoderInfo(codecName: String, isHardware: Boolean) = Unit

    fun onFilterInfo(mode: Int, backend: String) = Unit

    fun onSubtitleInfo(hasSubtitles: Boolean, streamCount: Int, fontOk: Boolean) = Unit

    fun onEndOfStream() = Unit
}

/**
 * Entry point for the native ARIB playback engine.
 *
 * Native calls are deliberately declared on this object rather than hidden behind the controller
 * so that the JNI ABI remains small, explicit, and independently testable.
 */
object NativePlayer {
    init {
        System.loadLibrary("aribplayer")
    }

    /** Allocates a native player and retains [callbackObj] for asynchronous native callbacks. */
    external fun nativeCreate(callbackObj: Any): Long

    /**
     * Opens [fd] synchronously. Native code must duplicate the descriptor before this returns.
     *
     * @return true when the source was accepted; false when it could not be opened.
     */
    external fun nativeOpen(
        handle: Long,
        fd: Int,
        fontPath: String?,
        displayName: String,
        startPositionMs: Long,
    ): Boolean

    external fun nativeSetSurface(handle: Long, surface: Surface?)

    external fun nativePlay(handle: Long)

    external fun nativePause(handle: Long)

    external fun nativeSeekTo(handle: Long, positionMs: Long, requestId: Long): Boolean

    external fun nativeSetVideoMode(handle: Long, mode: Int)

    external fun nativeSelectAudioTrack(handle: Long, streamIndex: Int, dualMonoMode: Int)

    external fun nativeSetSubtitlesEnabled(handle: Long, enabled: Boolean)

    external fun nativeSetCaptionStyle(
        handle: Long,
        ignoreBackground: Boolean,
        forceOutlineText: Boolean,
    )

    external fun nativeGetDurationMs(handle: Long): Long

    /** Returns the current native diagnostics as JSON. */
    external fun nativeGetStats(handle: Long): String

    external fun nativeClose(handle: Long)

    external fun nativeRelease(handle: Long)

    /** Returns build information from the currently loaded native library. */
    external fun nativeGetVersionInfo(): String
}

/**
 * Callback target retained by native code. Its public methods intentionally use only JVM-stable
 * primitive and String arguments; do not rename or overload them without updating native lookups.
 */
internal class NativeCallbackBridge(listener: PlayerListener) {
    private val listenerRef = AtomicReference<PlayerListener?>(listener)
    private val mainHandler = Handler(Looper.getMainLooper())

    fun onPrepared(durationMs: Long, trackInfo: String, isSeekable: Boolean) {
        notifyListener { it.onPrepared(PreparedInfo(durationMs, trackInfo, isSeekable)) }
    }

    fun onTracksChanged(trackInfo: String) {
        notifyListener { it.onTracksChanged(trackInfo) }
    }

    fun onVideoSize(
        width: Int,
        height: Int,
        pixelAspectRatioNumerator: Int,
        pixelAspectRatioDenominator: Int,
    ) {
        notifyListener {
            it.onVideoSize(
                VideoSize(
                    width,
                    height,
                    pixelAspectRatioNumerator,
                    pixelAspectRatioDenominator,
                ),
            )
        }
    }

    fun onState(state: Int) {
        notifyListener { it.onState(state) }
    }

    fun onPositionMs(positionMs: Long) {
        notifyListener { it.onPositionMs(positionMs) }
    }

    fun onSeekResult(requestId: Long, positionMs: Long, success: Boolean) {
        notifyListener { it.onSeekResult(requestId, positionMs, success) }
    }

    fun onError(code: Int, message: String?) {
        notifyListener { it.onError(PlayerError(code, message)) }
    }

    fun onDecoderInfo(codecName: String, isHardware: Boolean) {
        notifyListener { it.onDecoderInfo(codecName, isHardware) }
    }

    fun onFilterInfo(mode: Int, backend: String) {
        notifyListener { it.onFilterInfo(mode, backend) }
    }

    fun onSubtitleInfo(hasSubtitles: Boolean, streamCount: Int, fontOk: Boolean) {
        notifyListener { it.onSubtitleInfo(hasSubtitles, streamCount, fontOk) }
    }

    fun onEndOfStream() {
        notifyListener { it.onEndOfStream() }
    }

    fun detach() {
        listenerRef.set(null)
    }

    private fun notifyListener(block: (PlayerListener) -> Unit) {
        if (listenerRef.get() == null) return
        // Never execute application code reentrantly on a demux/control thread:
        // listener close/release calls may synchronously tear those threads down.
        mainHandler.post {
            listenerRef.get()?.let { listener -> runCatching { block(listener) } }
        }
    }
}

/**
 * Thin, serialized owner of one native player handle.
 *
 * All native calls and release transition are serialized. After [release], mutating methods are
 * no-ops that return false, [open] returns false, and [getDurationMs] returns null.
 */
class PlayerController(listener: PlayerListener = object : PlayerListener {}) {
    private val lock = Any()
    private val callbackBridge = NativeCallbackBridge(listener)

    private var nativeHandle: Long = NativePlayer.nativeCreate(callbackBridge)
    private var released = false

    init {
        check(nativeHandle != 0L) { "Native player creation returned a null handle" }
    }

    val isReleased: Boolean
        get() = synchronized(lock) { released }

    /** Opens an input descriptor; native code owns a duplicate once this succeeds. */
    fun open(
        fd: Int,
        fontPath: String? = null,
        displayName: String,
        startPositionMs: Long = 0L,
    ): Boolean = synchronized(lock) {
        if (released) {
            false
        } else {
            NativePlayer.nativeOpen(
                nativeHandle,
                fd,
                fontPath,
                displayName,
                startPositionMs.coerceAtLeast(0L),
            )
        }
    }

    fun setSurface(surface: Surface?): Boolean = withActiveHandle { handle ->
        NativePlayer.nativeSetSurface(handle, surface)
    }

    fun play(): Boolean = withActiveHandle(NativePlayer::nativePlay)

    fun pause(): Boolean = withActiveHandle(NativePlayer::nativePause)

    fun seekTo(positionMs: Long, requestId: Long): Boolean = withActiveHandle { handle ->
        NativePlayer.nativeSeekTo(handle, positionMs, requestId)
    }

    /**
     * Sets a concrete per-session mode, or passes the app-default AUTO request before open.
     * AUTO is resolved once by PlayerCore from stream metadata and is not a FilterGraph mode.
     */
    fun setVideoMode(mode: Int): Boolean = withActiveHandle { handle ->
        NativePlayer.nativeSetVideoMode(handle, mode)
    }

    fun selectAudioTrack(streamIndex: Int, dualMonoMode: Int): Boolean = withActiveHandle { handle ->
        NativePlayer.nativeSelectAudioTrack(handle, streamIndex, dualMonoMode)
    }

    fun setSubtitlesEnabled(enabled: Boolean): Boolean = withActiveHandle { handle ->
        NativePlayer.nativeSetSubtitlesEnabled(handle, enabled)
    }

    fun setCaptionStyle(ignoreBackground: Boolean, forceOutlineText: Boolean): Boolean =
        withActiveHandle { handle ->
            NativePlayer.nativeSetCaptionStyle(handle, ignoreBackground, forceOutlineText)
        }

    fun getDurationMs(): Long? = synchronized(lock) {
        if (released) null else NativePlayer.nativeGetDurationMs(nativeHandle)
    }

    /** Returns current native diagnostics JSON, or null after release. */
    fun getStats(): String? = synchronized(lock) {
        if (released) null else NativePlayer.nativeGetStats(nativeHandle)
    }

    /** Closes the current source while retaining the player for a later [open]. */
    fun close(): Boolean = withActiveHandle(NativePlayer::nativeClose)

    /** Releases the native handle. This is idempotent and permanently disables this controller. */
    fun release(): Boolean = synchronized(lock) {
        if (released) {
            false
        } else {
            released = true
            val handle = nativeHandle
            nativeHandle = 0L
            callbackBridge.detach()
            if (handle != 0L) {
                NativePlayer.nativeRelease(handle)
            }
            true
        }
    }

    private inline fun withActiveHandle(call: (Long) -> Unit): Boolean = synchronized(lock) {
        if (released) {
            false
        } else {
            call(nativeHandle)
            true
        }
    }
}
