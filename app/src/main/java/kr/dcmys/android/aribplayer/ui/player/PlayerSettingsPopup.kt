package kr.dcmys.android.aribplayer.ui.player

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Audiotrack
import androidx.compose.material.icons.filled.Check
import androidx.compose.material.icons.filled.ChevronLeft
import androidx.compose.material.icons.filled.ChevronRight
import androidx.compose.material.icons.filled.Info
import androidx.compose.material.icons.filled.Settings
import androidx.compose.material.icons.filled.Tune
import androidx.compose.material3.Icon
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusProperties
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.input.key.Key
import androidx.compose.ui.input.key.KeyEventType
import androidx.compose.ui.input.key.key
import androidx.compose.ui.input.key.onPreviewKeyEvent
import androidx.compose.ui.input.key.type
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.res.pluralStringResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.IntRect
import androidx.compose.ui.unit.IntSize
import androidx.compose.ui.unit.LayoutDirection
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.Popup
import androidx.compose.ui.window.PopupPositionProvider
import androidx.compose.ui.window.PopupProperties
import kr.dcmys.android.aribplayer.AudioTrackUi
import kr.dcmys.android.aribplayer.R
import kr.dcmys.android.aribplayer.data.PlayerPreferences
import kr.dcmys.android.aribplayer.nativeplayer.VideoMode
import kr.dcmys.android.aribplayer.ui.components.tvFocusRing
import kr.dcmys.android.aribplayer.ui.theme.PlayerColors
import kr.dcmys.android.aribplayer.ui.theme.PlayerDims
import kr.dcmys.android.aribplayer.ui.theme.PlayerTextStyles
import kotlinx.coroutines.android.awaitFrame

@Composable
internal fun PlayerSettingsPopup(
    page: PlayerSettingsPage,
    mainFocusTarget: PlayerSettingsMainRow,
    appSettingsFocusTarget: AppSettingsRow,
    selectedVideoMode: Int,
    tracks: List<AudioTrackUi>,
    selectedTrackKey: String?,
    preferences: PlayerPreferences,
    onOpenPage: (PlayerSettingsPage) -> Unit,
    onOpenAppSettingsPage: (PlayerSettingsPage, AppSettingsRow) -> Unit,
    onReturnToMain: (PlayerSettingsMainRow) -> Unit,
    onReturnToAppSettings: (AppSettingsRow) -> Unit,
    onSelectVideoMode: (Int) -> Unit,
    onSelectAudioTrack: (streamIndex: Int, dualMonoMode: Int) -> Unit,
    onSetDefaultVideoMode: (Int) -> Unit,
    onSetSeekStepMs: (Long) -> Unit,
    onSetDiagnosticsEnabled: (Boolean) -> Unit,
    onDismiss: () -> Unit,
    onInteraction: () -> Unit,
) {
    val gapPx = with(LocalDensity.current) { 8.dp.roundToPx() }
    Popup(
        popupPositionProvider = remember(gapPx) { AboveEndPopupPositionProvider(gapPx) },
        onDismissRequest = onDismiss,
        properties = PopupProperties(
            focusable = true,
            dismissOnBackPress = false,
            dismissOnClickOutside = true,
            clippingEnabled = true,
        ),
    ) {
        Surface(
            color = PlayerColors.Popup,
            contentColor = Color.White,
            shape = RoundedCornerShape(8.dp),
            modifier = Modifier
                .widthIn(min = PlayerDims.PopupMinWidth, max = PlayerDims.PopupMaxWidth)
                .onPreviewKeyEvent { event ->
                    if (event.type != KeyEventType.KeyDown || event.key != Key.Back) {
                        return@onPreviewKeyEvent false
                    }
                    when (page) {
                        PlayerSettingsPage.Main -> onDismiss()
                        PlayerSettingsPage.VideoFilter ->
                            onReturnToMain(PlayerSettingsMainRow.VideoFilter)
                        PlayerSettingsPage.Audio -> onReturnToMain(PlayerSettingsMainRow.Audio)
                        PlayerSettingsPage.AppSettings ->
                            onReturnToMain(PlayerSettingsMainRow.AppSettings)
                        PlayerSettingsPage.DefaultVideoMode ->
                            onReturnToAppSettings(AppSettingsRow.DefaultVideoMode)
                        PlayerSettingsPage.SeekStep ->
                            onReturnToAppSettings(AppSettingsRow.SeekStep)
                    }
                    true
                },
        ) {
            when (page) {
                PlayerSettingsPage.Main -> SettingsMainPage(
                    focusTarget = mainFocusTarget,
                    selectedVideoMode = selectedVideoMode,
                    tracks = tracks,
                    selectedTrackKey = selectedTrackKey,
                    onOpenPage = onOpenPage,
                    onInteraction = onInteraction,
                )
                PlayerSettingsPage.VideoFilter -> VideoFilterPage(
                    title = stringResource(R.string.player_video_filter),
                    selectedVideoMode = selectedVideoMode,
                    onBack = { onReturnToMain(PlayerSettingsMainRow.VideoFilter) },
                    onSelect = { mode ->
                        onSelectVideoMode(mode)
                        onReturnToMain(PlayerSettingsMainRow.VideoFilter)
                    },
                    onInteraction = onInteraction,
                )
                PlayerSettingsPage.Audio -> AudioPage(
                    tracks = tracks,
                    selectedTrackKey = selectedTrackKey,
                    onBack = { onReturnToMain(PlayerSettingsMainRow.Audio) },
                    onSelect = { track ->
                        onSelectAudioTrack(track.streamIndex, track.dualMonoMode)
                        onReturnToMain(PlayerSettingsMainRow.Audio)
                    },
                    onInteraction = onInteraction,
                )
                PlayerSettingsPage.AppSettings -> AppSettingsPage(
                    focusTarget = appSettingsFocusTarget,
                    preferences = preferences,
                    onOpenPage = onOpenAppSettingsPage,
                    onBack = { onReturnToMain(PlayerSettingsMainRow.AppSettings) },
                    onSetDiagnosticsEnabled = onSetDiagnosticsEnabled,
                    onInteraction = onInteraction,
                )
                PlayerSettingsPage.DefaultVideoMode -> VideoFilterPage(
                    title = stringResource(R.string.player_default_video_mode),
                    selectedVideoMode = preferences.defaultVideoMode,
                    includeAuto = true,
                    onBack = { onReturnToAppSettings(AppSettingsRow.DefaultVideoMode) },
                    onSelect = { mode ->
                        onSetDefaultVideoMode(mode)
                        onReturnToAppSettings(AppSettingsRow.DefaultVideoMode)
                    },
                    onInteraction = onInteraction,
                )
                PlayerSettingsPage.SeekStep -> SeekStepPage(
                    selectedSeekStepMs = preferences.seekStepMs,
                    onBack = { onReturnToAppSettings(AppSettingsRow.SeekStep) },
                    onSelect = { value ->
                        onSetSeekStepMs(value)
                        onReturnToAppSettings(AppSettingsRow.SeekStep)
                    },
                    onInteraction = onInteraction,
                )
            }
        }
    }
}

@Composable
private fun SettingsMainPage(
    focusTarget: PlayerSettingsMainRow,
    selectedVideoMode: Int,
    tracks: List<AudioTrackUi>,
    selectedTrackKey: String?,
    onOpenPage: (PlayerSettingsPage) -> Unit,
    onInteraction: () -> Unit,
) {
    val requesters = remember { List(3) { FocusRequester() } }
    val audioEnabled = tracks.isNotEmpty()
    val selectedTrack = tracks.firstOrNull { it.key == selectedTrackKey } ?: tracks.firstOrNull()
    LaunchedEffect(focusTarget, audioEnabled) {
        awaitFrame()
        val index = when (focusTarget) {
            PlayerSettingsMainRow.VideoFilter -> 0
            PlayerSettingsMainRow.Audio -> if (audioEnabled) 1 else 0
            PlayerSettingsMainRow.AppSettings -> 2
        }
        requesters[index].requestFocus()
    }
    Column {
        MainSettingsRow(
            icon = Icons.Filled.Tune,
            title = stringResource(R.string.player_video_filter),
            subtitle = videoModeLabel(selectedVideoMode),
            enabled = true,
            showChevron = true,
            modifier = Modifier
                .focusRequester(requesters[0])
                .focusProperties { down = if (audioEnabled) requesters[1] else requesters[2] },
            onClick = {
                onInteraction()
                onOpenPage(PlayerSettingsPage.VideoFilter)
            },
        )
        MainSettingsRow(
            icon = Icons.Filled.Audiotrack,
            title = stringResource(R.string.player_track_title_audio),
            subtitle = selectedTrack?.displayName ?: stringResource(R.string.player_track_none),
            enabled = audioEnabled,
            showChevron = true,
            modifier = Modifier
                .focusRequester(requesters[1])
                .focusProperties {
                    up = requesters[0]
                    down = requesters[2]
                    canFocus = audioEnabled
                },
            onClick = {
                onInteraction()
                onOpenPage(PlayerSettingsPage.Audio)
            },
        )
        MainSettingsRow(
            icon = Icons.Filled.Settings,
            title = stringResource(R.string.player_settings_description),
            subtitle = stringResource(R.string.player_app_settings_subtitle),
            enabled = true,
            showChevron = true,
            modifier = Modifier
                .focusRequester(requesters[2])
                .focusProperties { up = if (audioEnabled) requesters[1] else requesters[0] },
            onClick = {
                onInteraction()
                onOpenPage(PlayerSettingsPage.AppSettings)
            },
        )
    }
}

@Composable
private fun AppSettingsPage(
    focusTarget: AppSettingsRow,
    preferences: PlayerPreferences,
    onOpenPage: (PlayerSettingsPage, AppSettingsRow) -> Unit,
    onBack: () -> Unit,
    onSetDiagnosticsEnabled: (Boolean) -> Unit,
    onInteraction: () -> Unit,
) {
    val requesters = remember { List(4) { FocusRequester() } }
    LaunchedEffect(focusTarget) {
        awaitFrame()
        requesters[focusTarget.ordinal + 1].requestFocus()
    }
    Column {
        PopupHeaderRow(
            title = stringResource(R.string.player_settings_description),
            modifier = Modifier
                .focusRequester(requesters[0])
                .focusProperties { down = requesters[1] },
            onClick = {
                onInteraction()
                onBack()
            },
        )
        MainSettingsRow(
            icon = Icons.Filled.Tune,
            title = stringResource(R.string.player_default_video_mode),
            subtitle = videoModeLabel(preferences.defaultVideoMode),
            enabled = true,
            showChevron = true,
            modifier = Modifier
                .focusRequester(requesters[1])
                .focusProperties { up = requesters[0]; down = requesters[2] },
            onClick = {
                onInteraction()
                onOpenPage(PlayerSettingsPage.DefaultVideoMode, AppSettingsRow.DefaultVideoMode)
            },
        )
        MainSettingsRow(
            icon = Icons.Filled.ChevronRight,
            title = stringResource(R.string.player_seek_step),
            subtitle = seekStepLabel(preferences.seekStepMs),
            enabled = true,
            showChevron = true,
            modifier = Modifier
                .focusRequester(requesters[2])
                .focusProperties { up = requesters[1]; down = requesters[3] },
            onClick = {
                onInteraction()
                onOpenPage(PlayerSettingsPage.SeekStep, AppSettingsRow.SeekStep)
            },
        )
        MainSettingsRow(
            icon = Icons.Filled.Info,
            title = stringResource(R.string.player_diagnostics),
            subtitle = stringResource(
                if (preferences.diagnosticsEnabled) R.string.player_on else R.string.player_off,
            ),
            enabled = true,
            showChevron = false,
            modifier = Modifier
                .focusRequester(requesters[3])
                .focusProperties { up = requesters[2] },
            onClick = {
                onInteraction()
                onSetDiagnosticsEnabled(!preferences.diagnosticsEnabled)
            },
        )
    }
}

@Composable
private fun VideoFilterPage(
    title: String,
    selectedVideoMode: Int,
    includeAuto: Boolean = false,
    onBack: () -> Unit,
    onSelect: (Int) -> Unit,
    onInteraction: () -> Unit,
) {
    val modes = buildList {
        if (includeAuto) add(VideoMode.AUTO)
        add(VideoMode.DEINTERLACE)
        add(VideoMode.IVTC)
        add(VideoMode.OFF)
    }
    OptionPage(
        title = title,
        labels = modes.map { videoModeLabel(it) },
        selectedIndex = modes.indexOf(selectedVideoMode),
        onBack = onBack,
        onSelect = { onSelect(modes[it]) },
        onInteraction = onInteraction,
    )
}

@Composable
private fun SeekStepPage(
    selectedSeekStepMs: Long,
    onBack: () -> Unit,
    onSelect: (Long) -> Unit,
    onInteraction: () -> Unit,
) {
    val steps = listOf(10_000L, 30_000L)
    OptionPage(
        title = stringResource(R.string.player_seek_step),
        labels = steps.map { seekStepLabel(it) },
        selectedIndex = steps.indexOf(selectedSeekStepMs),
        onBack = onBack,
        onSelect = { onSelect(steps[it]) },
        onInteraction = onInteraction,
    )
}

@Composable
private fun AudioPage(
    tracks: List<AudioTrackUi>,
    selectedTrackKey: String?,
    onBack: () -> Unit,
    onSelect: (AudioTrackUi) -> Unit,
    onInteraction: () -> Unit,
) {
    OptionPage(
        title = stringResource(R.string.player_track_title_audio),
        labels = tracks.map { it.displayName },
        selectedIndex = tracks.indexOfFirst { it.key == selectedTrackKey },
        onBack = onBack,
        onSelect = { onSelect(tracks[it]) },
        onInteraction = onInteraction,
    )
}

@Composable
private fun OptionPage(
    title: String,
    labels: List<String>,
    selectedIndex: Int,
    onBack: () -> Unit,
    onSelect: (Int) -> Unit,
    onInteraction: () -> Unit,
) {
    val requesters = remember(labels) { List(labels.size + 1) { FocusRequester() } }
    LaunchedEffect(labels) {
        awaitFrame()
        requesters.first().requestFocus()
    }
    Column {
        PopupHeaderRow(
            title = title,
            modifier = Modifier
                .focusRequester(requesters[0])
                .focusProperties { if (labels.isNotEmpty()) down = requesters[1] },
            onClick = {
                onInteraction()
                onBack()
            },
        )
        labels.forEachIndexed { index, label ->
            val requesterIndex = index + 1
            PopupOptionRow(
                label = label,
                checked = selectedIndex == index,
                modifier = Modifier
                    .focusRequester(requesters[requesterIndex])
                    .focusProperties {
                        up = requesters[requesterIndex - 1]
                        if (requesterIndex < requesters.lastIndex) {
                            down = requesters[requesterIndex + 1]
                        }
                    },
                onClick = {
                    onInteraction()
                    onSelect(index)
                },
            )
        }
    }
}

@Composable
private fun MainSettingsRow(
    icon: ImageVector,
    title: String,
    subtitle: String,
    enabled: Boolean,
    showChevron: Boolean,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
) {
    val color = if (enabled) Color.White else PlayerColors.Disabled
    Row(
        modifier = modifier
            .fillMaxWidth()
            .height(PlayerDims.PopupRowHeight)
            .tvFocusRing()
            .clickable(enabled = enabled, onClick = onClick)
            .padding(horizontal = 12.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Icon(icon, contentDescription = null, tint = color, modifier = Modifier.size(24.dp))
        Spacer(Modifier.width(12.dp))
        Column(modifier = Modifier.weight(1f)) {
            Text(text = title, color = color, style = PlayerTextStyles.PopupTitle, maxLines = 1)
            Text(
                text = subtitle,
                color = if (enabled) PlayerColors.SecondaryText else PlayerColors.Disabled,
                style = PlayerTextStyles.PopupSubtitle,
                maxLines = 1,
            )
        }
        if (showChevron) {
            Icon(
                Icons.Filled.ChevronRight,
                contentDescription = null,
                tint = color,
                modifier = Modifier.size(24.dp),
            )
        }
    }
}

@Composable
private fun PopupHeaderRow(title: String, onClick: () -> Unit, modifier: Modifier = Modifier) {
    Row(
        modifier = modifier
            .fillMaxWidth()
            .height(PlayerDims.PopupRowHeight)
            .tvFocusRing()
            .clickable(onClick = onClick)
            .padding(horizontal = 12.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Icon(
            Icons.Filled.ChevronLeft,
            contentDescription = null,
            tint = Color.White,
            modifier = Modifier.size(24.dp),
        )
        Spacer(Modifier.width(12.dp))
        Text(text = title, color = Color.White, style = PlayerTextStyles.PopupTitle, maxLines = 1)
    }
}

@Composable
private fun PopupOptionRow(
    label: String,
    checked: Boolean,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
) {
    Row(
        modifier = modifier
            .fillMaxWidth()
            .height(PlayerDims.PopupRowHeight)
            .tvFocusRing()
            .clickable(onClick = onClick)
            .padding(horizontal = 12.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Box(modifier = Modifier.size(24.dp), contentAlignment = Alignment.Center) {
            if (checked) {
                Icon(
                    Icons.Filled.Check,
                    contentDescription = null,
                    tint = Color.White,
                    modifier = Modifier.size(24.dp),
                )
            }
        }
        Spacer(Modifier.width(12.dp))
        Text(text = label, color = Color.White, style = PlayerTextStyles.PopupTitle, maxLines = 1)
    }
}

@Composable
internal fun videoModeLabel(mode: Int): String = stringResource(
    when (mode) {
        VideoMode.AUTO -> R.string.player_mode_auto
        VideoMode.IVTC -> R.string.player_mode_ivtc
        VideoMode.DEINTERLACE -> R.string.player_mode_deinterlace
        VideoMode.OFF -> R.string.player_mode_off
        else -> R.string.player_mode_deinterlace
    },
)

@Composable
private fun seekStepLabel(valueMs: Long): String {
    val seconds = (valueMs / 1_000L).toInt()
    return pluralStringResource(R.plurals.player_seconds, seconds, seconds)
}

private class AboveEndPopupPositionProvider(private val gapPx: Int) : PopupPositionProvider {
    override fun calculatePosition(
        anchorBounds: IntRect,
        windowSize: IntSize,
        layoutDirection: LayoutDirection,
        popupContentSize: IntSize,
    ): IntOffset {
        val maxX = (windowSize.width - popupContentSize.width).coerceAtLeast(0)
        val x = (anchorBounds.right - popupContentSize.width).coerceIn(0, maxX)
        val preferredY = anchorBounds.top - gapPx - popupContentSize.height
        val fallbackY = anchorBounds.bottom + gapPx
        val maxY = (windowSize.height - popupContentSize.height).coerceAtLeast(0)
        val y = (if (preferredY >= 0) preferredY else fallbackY).coerceIn(0, maxY)
        return IntOffset(x, y)
    }
}
