package kr.dcmys.android.aribplayer.data

import android.content.Context
import androidx.datastore.core.DataStore
import androidx.datastore.preferences.core.Preferences
import androidx.datastore.preferences.core.booleanPreferencesKey
import androidx.datastore.preferences.core.edit
import androidx.datastore.preferences.core.emptyPreferences
import androidx.datastore.preferences.core.intPreferencesKey
import androidx.datastore.preferences.core.longPreferencesKey
import androidx.datastore.preferences.preferencesDataStore
import kr.dcmys.android.aribplayer.nativeplayer.VideoMode
import java.io.IOException
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.catch
import kotlinx.coroutines.flow.map

val Context.playerPreferencesDataStore: DataStore<Preferences> by preferencesDataStore(
    name = "player_preferences"
)

data class PlayerPreferences(
    val defaultVideoMode: Int = VideoMode.AUTO,
    val seekStepMs: Long = DEFAULT_SEEK_STEP_MS,
    val controlsTimeoutMs: Long = DEFAULT_CONTROLS_TIMEOUT_MS,
    val diagnosticsEnabled: Boolean = false
)

class PlayerPreferencesStore(context: Context) {
    private val dataStore = context.applicationContext.playerPreferencesDataStore

    val preferences: Flow<PlayerPreferences> = dataStore.data
        .catch { error ->
            if (error is IOException) emit(emptyPreferences()) else throw error
        }
        .map { storedPreferences ->
            PlayerPreferences(
                defaultVideoMode = VideoMode.normalizeDefault(
                    storedPreferences[PreferenceKeys.DEFAULT_VIDEO_MODE] ?: VideoMode.AUTO,
                ),
                seekStepMs = storedPreferences[PreferenceKeys.SEEK_STEP_MS] ?: DEFAULT_SEEK_STEP_MS,
                controlsTimeoutMs = storedPreferences[PreferenceKeys.CONTROLS_TIMEOUT_MS]
                    ?: DEFAULT_CONTROLS_TIMEOUT_MS,
                diagnosticsEnabled = storedPreferences[PreferenceKeys.DIAGNOSTICS_ENABLED] ?: false
            )
        }

    suspend fun setDefaultVideoMode(videoMode: Int) {
        dataStore.edit {
            it[PreferenceKeys.DEFAULT_VIDEO_MODE] = VideoMode.normalizeDefault(videoMode)
        }
    }

    suspend fun setSeekStepMs(seekStepMs: Long) {
        dataStore.edit { it[PreferenceKeys.SEEK_STEP_MS] = seekStepMs.coerceAtLeast(0L) }
    }

    suspend fun setControlsTimeoutMs(controlsTimeoutMs: Long) {
        dataStore.edit {
            it[PreferenceKeys.CONTROLS_TIMEOUT_MS] = controlsTimeoutMs.coerceAtLeast(0L)
        }
    }

    suspend fun setDiagnosticsEnabled(enabled: Boolean) {
        dataStore.edit { it[PreferenceKeys.DIAGNOSTICS_ENABLED] = enabled }
    }

    private object PreferenceKeys {
        val DEFAULT_VIDEO_MODE = intPreferencesKey("default_video_mode")
        val SEEK_STEP_MS = longPreferencesKey("seek_step_ms")
        val CONTROLS_TIMEOUT_MS = longPreferencesKey("controls_timeout_ms")
        val DIAGNOSTICS_ENABLED = booleanPreferencesKey("diagnostics_enabled")
    }
}

private const val DEFAULT_SEEK_STEP_MS = 30_000L
private const val DEFAULT_CONTROLS_TIMEOUT_MS = 5_000L
