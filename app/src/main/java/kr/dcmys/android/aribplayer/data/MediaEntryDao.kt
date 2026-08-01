package kr.dcmys.android.aribplayer.data

import androidx.room.Dao
import androidx.room.Query
import androidx.room.Upsert
import kotlinx.coroutines.flow.Flow

@Dao
interface MediaEntryDao {
    @Upsert
    suspend fun upsert(entry: MediaEntry)

    @Query(
        """
        SELECT * FROM media_entries
        ORDER BY lastOpenedEpochMs DESC
        LIMIT 50
        """
    )
    fun observeRecents(): Flow<List<MediaEntry>>

    @Query("SELECT * FROM media_entries WHERE uriString = :uri LIMIT 1")
    suspend fun getByUri(uri: String): MediaEntry?

    @Query("SELECT * FROM media_entries")
    suspend fun getAll(): List<MediaEntry>

    @Query(
        """
        UPDATE media_entries
        SET resumePositionMs = :positionMs,
            durationMs = :durationMs,
            lastOpenedEpochMs = :lastOpenedEpochMs
        WHERE uriString = :uri
        """
    )
    suspend fun updatePosition(
        uri: String,
        positionMs: Long,
        durationMs: Long?,
        lastOpenedEpochMs: Long
    )

    @Query("UPDATE media_entries SET audioTrackKey = :audioTrackKey WHERE uriString = :uri")
    suspend fun updateAudioTrack(uri: String, audioTrackKey: String?)

    @Query("UPDATE media_entries SET videoMode = :videoMode WHERE uriString = :uri")
    suspend fun updateVideoMode(uri: String, videoMode: Int)

    @Query("UPDATE media_entries SET subtitlesEnabled = :enabled WHERE uriString = :uri")
    suspend fun updateSubtitlesEnabled(uri: String, enabled: Boolean)

    @Query("UPDATE media_entries SET accessible = :accessible WHERE uriString = :uri")
    suspend fun updateAccessibility(uri: String, accessible: Boolean)

    @Query("DELETE FROM media_entries WHERE uriString = :uri")
    suspend fun delete(uri: String)
}
