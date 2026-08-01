package kr.dcmys.android.aribplayer.ui.player

import androidx.compose.runtime.Composable
import androidx.compose.runtime.Stable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.focus.FocusRequester

internal enum class PlayerSettingsPage {
    Main,
    VideoFilter,
    Audio,
    AppSettings,
    DefaultVideoMode,
    SeekStep,
}

internal enum class PlayerSettingsMainRow {
    VideoFilter,
    Audio,
    AppSettings,
}

internal enum class AppSettingsRow {
    DefaultVideoMode,
    SeekStep,
    Diagnostics,
}

@Stable
class PlayerChromeState internal constructor() {
    var controlsVisible by mutableStateOf(true)
        private set

    internal var settingsPage by mutableStateOf<PlayerSettingsPage?>(null)
        private set

    internal var settingsMainFocusTarget by mutableStateOf(PlayerSettingsMainRow.VideoFilter)
        private set

    internal var appSettingsFocusTarget by mutableStateOf(AppSettingsRow.DefaultVideoMode)
        private set

    var scrubbing by mutableStateOf(false)
        internal set

    var previewPositionMs by mutableStateOf<Long?>(null)
        internal set

    var interactionRevision by mutableLongStateOf(0L)
        private set

    val popupOpen: Boolean
        get() = settingsPage != null

    fun recordInteraction(showControls: Boolean = true) {
        if (showControls) controlsVisible = true
        interactionRevision++
    }

    fun showControls() {
        controlsVisible = true
        interactionRevision++
    }

    fun hideControls() {
        if (popupOpen || scrubbing) return
        controlsVisible = false
        previewPositionMs = null
    }

    fun onVideoTap() {
        if (popupOpen) {
            dismissSettings()
        } else if (controlsVisible) {
            hideControls()
        } else {
            showControls()
        }
    }

    fun openSettings() {
        settingsMainFocusTarget = PlayerSettingsMainRow.VideoFilter
        settingsPage = PlayerSettingsPage.Main
        recordInteraction()
    }

    internal fun openSettingsPage(page: PlayerSettingsPage) {
        if (page == PlayerSettingsPage.AppSettings) {
            appSettingsFocusTarget = AppSettingsRow.DefaultVideoMode
        }
        settingsPage = page
        recordInteraction()
    }

    internal fun openAppSettingsPage(page: PlayerSettingsPage, origin: AppSettingsRow) {
        appSettingsFocusTarget = origin
        settingsPage = page
        recordInteraction()
    }

    internal fun returnToSettingsMain(origin: PlayerSettingsMainRow) {
        settingsMainFocusTarget = origin
        settingsPage = PlayerSettingsPage.Main
        recordInteraction()
    }

    internal fun returnToAppSettings(origin: AppSettingsRow) {
        appSettingsFocusTarget = origin
        settingsPage = PlayerSettingsPage.AppSettings
        recordInteraction()
    }

    fun dismissSettings() {
        if (settingsPage == null) return
        settingsPage = null
        recordInteraction()
    }

    /** Returns true when the chrome consumed the back action. */
    fun handleBack(): Boolean = when (settingsPage) {
        PlayerSettingsPage.VideoFilter -> {
            returnToSettingsMain(PlayerSettingsMainRow.VideoFilter)
            true
        }
        PlayerSettingsPage.Audio -> {
            returnToSettingsMain(PlayerSettingsMainRow.Audio)
            true
        }
        PlayerSettingsPage.AppSettings -> {
            returnToSettingsMain(PlayerSettingsMainRow.AppSettings)
            true
        }
        PlayerSettingsPage.DefaultVideoMode -> {
            returnToAppSettings(AppSettingsRow.DefaultVideoMode)
            true
        }
        PlayerSettingsPage.SeekStep -> {
            returnToAppSettings(AppSettingsRow.SeekStep)
            true
        }
        PlayerSettingsPage.Main -> {
            dismissSettings()
            true
        }
        null -> false
    }
}

@Composable
fun rememberPlayerChromeState(): PlayerChromeState = remember { PlayerChromeState() }

@Stable
internal class PlayerFocusRequesters {
    val root = FocusRequester()
    val replay = FocusRequester()
    val playPause = FocusRequester()
    val forward = FocusRequester()
    val timeBar = FocusRequester()
    val info = FocusRequester()
    val captions = FocusRequester()
    val settings = FocusRequester()
}

@Composable
internal fun rememberPlayerFocusRequesters(): PlayerFocusRequesters =
    remember { PlayerFocusRequesters() }
