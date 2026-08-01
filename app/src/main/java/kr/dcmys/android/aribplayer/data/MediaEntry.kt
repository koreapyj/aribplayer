package kr.dcmys.android.aribplayer.data

import androidx.room.Entity
import androidx.room.PrimaryKey

@Entity(tableName = "media_entries")
data class MediaEntry(
    @PrimaryKey val uriString: String,
    val displayName: String,
    val sizeBytes: Long? = null,
    val durationMs: Long? = null,
    val resumePositionMs: Long = 0L,
    val videoMode: Int = -1,
    val audioTrackKey: String? = null,
    val subtitlesEnabled: Boolean? = null,
    val aspectMode: Int = 0,
    val lastOpenedEpochMs: Long,
    val accessible: Boolean = true
)
