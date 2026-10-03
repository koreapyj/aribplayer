#pragma once
#include <string>
#include <vector>

#include "wvp/DebugInfo.h"
#include "wvp/PlayerTypes.h"

namespace app::badge {

enum class ChipKind { HwSw, Hdr, VideoCodec, AudioCodec, Channels };

struct Chip {
    ChipKind kind;
    std::wstring text;
    bool operator==(const Chip&) const = default;
};

/// Order: HwSw (video only), Hdr (selected video's ColorTransfer is smpte2084 or arib-std-b67), VideoCodec, AudioCodec, Channels. Empty when no media is loaded
/// (no streams). Codec/channels come from the selected stream of each type, falling back to the first one.
std::vector<Chip> BuildChips(const wvp::DebugInfo& debug, const wvp::MediaInfo& media);

std::wstring AudioCodecName(const std::wstring& codec);
std::wstring ChannelsText(int channels);   // 1 -> "1.0", 2 -> "2.0", 6 -> "5.1", 8 -> "7.1", else "<n>ch"; <= 0 -> ""

}
