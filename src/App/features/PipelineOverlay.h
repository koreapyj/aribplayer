#pragma once
// The "pipeline" section of the Tab debug overlay: the planner's Plan (decoder, steps with copy kinds, planned copy
// counts, presenter parameters, rejected alternatives) next to what libffplay measured, with a mismatch check. Pure model.
#include <string>
#include <vector>

#include "features/DebugOverlayModel.h"
#include "wvp/DebugInfo.h"
#include "wvp/PipelinePlanner.h"

namespace app::dbg {

/// Frame path counters measured by libffplay (frame_path_counts). known = at least one counter was reported.
struct MeasuredCopies {
    bool known = false;
    long long hwDirect = 0, hwDownload = 0, software = 0;
};
MeasuredCopies Measure(const wvp::DebugInfo& d);

struct PlanCheck {
    bool mismatch = false;
    std::vector<std::string> problems;
    std::vector<std::string> notes;        // observed improvements over the plan, not mismatches
};
/// Planned vs measured. Decoder / frame path / presenter checks apply after kFpsWarmupFrames counted frames.
PlanCheck CheckPlan(const wvp::Plan& p, const wvp::DebugInfo& d);

/// The "pipeline" section rows (see PipelineOverlay.cpp). At most 4 rejected alternatives, none when !showRejected.
std::vector<OverlayLine> BuildPlanLines(const wvp::Plan& p, const wvp::DebugInfo& d, bool showRejected = true);

}
