#pragma once
// Capability-aware video pipeline planner. Turns intents (FilterSettings + IvtcTuning) and content info
// into one Plan: decoder (hwaccel and the decoder name), the vf string with hw transitions, the open options, the planned copy
// counts and the rejected alternatives. The presenter is the platform's (the PresenterCap of the capabilities); the planner
// only decides which frames reach it. Pure logic, unit-tested with fakecaps.
//
// Search: dynamic programming over the stages [decoder] [ivtc] [deinterlace] [setpts] [raw tokens...] [presenter], state =
// frame domain after the stage. Every candidate realization of a stage (each discovered variant, a domain transition before
// it) is costed; the cheapest complete path wins.
//
// Cost, compared lexicographically:
//   1. host copies + decodePenalty - decoder download (hwframes=download), hwupload, hwdownload, a presenter upload of system frames,
//                      plus the software-decode penalty floor(DecodeLoad) (see below);
//   2. device copies - hwmap with method "copy";
//   3. CPU load      - software decode load + CPU filter loads;
//   4. node count    - filters in the vf string (transitions included).
// Software-decode penalty: DecodeLoad = codec weight (mpeg2video 0.5, h264 1, hevc/vp9 2, av1 3, other 1; 10-bit x1.25)
// x pixels / 1920x1080 (unknown size = 1080p, unknown codec = weight 1). floor(DecodeLoad) host-copy equivalents are added,
// so cheap codecs (MPEG-2) may decode in software when that saves copies, while heavy ones stay on the GPU.
//
// Deinterlace On is one operation with bwdif and yadif as candidates: a GPU variant is always bwdif (a GPU yadif only in a domain
// without any bwdif), the CPU variant follows DeinterlacePriority (Efficiency yadif, Quality bwdif); cost picks GPU vs CPU. The
// step reason says why ("priority efficiency: no GPU deinterlacer -> yadif"). Driver deinterlacers are never planned
// (PipelineTable::IsDenied). Other intents are never silently degraded.
// Chroma: the presenter and the GPU filters take 4:2:0 hw frames only, so 4:2:2 / 4:4:4 content is never planned with
// hwframes=keep (GPU decode with download, or software).
// Decoders: every usable hardware decoder of the capabilities (order d3d11va, videotoolbox, d3d12va, dxva2) and software
// decoding are candidates; cost picks one, so software decoding wins only when no GPU decoder can take the stream or when it
// saves copies. libffplay always gets an explicit hwaccel for a hardware decoder, plus vcodec when the build's decoder for
// (codec, hwaccel) is not the codec's default decoder.
// Option mapping: only options the chosen filter exposes are emitted (FilterVariant::HasOption).
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "CapabilitySet.h"
#include "FilterSettings.h"
#include "IMediaPlayer.h"
#include "PlaybackController.h"
#include "PipelineTable.h"

namespace wvp {

enum class Chroma { Yuv420, Yuv422, Yuv444 };
const char* ChromaName(Chroma c);   // "4:2:0", "4:2:2", "4:4:4"

struct ContentInfo {
    std::string codec;         // codec id name ("h264", "mpeg2video", "hevc", ...); "" = unknown (planned before the first open)
    int bitDepth = 8;
    int width = 0, height = 0; // 0 = unknown
    bool hdr = false;
    Chroma chroma = Chroma::Yuv420;
    bool progressive = false;  // the stream declares field_order progressive; unknown or interlaced orders stay false
    bool Known() const { return !codec.empty(); }
    bool operator==(const ContentInfo&) const = default;
};

struct PlanRequest {
    FilterSettings settings;
    IvtcTuning ivtc;
    ContentInfo content;
    /// Elements that failed at runtime and must not be planned again: "hwaccel:d3d11va", "filter:ivtc_d3d11",
    /// "interop:d3d11>qsv", "hwframes_keep".
    std::set<std::string> blacklist;
};

enum class CopyKind { None, Host, Device, Map };
const char* CopyKindName(CopyKind k);   // "none", "host", "device", "map"

struct PlanStep {
    enum class Kind { Decode, Transfer, Filter, Present };
    Kind kind = Kind::Filter;
    std::string name;                    // "decode:d3d11va", "hwdownload", "ivtc_d3d11", "present:d3d11"...
    std::string text;                   // the vf text this step contributes ("" for decode/present/absorbed)
    FrameDomain in = FrameDomain::System, out = FrameDomain::System;
    CopyKind copy = CopyKind::None;
    int copies = 0;
    std::string reason;                  // why this realization ("cheapest: 0 host copies", "hw frames presented directly", ...)
};

struct PlanCost {
    int hostCopies = 0, deviceCopies = 0;   // real copies per frame (compare with the frame_path counters)
    int decodePenalty = 0;                  // floor(DecodeLoad) of software decoding, host-copy equivalents (cost only)
    double cpuLoad = 0;
    int nodes = 0;
    int Weighted() const { return hostCopies + decodePenalty; }   // first sort key
    bool operator<(const PlanCost& o) const;   // lexicographic as documented above (cpuLoad compared with 1e-9 tolerance)
    bool operator==(const PlanCost& o) const;
};

struct RejectedAlternative {
    std::string summary;                 // "d3d11va > hwdownload > fieldmatch,decimate > present:d3d11"
    PlanCost cost;
    std::string reason;                  // "2 host copies vs 1", "blacklisted: hwaccel:d3d11va", ...
};

struct Plan {
    bool ok = false;                     // false only when nothing can play the request (error says why)
    std::string error;
    std::string hwaccel;                 // "" = software decoding, else device type ("d3d11va", "videotoolbox", ...)
    std::string decoder;                 // vcodec: the build's decoder for (codec, hwaccel) when it is not the codec's default; "" = default
    bool hwframesKeep = false;           // libffplay hwframes=keep (hw frames enter the graph); false = download at decode
    std::string videoFilter;            // final vf string ("" = none)
    std::string filterHwDevice;          // "" or the filter_hw_device value ("d3d11va", "qsv", ...)
    PlanCost cost;
    std::vector<PlanStep> steps;         // decode ... present, in dataflow order
    std::vector<RejectedAlternative> rejected;   // best few alternatives (at most 6), cheapest first
    std::vector<std::string> notes;      // warnings / assumptions ("content unknown: planned for 1080p", ...)

    /// ffp_open options this plan needs (see PipelinePlanner::OpenOptionsFor).
    OpenOptions openOptions;
    /// Compact one-line summary: "d3d11va(keep) > ivtc_d3d11 > present:d3d11 [host 0, device 0]".
    std::string Summary() const;
};

enum class PlanDeltaKind { None, GraphRebuild, Reopen };

/// The cheapest live change from one plan to another.
struct PlanDelta {
    PlanDeltaKind kind = PlanDeltaKind::None;
    std::string reason;
};

class PipelinePlanner {
public:
    explicit PipelinePlanner(CapabilitySet caps);

    Plan Make(const PlanRequest& req) const;

    /// Reopen: hwaccel, decoder, hwframesKeep or filterHwDevice differ. GraphRebuild: videoFilter differs.
    static PlanDelta Diff(const Plan& before, const Plan& after);

    static double DecodeLoad(const ContentInfo& c);

    /// Raw/advanced chain tokenizer: top-level ',' split honouring '\' escapes, '...' quotes and [...] labels. Returns nullopt
    /// for a complex graph (';' or a [label] at top level) which is then treated as one opaque System block.
    static std::optional<std::vector<std::string>> TokenizeChain(const std::string& chain);
    static std::string TokenFilterName(const std::string& token);

    /// The ffp_open options for a plan: hwaccel (the device type; absent for software), vcodec when decoder is set,
    /// hwframes=keep|download for a hw decoder (always explicit: libffplay's default depends on the presenter), vf,
    /// filter_hw_device when needed.
    static OpenOptions OpenOptionsFor(const Plan& p);

private:
    CapabilitySet caps_;
    PipelineTable table_;
};

}
