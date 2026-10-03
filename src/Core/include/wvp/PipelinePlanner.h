#pragma once
// Capability-aware video pipeline planner. Turns intents (FilterSettings + IvtcTuning), content info and preferences
// into one Plan: decoder (hwaccel), the vf string with hw transitions, the presenter and what the presenter absorbs, the open
// options, the planned copy counts and the rejected alternatives. Pure logic, unit-tested with fakecaps.
//
// Search: dynamic programming over the stages [decoder] [ivtc] [deinterlace] [transpose] [hflip] [vflip] [eq] [setpts]
// [raw tokens...] [presenter], state = frame domain after the stage. Every candidate realization of a stage (each discovered
// variant, "absorbed by the presenter", a domain transition before it) is costed; the cheapest complete path wins.
//
// Cost, compared lexicographically:
//   1. host copies + decodePenalty - decoder download (hwframes=download), hwupload, hwdownload, a presenter upload of system frames,
//                      plus the software-decode penalty floor(DecodeLoad) (see below);
//   2. device copies - hwmap with method "copy", per-plane cuda presenter imports (registered CUDA pools cost zero);
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
// Option mapping: only options the chosen filter exposes are emitted (FilterVariant::HasOption).
// Policy Legacy uses FilterManager strings (hw filter variant when available, decoder download, sdl).
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
    bool Known() const { return !codec.empty(); }
    bool operator==(const ContentInfo&) const = default;
};

enum class DecoderPref { Auto, PreferGpu, Forced, Software };
enum class PresenterPref { Auto, D3D11, Sdl };
enum class PlanPolicy { Cost, Legacy };

struct PlanRequest {
    FilterSettings settings;
    IvtcTuning ivtc;
    ContentInfo content;
    DecoderPref decoder = DecoderPref::Auto;
    std::string forcedDecoder;           // hwaccel device type for DecoderPref::Forced ("cuda", "d3d11va", ...)
    PresenterPref presenter = PresenterPref::Auto;
    PlanPolicy policy = PlanPolicy::Cost;
    /// Elements that failed at runtime and must not be planned again: "hwaccel:cuda", "filter:ivtc_cuda", "presenter:d3d11",
    /// "interop:cuda>d3d11", "hwframes_keep". "absorb:eq" / "absorb:rotate" / "absorb:flip" stop the d3d11 presenter from absorbing that
    /// operation (the presenter refused the parameters at runtime): it then runs as a filter in the graph.
    std::set<std::string> blacklist;
};

enum class CopyKind { None, Host, Device, Map };
const char* CopyKindName(CopyKind k);   // "none", "host", "device", "map"

struct PlanStep {
    enum class Kind { Decode, Transfer, Filter, Present };
    Kind kind = Kind::Filter;
    std::string name;                    // "decode:cuda", "hwdownload", "ivtc_cuda", "present:d3d11", "absorbed:transpose"...
    std::wstring text;                   // the vf text this step contributes ("" for decode/present/absorbed)
    FrameDomain in = FrameDomain::System, out = FrameDomain::System;
    CopyKind copy = CopyKind::None;
    int copies = 0;
    std::string reason;                  // why this realization ("cheapest: 0 host copies", "absorbed by presenter VP", ...)
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
    std::string summary;                 // "d3d11va > hwdownload > fieldmatch,decimate > sdl"
    PlanCost cost;
    std::string reason;                  // "2 host copies vs 1", "blacklisted: hwaccel:cuda", "presenter d3d11 unavailable", ...
};

struct Plan {
    bool ok = false;                     // false only when nothing can play the request (error says why)
    std::string error;
    std::string hwaccel;                 // "" = software decoding, else device type ("cuda", "d3d11va", ...)
    bool hwframesKeep = false;           // libffplay hwframes=keep (hw frames enter the graph); false = download at decode
    std::wstring videoFilter;            // final vf string ("" = none)
    std::string filterHwDevice;          // "" or the filter_hw_device value ("cuda", "d3d11va", ...)
    std::string presenter = "sdl";       // "sdl" | "d3d11"
    PresenterParams presenterParams;
    PlanCost cost;
    std::vector<PlanStep> steps;         // decode ... present, in dataflow order
    std::vector<RejectedAlternative> rejected;   // best few alternatives (at most 6), cheapest first
    std::vector<std::string> notes;      // warnings / assumptions ("content unknown: planned for 1080p", ...)

    /// ffp_open options this plan needs (hwaccel or "off"... see PipelinePlanner::OpenOptionsFor).
    OpenOptions openOptions;
    /// vf with eq@color arguments replaced by "*": equal structure = no graph rebuild needed.
    std::wstring vfStructure;
    /// Compact one-line summary: "cuda(keep) > ivtc_cuda > present:d3d11(import cuda) [host 0, device 0]" (registered pool).
    std::string Summary() const;
};

enum class PlanDeltaKind { None, PresenterParams, FilterCommands, GraphRebuild, Reopen };

/// The cheapest live change from one plan to another. kind is the strongest change needed; commands / params are filled for
/// FilterCommands / PresenterParams (and are also filled alongside each other when both changed).
struct PlanDelta {
    PlanDeltaKind kind = PlanDeltaKind::None;
    std::vector<FilterCommand> commands;   // eq@color live commands (like FilterManager::Flush)
    PresenterParams params;
    std::string reason;
};

class PipelinePlanner {
public:
    explicit PipelinePlanner(CapabilitySet caps);

    Plan Make(const PlanRequest& req) const;

    /// Reopen: hwaccel, hwframesKeep, presenter or filterHwDevice differ. GraphRebuild: vfStructure differs.
    /// FilterCommands: same structure, eq@color values differ. PresenterParams: only presenterParams differ.
    static PlanDelta Diff(const Plan& before, const Plan& after);

    static double DecodeLoad(const ContentInfo& c);

    /// Raw/advanced chain tokenizer: top-level ',' split honouring '\' escapes, '...' quotes and [...] labels. Returns nullopt
    /// for a complex graph (';' or a [label] at top level) which is then treated as one opaque System block.
    static std::optional<std::vector<std::wstring>> TokenizeChain(const std::wstring& chain);
    static std::string TokenFilterName(const std::wstring& token);

    /// The ffp_open options for a plan: hwaccel (the device type, or "off" for software), vf, filter_hw_device when needed,
    /// hwframes=keep when hwframesKeep (hwframes=download for a hw decoder without keep on the d3d11 presenter, whose default
    /// is keep), presenter=<name> when not "sdl". Legacy-policy plans emit no hwaccel/hwframes/presenter.
    static OpenOptions OpenOptionsFor(const Plan& p, PlanPolicy policy);

private:
    CapabilitySet caps_;
    PipelineTable table_;
};

}
