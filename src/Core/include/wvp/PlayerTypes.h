#pragma once
// Strings are UTF-8; empty means null/absent.
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
    std::string Title;

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
    std::string Message;
    std::string Url;
    bool DeviceLost = false;       // the d3d11va decoder's device was removed ("device_lost"): rebind, not a media error
    std::string RemovedReason;     // "removed_reason"
};

// Type is "video" or "audio".
struct FilterErrorInfo {
    std::string Type;
    std::string Filter;
    int Code = 0;
    std::string Message;
    bool HwframesFallback = false;   // libffplay fell back from hwframes=keep to download ("hwframes_fallback":true)
};

/// FFP_EVENT_PRESENTER_ERROR {"presenter","error","device_lost","removed_reason"}: the presenter stopped for this open; the
/// host reopens (the presenter recovers its device in its next open).
struct PresenterErrorInfo {
    std::string Presenter, Error;  // Presenter: the FFPPresenter name
    bool DeviceLost = false;       // the presenter's device was removed / reset ("device_lost")
    std::string RemovedReason;     // "0x887a0005" ... ("removed_reason")
};

/// libaribcaption's "font" option value (the "arib_font" open option) for a font file: "file:<path>" with '/' separators
/// (the option parser reads a backslash as an escape). A value that already starts with "file:" only gets the separators.
std::string AribFontOption(const std::string& fontFile);

enum class StreamType { Unknown, Video, Audio, Subtitle };

struct StreamInfo {
    int Index = 0;
    StreamType Type = StreamType::Unknown;
    std::string Codec;
    bool Selected = false;
    std::string Language, Title;
    std::string AribType;                 // "caption" | "superimpose"; empty for other subtitle streams
    int ComponentTag = -1;
    int Width = 0, Height = 0, SarNum = 0, SarDen = 0;
    double Fps = 0;
    std::string PixFmt;
    std::string FourCC;                   // 4 chars from libffplay (container tag or RIFF tag); empty when none
    std::string FieldOrder;               // video: "progressive" | "tt" | "bb" | "tb" | "bt" | "unknown"
    std::string ColorTransfer;            // video: av_color_transfer_name ("smpte2084", "arib-std-b67", "bt709" ...)
    int SampleRate = 0, Channels = 0;
    bool external = false;                // subtitle added from a file (AddSubtitleFile)
    std::string path;                     // external subtitle: UTF-8 file path
    bool isText = false;                  // subtitle rendered by libass (everything but bitmap PGS and ARIB)
};

// ARIB transport stream: the audio roles of the video's program (media info "arib_audio"). Audio 1 and audio 2 are decoded
// together and chosen by a preference (IMediaPlayer::SetAudioPreference) instead of by cycling streams. Indexes are
// StreamInfo::Index at the time of the call; -1 = no such stream now (audio 2 may appear later).
struct AribAudioInfo {
    bool Present = false;
    int Audio1 = -1, Audio2 = -1;
};

struct MediaInfo {
    std::string Url, Title, Format;
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

// Source timestamps of the displayed video frame, media-relative seconds (the timeline of ffmpeg's input -ss): the
// frame itself and the key packet decoding has to start from to reach it.
struct FrameSeekPoint {
    double Frame = 0, Key = 0;
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
