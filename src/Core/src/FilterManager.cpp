#include "wvp/FilterManager.h"

#include <algorithm>
#include <charconv>
#include <cmath>

#include "wvp/Utf8.h"

namespace wvp {

namespace {

double ClampD(double v, double lo, double hi, double fallback) {
    if (std::isnan(v)) return fallback;
    return std::clamp(v, lo, hi);
}

std::string NumFixed(double d, int decimals) {
    char buf[400];
    auto r = std::to_chars(buf, buf + sizeof buf, d, std::chars_format::fixed, decimals);
    std::string s(buf, r.ptr);
    if (s.find('.') != std::string::npos) {
        while (!s.empty() && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
    }
    if (s == "-0") s = "0";
    return s;
}

std::string Trim(const std::string& s) { return std::string(utf8::Trim(s)); }

void Join(std::string& out, const std::string& part) {
    if (!out.empty()) out += ',';
    out += part;
}

}

std::string FilterManager::Num(double d) { return NumFixed(d, 6); }

FilterManager::FilterManager() { lastAf_ = ComposeAudio(); }

FilterSettings FilterManager::Sanitize(FilterSettings s) {
    using namespace filters;
    if (s.deinterlace != DeinterlaceMode::Off && s.deinterlace != DeinterlaceMode::On && s.deinterlace != DeinterlaceMode::Ivtc)
        s.deinterlace = DeinterlaceMode::Off;
    if (s.deinterlacePriority != DeinterlacePriority::Efficiency && s.deinterlacePriority != DeinterlacePriority::Quality)
        s.deinterlacePriority = DeinterlacePriority::Efficiency;
    if (s.deinterlaceRate != DeinterlaceRate::Frame && s.deinterlaceRate != DeinterlaceRate::Field)
        s.deinterlaceRate = DeinterlaceRate::Frame;
    if (s.deinterlace == DeinterlaceMode::Ivtc) {   // rate / only-interlaced apply to On only
        s.deinterlaceRate = DeinterlaceRate::Frame;
        s.deinterlaceOnlyInterlaced = false;
    }
    for (double& g : s.eqGainsDb) g = ClampD(g, kEqGainMinDb, kEqGainMaxDb, 0);
    s.speed = ClampD(s.speed, kMinSpeed, kMaxSpeed, kDefaultSpeed);
    s.customVideoFilter = Trim(s.customVideoFilter);
    s.customAudioFilter = Trim(s.customAudioFilter);
    return s;
}

std::string FilterManager::ComposeAudio() const {
    std::string out;
    if (!s_.advancedMode) {
        if (s_.normalize) Join(out, "dynaudnorm");
        if (std::any_of(s_.eqGainsDb.begin(), s_.eqGainsDb.end(), [](double g) { return g != 0; })) {
            auto lin = MapEqualizer(s_.eqGainsDb);
            std::string eq = "superequalizer=";
            for (size_t i = 0; i < lin.size(); i++) {
                if (i > 0) eq += ':';
                eq += std::to_string(i + 1) + "b=" + NumFixed(lin[i], 4);
            }
            Join(out, eq);
        }
        if (s_.speed != 1) for (auto& p : AtempoChain(s_.speed)) Join(out, p);
    }
    if (!s_.customAudioFilter.empty()) Join(out, s_.customAudioFilter);
    return out;
}

FilterChange FilterManager::Flush(const Plan& before) {
    std::string af = ComposeAudio();
    FilterChange c;
    c.audioRebuild = af != lastAf_;
    lastAf_ = std::move(af);
    if (planner_) {
        if (!before.ok || !plan_.ok) {
            // a plan that appears is opened with; one that vanishes leaves a running item alone (the host reports PlanError)
            c.reopen = !before.ok && plan_.ok;
            c.videoRebuild = c.reopen;
        } else {
            PlanDelta d = PipelinePlanner::Diff(before, plan_);
            c.videoRebuild = d.kind >= PlanDeltaKind::GraphRebuild;
            c.reopen = d.kind == PlanDeltaKind::Reopen;
        }
    }
    c.videoFilter = VideoFilter();
    c.audioFilter = AudioFilter();
    return c;
}

PlanRequest FilterManager::Request() const {
    PlanRequest r;
    r.settings = s_;
    r.ivtc = tuning_;
    r.content = content_;
    r.decoder = decoder_;
    r.forcedDecoder = forcedDecoder_;
    r.blacklist = blacklist_;
    return r;
}

void FilterManager::Replan() {
    if (!planner_) { plan_ = Plan{}; return; }
    plan_ = planner_->Make(Request());
}

FilterChange FilterManager::Commit() {
    Plan before = std::move(plan_);
    Replan();
    return Flush(before);
}

FilterChange FilterManager::Apply(const FilterSettings& next) {
    s_ = Sanitize(next);
    return Commit();
}

FilterChange FilterManager::SetCapabilities(CapabilitySet caps) {
    Plan before = std::move(plan_);
    planner_ = std::make_shared<const PipelinePlanner>(std::move(caps));
    Replan();
    return Flush(before);
}

FilterChange FilterManager::SetPlanPreferences(DecoderPref decoder, std::string forcedDecoder) {
    decoder_ = decoder;
    forcedDecoder_ = std::move(forcedDecoder);
    return Commit();
}

FilterChange FilterManager::SetContent(const ContentInfo& content) {
    content_ = content;
    return Commit();
}

FilterChange FilterManager::Blacklist(const std::string& key) {
    blacklist_.insert(key);
    return Commit();
}

FilterChange FilterManager::ClearBlacklist() {
    blacklist_.clear();
    return Commit();
}

FilterChange FilterManager::Reset() { return Apply(FilterSettings{}); }

FilterChange FilterManager::SetIvtcTuning(const IvtcTuning& t) {
    tuning_.pixelFormat = Trim(t.pixelFormat).empty() ? "nv12" : Trim(t.pixelFormat);
    tuning_.fieldOrder = std::clamp(t.fieldOrder, -1, 1);
    tuning_.combThreshold = ClampD(t.combThreshold, 0, 255, 9.0);
    tuning_.dupThreshold = ClampD(t.dupThreshold, 0, 100, 1.1);
    tuning_.sceneThreshold = ClampD(t.sceneThreshold, 0, 100, 15.0);
    return Commit();
}

FilterProvider FilterManager::Provider() {
    FilterProvider p;
    p.audioFilter = [this] { return AudioFilter(); };
    p.planOptions = [this]() -> std::optional<OpenOptions> {
        if (!PlannerActive()) return std::nullopt;
        return PipelinePlanner::OpenOptionsFor(plan_);
    };
    return p;
}

std::array<double, 18> FilterManager::MapEqualizer(const std::array<double, 10>& g) {
    std::array<double, 18> res{};
    for (size_t i = 0; i < res.size(); i++) {
        double fr = kSuperEqHz[i], db;
        if (fr <= kEqBandsHz.front()) db = g.front();
        else if (fr >= kEqBandsHz.back()) db = g.back();
        else {
            size_t k = 0;
            while (fr > kEqBandsHz[k + 1]) k++;
            double t = (std::log(fr) - std::log((double)kEqBandsHz[k])) / (std::log((double)kEqBandsHz[k + 1]) - std::log((double)kEqBandsHz[k]));
            db = g[k] + t * (g[k + 1] - g[k]);
        }
        res[i] = std::nearbyint(std::pow(10.0, db / 20.0) * 1e4) / 1e4;
    }
    return res;
}

std::vector<std::string> FilterManager::AtempoChain(double speed) {
    std::vector<std::string> r;
    double remaining = speed;
    while (remaining > 2.0 + 1e-9) { r.push_back("atempo=2"); remaining /= 2.0; }
    while (remaining < 0.5 - 1e-9) { r.push_back("atempo=0.5"); remaining /= 0.5; }
    r.push_back("atempo=" + Num(remaining));
    return r;
}

std::string FilterManager::Escape(const std::string& value) {
    std::string o;
    o.reserve(value.size() + 4);
    for (char ch : value) {
        if (ch == '\\' || ch == '\'' || ch == ':' || ch == ',' || ch == ';' || ch == '=' || ch == '[' || ch == ']' ||
            ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') o += '\\';
        o += ch;
    }
    return o;
}

}
