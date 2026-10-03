#pragma once
// No Win32 / Direct2D here: everything is tested by --logictest (DebugTests.cpp).
//
// Every value shown is what libffplay observed at runtime (decoder object, hw device really created, graph really
// configured, renderer object really created). Where the app asked for something, requested and actual are shown side by
// side and a difference is a Warn line (drawn in the theme's caution colour).
#include <optional>
#include <string>
#include <vector>

#include "ShellLogic.h"
#include "wvp/DebugInfo.h"
#include "wvp/PipelinePlanner.h"

namespace app::dbg {

enum class LineKind { Header, Normal, Warn, Dim };

struct OverlayLine {
    LineKind kind = LineKind::Normal;
    std::string text;   // UTF-8
    bool operator==(const OverlayLine&) const = default;
};

struct OverlayContext {
    bool haveJson = false;          // ffp_get_debug_info returned something
    wvp::DebugInfo info;            // parsed (info.Valid false when !haveJson or unparsable)
    std::string mode;               // "now playing" / "full screen" / "mini player"
    std::string resumeDecision;
    std::string url;
    std::string openOptions;        // compact "k=v k=v" line of the options passed to ffp_open
    std::string ffplayCmd;
    double requestedSpeed = 0;      // FilterManager speed; 0 = unknown (info.Speed is used)
    std::string deinterlaceRequested;   // requested Deinterlace mode: "", "off", "deinterlace" (On), "ivtc"
    std::string adapterNote;        // video GPU, display adapter, why, runner-up ("video GPU NVIDIA ... (luid ...), display ..."); "" = unknown
    bool adapterWarn = false;       // cross-adapter copy, missing manual GPU or no hardware adapter
    std::string buildStamp;         // "aribplayer.exe built ... | libffplay ..." (stale-exe detection); "" = unknown
    std::string acrylicFallbackReason; // empty when in-app Acrylic is active; otherwise the solid-fallback reason
    double displayRefreshHz = 0;    // refresh rate of the monitor hosting the window (disp::RefreshRateHz); 0 = unknown
    std::optional<wvp::Plan> plan;  // the pipeline planner's plan for the current playback; nullopt = no pipeline section
};

/// Filter names of a filter graph string ("buffer=...,yadif,scale=...") at top level: labels ([in]), options (=...),
/// instance names (@x) removed.
std::vector<std::string> GraphFilterNames(const std::string& graph);
struct GraphCompare {
    bool known = false;                     // the actual graph was reported
    bool mismatch = false;                  // a requested filter is not (in order) in the actual graph
    std::vector<std::string> missing;
    std::vector<std::string> autoInserted;  // actual, not requested, not source/sink (scale, format, aresample, ...)
};
GraphCompare CompareGraph(const std::string& requested, const std::vector<std::string>& actualNames);
/// Product of every atempo value in the audio graph (1.0 when none), `any` = at least one atempo present.
double AtempoProduct(const std::string& afGraph, bool* any = nullptr);
/// One row for the single Deinterlace option: "IVTC: ivtc_cuda (cuda), residual-combing fallback unavailable" |
/// "IVTC: ivtc_d3d11 (d3d11va)" | "IVTC: fieldmatch+bwdif(interlaced)+decimate (CPU)" | "bwdif (CPU)" | "none" ...;
/// for On the planner's reason follows: "yadif (CPU); priority efficiency: no GPU deinterlacer -> yadif".
/// `warn` when the requested mode is not in the actual graph.
std::string DescribeIvtcDeint(const OverlayContext& c, bool* warn = nullptr);

/// Warm-up: displayed fps is only compared once this many frames were displayed.
constexpr long long kFpsWarmupFrames = 40;
/// Tolerated relative difference between displayed and filter-out fps (10 %).
constexpr double kFpsTolerance = 0.10;
/// True when the measured displayed fps differs from the filter-out fps by more than kFpsTolerance after warm-up
/// (unknown values and a paused player are never a mismatch).
bool FpsMismatch(const wvp::DebugFps& f, bool paused);
struct Cadence {
    enum class Kind { Unknown, Even, Pulldown32, Uneven, FasterThanRefresh } kind = Kind::Unknown;
    double framesPerRefresh = 0;   // refresh / fps
    std::string text;              // "even 2:2 (60 Hz / 30.00 fps)", "3:2 cadence (judder) ...", ""
};
/// How `fps` frames per second land on a `refreshHz` display: even (integer ratio), 3:2 (ratio ~ n + 0.5, e.g. 23.976 on
/// 60 Hz), uneven (any other ratio) or faster than the refresh (frames skipped).
Cadence DescribeCadence(double fps, double refreshHz);
/// The "fps" overlay rows: "in A -> filter out B -> displayed C (refresh R Hz)", decoded / counters / drops, cadence note.
/// The first row is Warn when FpsMismatch. A missing fps object gives a single row with "?" values.
std::vector<OverlayLine> BuildFpsLines(const wvp::DebugInfo& d, double refreshHz);

std::vector<OverlayLine> BuildOverlayLines(const OverlayContext& c);
/// Columns count UTF-8 code points; a logical line is capped at 1 + maxContinuation rows, ending in "...".
std::vector<OverlayLine> WrapLines(const std::vector<OverlayLine>& lines, int maxCols, int maxContinuation = 3);
/// At most maxRows rows; the last kept row becomes "... (N more)" when rows were dropped.
std::vector<OverlayLine> ClipRows(const std::vector<OverlayLine>& lines, int maxRows);
int Utf8Len(const std::string& s);
std::string PlainText(const std::vector<OverlayLine>& lines);

struct OverlayGeometry {
    float x = 0, y = 0, w = 0, h = 0;   // device pixels, client coordinates
    int cols = 0, rows = 0;
    bool visible = false;
};
/// Top-left below the title strip (Now playing) or at the top (full screen / mini), 8 DIP margin, 6 DIP padding, at most
/// kMaxCols columns wide; the bottom `bottomReservedPx` (the bar) stays free.
constexpr int kMaxCols = 112;
OverlayGeometry ComputeOverlayGeometry(int clientWpx, int clientHpx, double scale, ShellMode mode, float charWpx, float lineHpx,
                                       int bottomReservedPx);
constexpr float kMarginDip = 8, kPadDip = 6;

enum class TabAction { ToggleOverlay, FocusTraversal, Ignore };
/// Player views have no keyboard focus: Tab toggles the overlay there (Shift+Tab does nothing) unless a popup/flyout is open;
/// popups keep normal focus traversal.
TabAction DecideTab(bool popupOpen, bool shift);

}
