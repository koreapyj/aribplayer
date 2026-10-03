#include "wvp/FilterManager.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cwctype>

namespace wvp {

namespace {

double ClampD(double v, double lo, double hi, double fallback) {
    if (std::isnan(v)) return fallback;
    return std::clamp(v, lo, hi);
}

std::wstring NumFixed(double d, int decimals) {
    char buf[400];
    auto r = std::to_chars(buf, buf + sizeof buf, d, std::chars_format::fixed, decimals);
    std::string s(buf, r.ptr);
    if (s.find('.') != std::string::npos) {
        while (!s.empty() && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
    }
    if (s == "-0") s = "0";
    return std::wstring(s.begin(), s.end());
}

std::wstring Trim(const std::wstring& s) {
    size_t a = 0, b = s.size();
    while (a < b && iswspace(s[a])) a++;
    while (b > a && iswspace(s[b - 1])) b--;
    return s.substr(a, b - a);
}

void Join(std::wstring& out, const std::wstring& part) {
    if (!out.empty()) out += L',';
    out += part;
}

}

std::wstring FilterManager::Num(double d) { return NumFixed(d, 6); }

FilterManager::FilterManager() {
    lastVfStructure_ = Compose(true, true);
    lastAfStructure_ = Compose(false, true);
}

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
    s.brightness = ClampD(s.brightness, kBrightnessMin, kBrightnessMax, kBrightnessDefault);
    s.contrast = ClampD(s.contrast, kContrastMin, kContrastMax, kContrastDefault);
    s.saturation = ClampD(s.saturation, kSaturationMin, kSaturationMax, kSaturationDefault);
    s.gamma = ClampD(s.gamma, kGammaMin, kGammaMax, kGammaDefault);
    switch (s.rotation) {
    case Rotation::None: case Rotation::Cw90: case Rotation::Rotate180: case Rotation::Ccw90: break;
    default: s.rotation = Rotation::None;
    }
    for (double& g : s.eqGainsDb) g = ClampD(g, kEqGainMinDb, kEqGainMaxDb, 0);
    s.speed = ClampD(s.speed, kMinSpeed, kMaxSpeed, kDefaultSpeed);
    s.customVideoFilter = Trim(s.customVideoFilter);
    s.customAudioFilter = Trim(s.customAudioFilter);
    return s;
}

bool FilterManager::EqActive() const {
    return !s_.advancedMode && (s_.brightness != 0 || s_.contrast != 1 || s_.saturation != 1 || s_.gamma != 1);
}

std::wstring FilterManager::Compose(bool video, bool structure) const {
    std::wstring out;
    if (!s_.advancedMode) {
        if (video) {
            // Ivtc is one deinterlace operation (no extra bwdif/yadif). CPU recipe: fieldmatch,
            // bwdif deint=interlaced for residual combing, decimate last (FFmpeg's recommended order).
            if (s_.deinterlace == DeinterlaceMode::Ivtc) {
                Join(out, L"fieldmatch");
                Join(out, L"bwdif=mode=send_frame:parity=auto:deint=interlaced");
                Join(out, L"decimate");
            }
            if (s_.deinterlace == DeinterlaceMode::On) {
                const wchar_t* name = s_.deinterlacePriority == DeinterlacePriority::Quality ? L"bwdif" : L"yadif";
                const wchar_t* mode = s_.deinterlaceRate == DeinterlaceRate::Field ? L"send_field" : L"send_frame";
                const wchar_t* deint = s_.deinterlaceOnlyInterlaced ? L"interlaced" : L"all";
                Join(out, std::wstring(name) + L"=mode=" + mode + L":parity=auto:deint=" + deint);
            }
            // Rotation + flips: 180 == hflip+vflip, XOR with the explicit flips.
            bool h = s_.flipHorizontal, v = s_.flipVertical;
            if (s_.rotation == Rotation::Rotate180) { h = !h; v = !v; }
            if (s_.rotation == Rotation::Cw90) Join(out, L"transpose=1");
            else if (s_.rotation == Rotation::Ccw90) Join(out, L"transpose=2");
            if (h) Join(out, L"hflip");
            if (v) Join(out, L"vflip");
            if (EqActive()) {
                if (structure) Join(out, std::wstring(kEqInstance) + L"=*");
                else Join(out, std::wstring(kEqInstance) + L"=brightness=" + Num(s_.brightness) + L":contrast=" + Num(s_.contrast) +
                                   L":saturation=" + Num(s_.saturation) + L":gamma=" + Num(s_.gamma));
            }
            if (s_.speed != 1) Join(out, L"setpts=PTS/" + Num(s_.speed));
        } else {
            if (s_.normalize) Join(out, L"dynaudnorm");
            if (std::any_of(s_.eqGainsDb.begin(), s_.eqGainsDb.end(), [](double g) { return g != 0; })) {
                auto lin = MapEqualizer(s_.eqGainsDb);
                std::wstring eq = L"superequalizer=";
                for (size_t i = 0; i < lin.size(); i++) {
                    if (i > 0) eq += L':';
                    eq += std::to_wstring(i + 1) + L"b=" + NumFixed(lin[i], 4);
                }
                Join(out, eq);
            }
            if (s_.speed != 1) for (auto& p : AtempoChain(s_.speed)) Join(out, p);
        }
    }
    const std::wstring& raw = video ? s_.customVideoFilter : s_.customAudioFilter;
    if (!raw.empty()) Join(out, raw);
    return out;
}

FilterChange FilterManager::Flush(const Plan* before) {
    std::wstring vfStruct = Compose(true, true), afStruct = Compose(false, true);
    FilterChange c;
    c.videoRebuild = vfStruct != lastVfStructure_;
    c.audioRebuild = afStruct != lastAfStructure_;
    if (!c.videoRebuild && EqActive()) {
        auto cmd = [&](const wchar_t* name, double now, double was) {
            if (now != was) c.commands.push_back({ kEqInstance, name, Num(now) });
        };
        cmd(L"brightness", s_.brightness, lastB_);
        cmd(L"contrast", s_.contrast, lastC_);
        cmd(L"saturation", s_.saturation, lastS_);
        cmd(L"gamma", s_.gamma, lastG_);
    }
    lastVfStructure_ = std::move(vfStruct);
    lastAfStructure_ = std::move(afStruct);
    lastB_ = s_.brightness; lastC_ = s_.contrast; lastS_ = s_.saturation; lastG_ = s_.gamma;
    if (planner_ && before) {
        c.videoRebuild = false;
        c.commands.clear();
        if (!before->ok || !plan_.ok) {
            c.reopen = true;
            c.videoRebuild = true;
        } else {
            PlanDelta d = PipelinePlanner::Diff(*before, plan_);
            c.videoRebuild = d.kind >= PlanDeltaKind::GraphRebuild;
            c.reopen = d.kind == PlanDeltaKind::Reopen;
            c.presenterParamsChanged = !(before->presenterParams == plan_.presenterParams);
            c.presenterParams = plan_.presenterParams;
            if (!c.videoRebuild) c.commands = std::move(d.commands);
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
    r.presenter = presenter_;
    r.policy = PlanPolicy::Cost;
    r.blacklist = blacklist_;
    return r;
}

void FilterManager::Replan() {
    if (!planner_) { plan_ = Plan{}; return; }
    plan_ = planner_->Make(Request());
}

FilterChange FilterManager::Commit() {
    if (!planner_) return Flush();
    Plan before = std::move(plan_);
    Replan();
    return Flush(&before);
}

Plan FilterManager::LegacyPlanFor(const Plan& target) const {
    Plan p;
    p.ok = true;
    p.hwaccel = decoder_ == DecoderPref::Software ? "" : decoder_ == DecoderPref::Forced ? forcedDecoder_ : target.hwaccel;
    p.presenter = "sdl";
    p.videoFilter = Compose(true, false);
    p.vfStructure = Compose(true, true);
    return p;
}

FilterChange FilterManager::Apply(const FilterSettings& next) {
    s_ = Sanitize(next);
    return Commit();
}

FilterChange FilterManager::SetCapabilities(std::optional<CapabilitySet> caps) {
    if (!caps) {
        if (!planner_) return Flush();
        bool wasActive = PlannerActive();
        planner_.reset();
        plan_ = Plan{};
        FilterChange c = Flush();
        c.reopen = wasActive;   // back to the legacy open options
        c.videoRebuild = wasActive;
        return c;
    }
    bool wasActive = PlannerActive();
    Plan before = plan_;
    planner_ = std::make_shared<const PipelinePlanner>(std::move(*caps));
    Replan();
    if (!wasActive) before = LegacyPlanFor(plan_);
    return Flush(&before);
}

FilterChange FilterManager::SetPlanPreferences(DecoderPref decoder, std::string forcedDecoder, PresenterPref presenter) {
    decoder_ = decoder;
    forcedDecoder_ = std::move(forcedDecoder);
    presenter_ = presenter;
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

FilterChange FilterManager::PresenterParamsRejected(const PresenterParams& rejected) {
    // The presenter cannot apply what the plan absorbed: stop absorbing the first absorbed operation that is not yet excluded
    // (eq first: its limits are the most likely to differ from the caps; then rotation; then flips) and replan. Each call adds
    // a new key, so a rejection cannot loop.
    std::string key;
    auto fresh = [&](const char* k) { return !blacklist_.count(k); };
    if (rejected.eq && fresh("absorb:eq")) key = "absorb:eq";
    else if (rejected.rotation != 0 && fresh("absorb:rotate")) key = "absorb:rotate";
    else if ((rejected.flipH || rejected.flipV) && fresh("absorb:flip")) key = "absorb:flip";
    if (key.empty()) {
        FilterChange none;
        none.videoFilter = VideoFilter();
        none.audioFilter = AudioFilter();
        return none;
    }
    return Blacklist(key);
}

FilterChange FilterManager::ClearBlacklist() {
    blacklist_.clear();
    return Commit();
}

FilterChange FilterManager::Reset() { return Apply(FilterSettings{}); }

FilterChange FilterManager::SetIvtcTuning(const IvtcTuning& t) {
    tuning_.pixelFormat = Trim(t.pixelFormat).empty() ? L"nv12" : Trim(t.pixelFormat);
    tuning_.fieldOrder = std::clamp(t.fieldOrder, -1, 1);
    tuning_.combThreshold = ClampD(t.combThreshold, 0, 255, 9.0);
    tuning_.dupThreshold = ClampD(t.dupThreshold, 0, 100, 1.1);
    tuning_.sceneThreshold = ClampD(t.sceneThreshold, 0, 100, 15.0);
    return Commit();
}

FilterProvider FilterManager::Provider() {
    FilterProvider p;
    p.videoFilter = [this] { return VideoFilter(); };
    p.audioFilter = [this] { return AudioFilter(); };
    p.planOptions = [this]() -> OpenOptions {
        if (!PlannerActive()) return {};
        return PipelinePlanner::OpenOptionsFor(plan_, PlanPolicy::Cost);
    };
    p.planActive = [this] { return PlannerActive(); };
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

std::vector<std::wstring> FilterManager::AtempoChain(double speed) {
    std::vector<std::wstring> r;
    double remaining = speed;
    while (remaining > 2.0 + 1e-9) { r.push_back(L"atempo=2"); remaining /= 2.0; }
    while (remaining < 0.5 - 1e-9) { r.push_back(L"atempo=0.5"); remaining /= 0.5; }
    r.push_back(L"atempo=" + Num(remaining));
    return r;
}

std::wstring FilterManager::Escape(const std::wstring& value) {
    std::wstring o;
    o.reserve(value.size() + 4);
    for (wchar_t ch : value) {
        if (ch == L'\\' || ch == L'\'' || ch == L':' || ch == L',' || ch == L';' || ch == L'=' || ch == L'[' || ch == L']' ||
            ch == L' ' || ch == L'\t' || ch == L'\n' || ch == L'\r') o += L'\\';
        o += ch;
    }
    return o;
}

}
