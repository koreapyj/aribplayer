#include "features/BadgeLogic.h"

#include <cwctype>

namespace app::badge {

namespace {

std::wstring Upper(std::wstring s) {
    for (auto& c : s) c = (wchar_t)std::towupper(c);
    return s;
}

std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = (wchar_t)std::towlower(c);
    return s;
}

std::wstring Widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }   // codec names are ASCII

const wvp::StreamInfo* PickStream(const wvp::MediaInfo& m, wvp::StreamType t) {
    const wvp::StreamInfo* first = nullptr;
    for (const auto& s : m.Streams) {
        if (s.Type != t) continue;
        if (s.Selected) return &s;
        if (!first) first = &s;
    }
    return first;
}

bool HardwareDecoding(const wvp::DebugVideo& v) {
    if (v.FramePath == "hw_direct" || v.FramePath == "hw_download") return true;
    return !v.HwaccelDevice.empty() && v.HwaccelDevice != "none";
}

}

std::wstring AudioCodecName(const std::wstring& codec) {
    std::wstring c = Lower(codec);
    if (c.empty()) return L"";
    if (c == L"aac") return L"AAC";
    if (c == L"ac3") return L"AC3";
    if (c == L"eac3") return L"E-AC3";
    if (c == L"mp2") return L"MP2";
    if (c == L"mp3") return L"MP3";
    if (c == L"opus") return L"OPUS";
    if (c == L"flac") return L"FLAC";
    if (c == L"truehd") return L"TrueHD";
    if (c == L"dts") return L"DTS";
    if (c.rfind(L"pcm_", 0) == 0) return L"PCM";
    return Upper(codec);
}

std::wstring ChannelsText(int channels) {
    switch (channels) {
        case 1: return L"1.0";
        case 2: return L"2.0";
        case 6: return L"5.1";
        case 8: return L"7.1";
        default: return channels > 0 ? std::to_wstring(channels) + L"ch" : std::wstring();
    }
}

std::vector<Chip> BuildChips(const wvp::DebugInfo& debug, const wvp::MediaInfo& media) {
    std::vector<Chip> chips;
    if (media.Streams.empty() && !debug.Valid) return chips;

    const wvp::StreamInfo* vs = PickStream(media, wvp::StreamType::Video);
    const wvp::StreamInfo* as = PickStream(media, wvp::StreamType::Audio);
    bool hasVideo = vs != nullptr || (debug.Valid && debug.HasVideo);
    bool hasAudio = as != nullptr || (debug.Valid && debug.HasAudio);

    // H/W | S/W needs the live decoder state, so it appears once the debug info is valid.
    if (hasVideo && debug.Valid && debug.HasVideo)
        chips.push_back({ChipKind::HwSw, HardwareDecoding(debug.Video) ? L"H/W" : L"S/W"});
    if (hasVideo) {
        // FourCC from libffplay (MPG2, H264, HEVC ...); the codec name when the container has none
        std::wstring name = Upper(vs && !vs->FourCC.empty() ? vs->FourCC : vs ? vs->Codec : Widen(debug.Video.Codec));
        if (vs && (vs->ColorTransfer == L"smpte2084" || vs->ColorTransfer == L"arib-std-b67"))
            chips.push_back({ChipKind::Hdr, L"HDR"});
        if (!name.empty()) chips.push_back({ChipKind::VideoCodec, name});
    }
    if (hasAudio) {
        std::wstring name = AudioCodecName(as ? as->Codec : Widen(debug.Audio.Codec));
        if (!name.empty()) chips.push_back({ChipKind::AudioCodec, name});
        int ch = as && as->Channels > 0 ? as->Channels : debug.Audio.Channels.value_or(0);
        std::wstring ct = ChannelsText(ch);
        if (!ct.empty()) chips.push_back({ChipKind::Channels, ct});
    }
    return chips;
}

}
