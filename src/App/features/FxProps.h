#pragma once
// Properties model: ffp_get_media_info (wvp::MediaInfo, parsed with JsonLite by MediaInfo::Parse) -> sections of
// label/value rows, plus the formatting helpers. Pure (the file size is passed in).
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "wvp/PlayerTypes.h"

namespace app::fx {

struct PropRow { std::wstring label, value; };
struct PropSection { std::wstring title; std::vector<PropRow> rows; };

/// "1:02:03", "2:03", "0:07" (rounded down); "Unknown" when <= 0 / NaN.
std::wstring FormatDurationText(double seconds);
/// "123 kbps", "4.5 Mbps"; "Unknown" when <= 0.
std::wstring FormatBitRateText(std::int64_t bitsPerSecond);
/// "12.3 MB (12,345,678 bytes)" style: "532 bytes", "1.5 KB", "12.3 MB", "4.20 GB".
std::wstring FormatFileSizeText(std::int64_t bytes);
std::wstring FormatResolution(int w, int h);
/// Pixel aspect "1:1" and display aspect "16:9" (reduced; a ratio within 1% of a common one is shown as it, else "1.85:1").
std::wstring FormatSar(int num, int den);
std::wstring FormatDar(int w, int h, int sarNum, int sarDen);
std::wstring FormatFrameRate(double fps);          // "25 fps", "29.97 fps"
std::wstring FormatChannels(int channels);         // "Stereo", "Mono", "5.1", "2 channels"
std::wstring FormatSampleRate(int hz);             // "48 kHz", "44.1 kHz"
bool IsLocalPath(const std::wstring& s);

/// Sections: File (name/title, path or URL, container, duration, bitrate, size, seekable), Video, Audio, Subtitles.
/// Stream sections are omitted when the file has no such stream; a section holds one group of rows per stream
/// (rows are prefixed with "Stream N" separators when there are several).
std::vector<PropSection> BuildProperties(const wvp::MediaInfo& info, std::optional<std::int64_t> fileSizeBytes);

}
