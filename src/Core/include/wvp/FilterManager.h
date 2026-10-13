#pragma once
// The filter settings and the pipeline plan made from them. Video: the PipelinePlanner turns the settings (deinterlace, speed,
// the custom raw string) into the plan once capabilities are set; there is no video filter without a plan. Audio: presets and
// the raw string composed in a deterministic order: dynaudnorm, superequalizer, atempo chain (speed), then the custom raw
// string (appended to the presets; with advancedMode it replaces them).
// Not thread-safe.
#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "FilterSettings.h"
#include "PipelinePlanner.h"
#include "PlaybackController.h"

namespace wvp {


class FilterManager {
public:
    static constexpr std::size_t kEqBandCount = filters::kEqBandCount;
    static constexpr std::array<int, 10> kEqBandsHz = { 31, 62, 125, 250, 500, 1000, 2000, 4000, 8000, 16000 };
    /// Centre frequencies of ffmpeg's superequalizer bands 1b..18b.
    static constexpr std::array<int, 18> kSuperEqHz = { 65, 92, 131, 185, 262, 370, 523, 740, 1047, 1480, 2093, 2960, 4186, 5920, 8372, 11840, 16744, 20000 };
    static constexpr double kMinSpeed = filters::kMinSpeed, kMaxSpeed = filters::kMaxSpeed;
    static constexpr std::array<double, 8> kSpeedPresets = filters::kSpeedPresets;

    FilterManager();

    const FilterSettings& Settings() const { return s_; }

    /// Clamps `next`, makes it the current state and returns the minimal live change: rebuild flags + new strings for
    /// changed strings, an all-false change for no-ops. videoFilter/audioFilter in the result always hold the current strings.
    FilterChange Apply(const FilterSettings& next);

    /// Back to defaults (presets and raw strings). IVTC tuning is kept.
    FilterChange Reset();

    /// The vf of the current plan ("" without a plan).
    std::string VideoFilter() const { return PlannerActive() ? plan_.videoFilter : std::string(); }
    std::string AudioFilter() const { return ComposeAudio(); }

    /// Provider for PlaybackController::SetFilters. Captures `this`: the manager must outlive the controller use.
    FilterProvider Provider();

    const IvtcTuning& Ivtc() const { return tuning_; }
    FilterChange SetIvtcTuning(const IvtcTuning& t);

    // Before capabilities are set there is no plan (and no video part in a FilterChange). With capabilities every setter
    // replans and returns a FilterChange whose video part comes from PipelinePlanner::Diff(old, new): videoRebuild for a graph
    // rebuild, reopen (+ videoRebuild) when the decoder, hwframes mode or filter device changed, or a plan appeared or vanished.
    FilterChange SetCapabilities(CapabilitySet caps);
    bool HasCapabilities() const { return planner_ != nullptr; }
    FilterChange SetContent(const ContentInfo& content);
    /// Adds a runtime-failure key ("hwaccel:d3d11va", "filter:ivtc_d3d11", "hwframes_keep", ...) and replans.
    FilterChange Blacklist(const std::string& key);
    FilterChange ClearBlacklist();
    /// The current plan; nullptr without capabilities or when the planner found no valid pipeline (PlanError says why).
    const Plan* CurrentPlan() const { return PlannerActive() ? &plan_ : nullptr; }
    /// Why there is no plan although capabilities are set; "" otherwise.
    std::string PlanError() const { return planner_ && !plan_.ok ? plan_.error : std::string(); }
    /// The request the planner is given (settings, tuning, content, blacklist).
    PlanRequest Request() const;
    bool PlannerActive() const { return planner_ && plan_.ok; }
    const ContentInfo& Content() const { return content_; }

    static FilterSettings Sanitize(FilterSettings s);
    /// 10 UI bands (dB) -> superequalizer's 18 band multipliers (log-frequency interpolation, 4 decimals).
    static std::array<double, 18> MapEqualizer(const std::array<double, 10>& gainsDb);
    /// atempo only accepts 0.5..2 reliably: other speeds are split into a chain.
    static std::vector<std::string> AtempoChain(double speed);
    /// Backslash-escapes \ ' : , ; = [ ] and whitespace for use as a filter option value.
    static std::string Escape(const std::string& value);
    /// Culture-invariant "0.######" number formatting.
    static std::string Num(double d);

private:
    std::string ComposeAudio() const;
    FilterChange Flush(const Plan& before);
    FilterChange Commit();   // replans and flushes against the previous plan
    void Replan();

    FilterSettings s_;
    IvtcTuning tuning_;
    std::string lastAf_;

    std::shared_ptr<const PipelinePlanner> planner_;   // null until capabilities are set
    Plan plan_;
    ContentInfo content_;
    std::set<std::string> blacklist_;
};

}
