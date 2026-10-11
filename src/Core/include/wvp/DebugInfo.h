#pragma once
// Parsed form of libffplay's ffp_get_debug_info JSON (see docs/libffplay-api.md).
// Absent numbers are nullopt, absent strings "", absent sections have Has* == false.
// A JSON null number is also nullopt.
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace wvp {

struct DebugVideo {
    int StreamIndex = -1;
    std::string Codec, Decoder, Hwaccel, PixFmt, HwPixFmt, SwPixFmt;
    std::optional<int> Width, Height;
    std::string Sar;                       // "num:den"
    std::optional<double> FrameRate;
    std::optional<int> OutWidth, OutHeight;
    std::string OutPixFmt, TimeBase;
    std::optional<double> StartTime;
    // Requested vs actual (runtime observed) hardware decoding path.
    std::string HwaccelRequested;          // "d3d11va" or ""
    std::string HwaccelDevice;             // device type really used for decoding, "none" if not
    std::string HwaccelError;              // why a requested hwaccel is not in use
    std::string FramePath;                 // "hw_direct" | "hw_download" | "software" | "" (last decoded frame)
    std::optional<std::int64_t> FramesHwDirect, FramesHwDownload, FramesSoftware;
    std::string PixFmtDecoded, PixFmtAfterTransfer;
    // The hwframes open option as requested ("keep"|"download"|"") and the mode really in use ("keep"|"download");
    // frames delivered through the download sink. Absent values are "" / nullopt.
    std::string HwframesRequested, Hwframes;
    std::optional<std::int64_t> FramesSinkDownload;
};

struct DebugAcrylic {
    bool Present = false;
    std::optional<int> Regions;
    std::optional<std::int64_t> FramesBlurred, Failures;
    std::optional<double> BlurMs;
};

// "presenter" object; Present == false when absent. Name / Error / FramesDirect / FramesUploaded are the engine's; the rest
// comes from the presenter's own "stats" object (FFPPresenter.stats_json; the D3D11 presenter's members below).
struct DebugPresenter {
    bool Present = false;
    std::string Name;                      // the presenter's FFPPresenter name ("d3d11", "metal")
    std::string Error;                     // non-empty after the presenter failed for this open
    std::optional<std::int64_t> FramesDirect, FramesUploaded;
    DebugAcrylic Acrylic;
    std::string Renderer;                  // the drawing path in use ("d3d11:videoprocessor", "d3d11:shader", ...; "" when absent)
    std::string Route;                     // "vp" | "shader" ("" when absent)
    std::optional<std::int64_t> FramesShader, FramesD3d11Copied;
    // The presenter's adapter ("adapter": name, vendor_id, luid "HHHHHHHH:LLLLLLLL"); empty when absent.
    std::string AdapterName, AdapterLuid;
    std::optional<int> AdapterVendor;
    std::optional<bool> DebugLayer;        // d3d11_debug: the debug layer is active
    std::optional<std::int64_t> DebugMessages;
    std::vector<std::string> Fallbacks;    // "decoder_pool_no_srv", "p010_vp_unsupported:shader", "force_caps:p010=0", ...
    // "frame_stats": DXGI frame statistics of the swap chain and GPU timing of the presenter's draws.
    std::optional<bool> DxgiValid;
    std::optional<std::int64_t> DxgiPresentCount, DxgiPresentRefreshCount, DxgiSyncRefreshCount;
    std::optional<std::int64_t> DxgiSyncQpcTime, DxgiSyncGpuTime;
    std::optional<std::int64_t> D3dGpuTotalUs, D3dGpuFrames, D3dGpuLastUs;
};

struct DebugAudio {
    int StreamIndex = -1;
    std::string Codec, Decoder;
    std::optional<int> SampleRate, Channels;
    std::string ChannelLayout, SampleFmt, TimeBase;
    std::optional<double> StartTime;
    std::string OutDevice;
    std::optional<int> OutSampleRate, OutChannels, HwBufSize;
    std::optional<double> DiffAvg, LatencyMs;
};

// ARIB audio model ("arib_audio"; absent for other media): the two roles, the preference and what plays now.
struct DebugAribRole {
    std::string Role;                      // "audio1" | "audio2"
    bool Present = false;                  // a stream holds the role now
    int StreamIndex = -1, Pid = -1, ComponentTag = -1;
    bool Starved = false;                  // no data from it for longer than the starvation limit (or removed by a PMT)
    std::optional<double> BufferedMs;      // decoded audio ahead of the output cursor
};

struct DebugAribAudio {
    bool Present = false;
    std::string Preference;                // "audio1" | "audio2"
    std::string Playing;                   // "audio1" | "audio2" | "silence" | "" (nothing rendered yet)
    DebugAribRole Audio1, Audio2;
    std::optional<std::int64_t> Switches, Rebinds;
    std::optional<double> SilenceMs;       // silence synthesised where no source had samples
};

struct DebugSubtitle {
    int StreamIndex = -1;
    std::string Codec, TimeBase;
    std::optional<double> StartTime;
};

struct DebugSubtitleLayer {
    bool Present = false;
    int StreamIndex = -1;
    std::string Codec, DecoderOptions, ResolvedFont;
    std::optional<int> RectCount, CanvasWidth, CanvasHeight;
    std::optional<double> CueStart, CueEnd;
    std::optional<std::int64_t> CuesShown, CuesCleared, BitmapsUploaded, Rerenders;
    std::optional<std::int64_t> HistoryEvents, HistoryBytes, HistoryAppendedBytes;   // ARIB caption history
};

struct DebugFilterNode {
    std::string Name, Instance, Args, HwDevice;
};

struct DebugFilters {
    std::string VfRequested, AfRequested, VfGraph, AfGraph;
    std::vector<std::string> VfFilters, AfFilters;
    std::vector<DebugFilterNode> VfNodes, AfNodes;
};

struct DebugSync {
    std::string Master;                    // "audio" | "video" | "external"
    std::optional<double> AudioClock, VideoClock, ExternalClock, MasterClock;
    std::optional<double> AvDiffMs;
    std::optional<std::int64_t> FrameDropsEarly, FrameDropsLate, FramesDuplicated;
    std::optional<double> FrameTimerDriftMs;
};

struct DebugSeekInfo {
    bool Valid = false;
    std::string Direction;
    std::optional<double> Requested, Keyframe, DistanceMs, GopMs;
};

struct DebugFrameMark {
    std::optional<double> Pts;
    std::optional<std::int64_t> Serial, AtUs, Count;
};

struct DebugFrameEvent {
    std::string Kind;
    std::optional<std::int64_t> Count, Serial, AtUs;
    std::optional<double> Pts;
};

// FFmpeg presentation trace; Present == false when absent.
struct DebugFrameTrace {
    bool Present = false;
    DebugFrameMark Decoded, Filtered, Presented, EarlyDrop, LateDrop;
    std::vector<DebugFrameEvent> Events;
};

struct DebugQueue {
    std::optional<std::int64_t> Packets, Bytes;
    std::optional<double> Duration;
};

// Output frame rates (top-level "fps" object). All values are runtime observed;
// nullopt = unknown (absent, no video, not measurable yet).
struct DebugFps {
    bool Present = false;
    std::optional<double> StreamAvg, StreamR;   // AVStream avg_frame_rate / r_frame_rate
    std::optional<double> FilterOut;       // buffersink frame rate (or 1/time_base when the sink has none)
    std::string FilterOutTimeBase, FilterOutSource; // "a/b"; "sink_frame_rate" | "time_base" | ""
    std::optional<double> Decoded, Filtered, Displayed; // measured over a ~1.5 s sliding window
    std::optional<std::int64_t> FramesDecoded, FramesFiltered, FramesDisplayed; // cumulative
    std::optional<std::int64_t> DropsEarly, DropsLate;
};

struct DebugInfo {
    bool Valid = false;                    // JSON parsed as an object
    std::optional<double> Position, Duration;
    std::optional<std::int64_t> ClockUs;
    bool Paused = false;
    std::optional<double> Speed;
    bool HasVideo = false, HasAudio = false, HasSubtitle = false;
    DebugVideo Video;
    DebugAudio Audio;
    DebugAribAudio AribAudio;
    DebugSubtitle Subtitle;
    DebugSubtitleLayer CaptionLayer, SuperimposeLayer;
    DebugFilters Filters;
    DebugSync Sync;
    DebugSeekInfo Seek;
    DebugFps Fps;
    DebugFrameTrace FrameTrace;
    DebugPresenter Presenter;
    DebugQueue AudioQueue, VideoQueue, SubtitleQueue;
};

// Never throws; invalid JSON yields a default DebugInfo with Valid == false.
DebugInfo ParseDebugInfo(const std::string& json);

}
