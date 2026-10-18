#pragma once
// "Copy ffmpeg command line for current frame": the start of a command that begins exactly at the displayed source frame.
#include <string>

#include "wvp/MediaItem.h"
#include "wvp/PlayerTypes.h"

namespace app {

/// `ffmpeg -ss HH:MM:SS.mmm -i <input> -ss S.mmm`: the input -ss is the frame's key packet (fast seek), the output -ss the
/// remaining offset to the frame. posixQuote = single quotes (shell), otherwise double quotes (Windows).
std::wstring FfmpegFrameCommandLine(const std::wstring& input, const wvp::FrameSeekPoint& p, bool posixQuote);

/// The item's local path in native form, or its URL as is; empty without an item.
std::wstring FfmpegInputOf(const wvp::MediaItem* item);

}
