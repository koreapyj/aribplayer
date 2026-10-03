#pragma once
// Strings are UTF-16 (std::wstring); empty means null/absent.
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace wvp {

enum class PlayerState { Idle, Opening, Playing, Paused, Stopped, Ended, Error };

// Payload of PREPARED. Duration is 0 for live streams.
struct MediaPreparedInfo {
    double Duration = 0;
    bool Seekable = false, HasVideo = false, HasAudio = false;
    int Width = 0, Height = 0, SarNum = 0, SarDen = 0;
    std::wstring Title;

    // Display size after applying the sample aspect ratio.
    int DisplayWidth() const {
        return SarNum > 0 && SarDen > 0 && SarNum > SarDen
            ? (int)std::lround(Width * (double)SarNum / SarDen) : Width;
    }
    int DisplayHeight() const {
        return SarNum > 0 && SarDen > 0 && SarNum < SarDen
            ? (int)std::lround(Height * (double)SarDen / SarNum) : Height;
    }
};

struct PlayerErrorInfo {
    int Code = 0;
    std::wstring Message;
    std::wstring Url;
    bool DeviceLost = false;       // the d3d11va decoder's device was removed ("device_lost"): rebind, not a media error
    std::string RemovedReason;     // "removed_reason"
};

// Type is "video" or "audio".
struct FilterErrorInfo {
    std::wstring Type;
    std::wstring Filter;
    int Code = 0;
    std::wstring Message;
    bool HwframesFallback = false;   // libffplay fell back from hwframes=keep to download ("hwframes_fallback":true)
};

/// Parameters a presenter applies itself (live, no graph rebuild).
struct PresenterParams {
    int rotation = 0;                    // 0, 90, 180, 270 clockwise
    bool flipH = false, flipV = false;   // after rotation (same composition as the filter path: transpose then hflip/vflip)
    bool eq = false;                     // brightness/contrast/saturation/gamma below are applied
    double brightness = 0, contrast = 1, saturation = 1, gamma = 1;   // ffmpeg eq semantics and ranges
    bool operator==(const PresenterParams&) const = default;
    bool IsIdentity() const { return rotation == 0 && !flipH && !flipV && !eq; }
};

/// libffplay presenter params JSON: {"rotation":90,"flip_h":false,"flip_v":false,"eq":{"brightness":..,"contrast":..,"saturation":..,"gamma":..}|null}
/// (the presenter_params open option and ffp_set_presenter_params).
std::wstring PresenterParamsJson(const PresenterParams& pp);

/// Plain C-layout mirror of FFPBackdropRegion; field order/types must match ffplay.h exactly.
/// Colors are 0xAARRGGBB-style packed values as defined by libffplay; coordinates are in output-window pixels.
struct BackdropRegion {
    int x = 0, y = 0, width = 0, height = 0;
    float corner_radius = 0;
    std::uint32_t tint_color = 0;
    float tint_opacity = 0;
    std::uint32_t luminosity_color = 0;
    float luminosity_opacity = 0;
    int noise = 0;
    int z = 0;
};
static_assert(sizeof(BackdropRegion) == 44, "BackdropRegion must match FFPBackdropRegion");

/// FFP_EVENT_PRESENTER_ERROR {"presenter","error","fallback"}.
struct PresenterErrorInfo {
    std::wstring Presenter, Error, Fallback;
    bool DeviceLost = false;       // the presenter's device was removed / reset ("device_lost"): rebind, not a d3d11 failure
    std::string RemovedReason;     // "0x887a0005" ... ("removed_reason")
};

enum class InputKind { Move, Down, Up, DoubleClick, Wheel, KeyDown, KeyUp };

enum class InputModifiers : int { None = 0, Shift = 1, Ctrl = 2, Alt = 4 };

// Typed INPUT event. Coordinates are client pixels of the video HWND.
struct InputEventArgs {
    InputKind Kind = InputKind::Move;
    int X = 0, Y = 0, Button = 0, Buttons = 0, Delta = 0, VirtualKey = 0;
    InputModifiers Modifiers = InputModifiers::None;
};

enum class StreamType { Unknown, Video, Audio, Subtitle };

struct StreamInfo {
    int Index = 0;
    StreamType Type = StreamType::Unknown;
    std::wstring Codec;
    bool Selected = false;
    std::wstring Language, Title;
    std::wstring AribType;                 // "caption" | "superimpose"; empty for other subtitle streams
    int ComponentTag = -1;
    int Width = 0, Height = 0, SarNum = 0, SarDen = 0;
    double Fps = 0;
    std::wstring PixFmt;
    std::wstring FourCC;                   // 4 chars from libffplay (container tag or RIFF tag); empty when none
    std::wstring ColorTransfer;            // video: av_color_transfer_name ("smpte2084", "arib-std-b67", "bt709" ...)
    int SampleRate = 0, Channels = 0;
};

// ARIB transport stream: the audio roles of the video's program (media info "arib_audio"). Audio 1 and audio 2 are decoded
// together and chosen by a preference (IMediaPlayer::SetAudioPreference) instead of by cycling streams. Indexes are
// StreamInfo::Index at the time of the call; -1 = no such stream now (audio 2 may appear later).
struct AribAudioInfo {
    bool Present = false;
    int Audio1 = -1, Audio2 = -1;
};

struct MediaInfo {
    std::wstring Url, Title, Format;
    double Duration = 0;
    bool Seekable = false;
    std::int64_t BitRate = 0;
    std::vector<StreamInfo> Streams;
    AribAudioInfo AribAudio;

    std::vector<StreamInfo> Of(StreamType t) const {
        std::vector<StreamInfo> r;
        for (auto& s : Streams) if (s.Type == t) r.push_back(s);
        return r;
    }

    // Parses the ffp_get_media_info JSON (UTF-8). nullopt if the JSON is invalid.
    static std::optional<MediaInfo> Parse(const std::string& json);
};

// Owned BGRA frame (bytes B,G,R,A; top-down rows; Stride bytes per row).
struct BgraFrame {
    int Width = 0, Height = 0, Stride = 0;
    std::vector<std::uint8_t> Data;

    static BgraFrame Create(int width, int height, std::vector<std::uint8_t> data) {
        BgraFrame f; f.Width = width; f.Height = height; f.Stride = width * 4; f.Data = std::move(data); return f;
    }
};

}
