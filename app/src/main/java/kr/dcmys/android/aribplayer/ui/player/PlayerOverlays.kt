package kr.dcmys.android.aribplayer.ui.player

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import kr.dcmys.android.aribplayer.PlaybackState
import kr.dcmys.android.aribplayer.PlayerUiState
import kr.dcmys.android.aribplayer.R
import kr.dcmys.android.aribplayer.ui.theme.PlayerColors
import kr.dcmys.android.aribplayer.ui.theme.PlayerDims
import kr.dcmys.android.aribplayer.ui.theme.PlayerTextStyles

@Composable
internal fun PlayerOverlays(
    state: PlayerUiState,
    modifier: Modifier = Modifier,
) {
    Box(modifier = modifier.fillMaxSize()) {
        if (state.playbackState == PlaybackState.PREPARING) {
            CircularProgressIndicator(
                color = Color.White,
                modifier = Modifier.align(Alignment.Center),
            )
        }

        state.errorMessage?.let { message ->
            Text(
                text = message,
                color = Color.White,
                style = PlayerTextStyles.ErrorText,
                modifier = Modifier
                    .align(Alignment.BottomCenter)
                    .padding(bottom = 64.dp)
                    .background(
                        color = PlayerColors.ErrorPill,
                        shape = RoundedCornerShape(PlayerDims.ErrorPillRadius),
                    )
                    .padding(horizontal = 16.dp, vertical = 8.dp),
            )
        }

        if (state.diagnosticsVisible) {
            DiagnosticsPanel(
                state = state,
                modifier = Modifier
                    .align(Alignment.TopEnd)
                    .padding(12.dp),
            )
        }
    }
}

@Composable
private fun DiagnosticsPanel(
    state: PlayerUiState,
    modifier: Modifier = Modifier,
) {
    val decoder = state.decoderName?.let { codecName ->
        val implementation = stringResource(
            if (state.decoderIsHardware == true) {
                R.string.player_decoder_hardware
            } else {
                R.string.player_decoder_software
            },
        )
        stringResource(R.string.player_diagnostics_decoder, codecName, implementation)
    } ?: stringResource(R.string.player_diagnostics_decoder_pending)

    val backend = stringResource(
        when (state.filterBackend) {
            "opencl" -> R.string.player_backend_opencl
            "software" -> R.string.player_backend_software
            "none" -> R.string.player_backend_bypass
            else -> R.string.player_backend_pending
        },
    )
    val filter = stringResource(
        R.string.player_diagnostics_filter,
        videoModeLabel(state.effectiveVideoMode),
        backend,
    )

    Column(
        modifier = modifier
            .widthIn(max = 360.dp)
            .background(PlayerColors.Popup, RoundedCornerShape(8.dp))
            .padding(12.dp),
    ) {
        Text(text = decoder, color = Color.White, style = PlayerTextStyles.Diagnostics)
        Text(text = filter, color = Color.White, style = PlayerTextStyles.Diagnostics)
        Text(
            text = state.statsJson ?: stringResource(R.string.player_diagnostics_waiting),
            color = Color.White,
            style = PlayerTextStyles.Diagnostics,
        )
    }
}
