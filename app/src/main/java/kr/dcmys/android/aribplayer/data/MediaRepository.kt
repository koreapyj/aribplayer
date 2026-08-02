package kr.dcmys.android.aribplayer.data

import android.content.Context
import android.net.Uri
import android.provider.DocumentsContract
import androidx.core.net.toUri
import java.io.File
import kr.dcmys.android.aribplayer.nativeplayer.VideoMode
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.withContext

class MediaRepository(
    context: Context,
    private val dao: MediaEntryDao,
    private val prefs: PlayerPreferencesStore
) {
    private val appContext = context.applicationContext

    val recents: Flow<List<MediaEntry>> = dao.observeRecents()
    val preferences: Flow<PlayerPreferences> = prefs.preferences

    suspend fun get(uri: String): MediaEntry? = dao.getByUri(uri)

    suspend fun upsert(entry: MediaEntry) {
        dao.upsert(entry)
    }

    suspend fun remove(uri: String) {
        dao.delete(uri)
    }

    suspend fun updatePosition(
        uri: String,
        positionMs: Long,
        durationMs: Long?,
        lastOpenedEpochMs: Long
    ) {
        dao.updatePosition(
            uri = uri,
            positionMs = positionMs.coerceAtLeast(0L),
            durationMs = durationMs?.coerceAtLeast(0L),
            lastOpenedEpochMs = lastOpenedEpochMs
        )
    }

    suspend fun updateAudioTrack(uri: String, audioTrackKey: String?) {
        dao.updateAudioTrack(uri, audioTrackKey)
    }

    suspend fun updateVideoMode(uri: String, videoMode: Int) {
        dao.updateVideoMode(uri, VideoMode.normalizeStored(videoMode))
    }

    suspend fun updateSubtitlesEnabled(uri: String, enabled: Boolean) {
        dao.updateSubtitlesEnabled(uri, enabled)
    }

    suspend fun updateAccessibility(uri: String, accessible: Boolean) {
        dao.updateAccessibility(uri, accessible)
    }

    suspend fun setDefaultVideoMode(videoMode: Int) {
        prefs.setDefaultVideoMode(videoMode)
    }

    suspend fun setSeekStepMs(seekStepMs: Long) {
        prefs.setSeekStepMs(seekStepMs)
    }

    suspend fun setControlsTimeoutMs(controlsTimeoutMs: Long) {
        prefs.setControlsTimeoutMs(controlsTimeoutMs)
    }

    suspend fun setDiagnosticsEnabled(enabled: Boolean) {
        prefs.setDiagnosticsEnabled(enabled)
    }

    /**
     * Refreshes entries whose underlying URI permission or local file availability has changed.
     * Content URIs require a persisted read grant, including grants inherited through a SAF tree.
     */
    suspend fun reconcilePersistedPermissions() = withContext(Dispatchers.IO) {
        val persistedReadUris = appContext.contentResolver.persistedUriPermissions
            .asSequence()
            .filter { it.isReadPermission }
            .map { it.uri }
            .toList()

        dao.getAll().forEach { entry ->
            val accessible = entry.uriString.accessibilityAfterReconciliation(persistedReadUris)
            if (accessible != null && accessible != entry.accessible) {
                dao.updateAccessibility(entry.uriString, accessible)
            }
        }
    }

    private fun String.accessibilityAfterReconciliation(
        persistedReadUris: List<Uri>
    ): Boolean? {
        val uri = toUri()
        return when (uri.scheme) {
            "content" -> uri.matchesPersistedReadGrant(persistedReadUris)
            "file" -> uri.path?.let(::isReadableLocalFile) ?: false
            null -> isReadableLocalFile(this)
            else -> null
        }
    }

    private fun Uri.matchesPersistedReadGrant(persistedReadUris: List<Uri>): Boolean {
        if (persistedReadUris.any { it == this }) return true
        if (!DocumentsContract.isTreeUri(this)) return false

        val targetTreeDocumentId = runCatching {
            DocumentsContract.getTreeDocumentId(this)
        }.getOrNull() ?: return false

        return persistedReadUris.any { persistedUri ->
            persistedUri.authority == authority &&
                DocumentsContract.isTreeUri(persistedUri) &&
                runCatching {
                    DocumentsContract.getTreeDocumentId(persistedUri) == targetTreeDocumentId
                }.getOrDefault(false)
        }
    }

    private fun isReadableLocalFile(path: String): Boolean = File(path).isFile && File(path).canRead()
}
