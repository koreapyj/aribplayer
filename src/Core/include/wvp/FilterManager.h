#pragma once
// Composes presets and raw strings into one video filter string and one audio filter string in a deterministic order:
//   video: deinterlace (On: yadif for priority Efficiency, bwdif for Quality; or ivtc), rotate/flip, eq@color, setpts (speed), then the custom raw string
//   audio: dynaudnorm, superequalizer, atempo chain (speed), then the custom raw string
// Raw mode: custom strings are APPENDED to the presets; with advancedMode they REPLACE the presets.
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
    static constexpr const wchar_t* kEqInstance = filters::kEqInstance;

    FilterManager();

    const FilterSettings& Settings() const { return s_; }

    /// Clamps `next`, makes it the current state and returns the minimal live change: only eq commands for
    /// eq-value-only changes (while eq stays present), rebuild flags + new strings for structural changes,
    /// an all-false/empty change for no-ops. videoFilter/audioFilter in the result always hold the current strings.
    FilterChange Apply(const FilterSettings& next);

    /// Back to defaults (presets and raw strings). IVTC tuning is kept.
    FilterChange Reset();

    std::wstring VideoFilter() const { return PlannerActive() ? plan_.videoFilter : Compose(true, false); }
    std::wstring AudioFilter() const { return Compose(false, false); }

    /// Provider for PlaybackController::SetFilters. Captures `this`: the manager must outlive the controller use.
    FilterProvider Provider();

    const IvtcTuning& Ivtc() const { return tuning_; }
    FilterChange SetIvtcTuning(const IvtcTuning& t);

    // Without capabilities, uses legacy filter composition.
    // With capabilities every setter replans (PlanPolicy::Cost) and returns a FilterChange whose video part comes from
    // PipelinePlanner::Diff(old, new): videoRebuild for a graph rebuild, reopen (+ videoRebuild) when the decoder, presenter,
    // hwframes mode or filter device changed, presenterParamsChanged when only presenter-applied parameters changed.
    // Switching legacy <-> planner mode compares against the legacy plan (reopen when the open options differ).
    FilterChange SetCapabilities(std::optional<CapabilitySet> caps);
    FilterChange SetPlanPreferences(DecoderPref decoder, std::string forcedDecoder, PresenterPref presenter);
    FilterChange SetContent(const ContentInfo& content);
    /// Adds a runtime-failure key ("hwaccel:cuda", "filter:ivtc_cuda", "hwframes_keep", ...) and replans.
    FilterChange Blacklist(const std::string& key);
    /// The presenter refused `rejected` (SetPresenterParams error): blacklists "absorb:eq", "absorb:rotate" or "absorb:flip" (first
    /// absorbed one not yet excluded) so that operation moves into the graph, and returns the resulting change. A no-op change
    /// when nothing is left to exclude.
    FilterChange PresenterParamsRejected(const PresenterParams& rejected);
    FilterChange ClearBlacklist();
    /// The current plan; nullptr in legacy mode (no caps, or the planner found no valid pipeline).
    const Plan* CurrentPlan() const { return PlannerActive() ? &plan_ : nullptr; }
    /// The request the planner is given (settings, tuning, content, preferences, blacklist).
    PlanRequest Request() const;
    bool PlannerActive() const { return planner_ && plan_.ok; }
    const ContentInfo& Content() const { return content_; }

    static FilterSettings Sanitize(FilterSettings s);
    /// 10 UI bands (dB) -> superequalizer's 18 band multipliers (log-frequency interpolation, 4 decimals).
    static std::array<double, 18> MapEqualizer(const std::array<double, 10>& gainsDb);
    /// atempo only accepts 0.5..2 reliably: other speeds are split into a chain.
    static std::vector<std::wstring> AtempoChain(double speed);
    /// Backslash-escapes \ ' : , ; = [ ] and whitespace for use as a filter option value.
    static std::wstring Escape(const std::wstring& value);
    /// Culture-invariant "0.######" number formatting.
    static std::wstring Num(double d);

private:
    std::wstring Compose(bool video, bool structure) const;
    bool EqActive() const;
    FilterChange Flush(const Plan* before = nullptr);
    FilterChange Commit();   // replans (planner mode) and flushes against the previous plan
    void Replan();
    Plan LegacyPlanFor(const Plan& target) const;

    FilterSettings s_;
    IvtcTuning tuning_;
    std::wstring lastVfStructure_, lastAfStructure_;
    double lastB_ = 0, lastC_ = 1, lastS_ = 1, lastG_ = 1;

    std::shared_ptr<const PipelinePlanner> planner_;   // null = legacy
    Plan plan_;
    DecoderPref decoder_ = DecoderPref::Auto;
    std::string forcedDecoder_;
    PresenterPref presenter_ = PresenterPref::Auto;
    ContentInfo content_;
    std::set<std::string> blacklist_;
};

}
