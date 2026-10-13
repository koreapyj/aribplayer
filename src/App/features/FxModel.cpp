#include "features/FxModel.h"
#include "Localize.h"
#include "wvp/win32/Utf.h"

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <cctype>
#include <cwctype>

namespace app::fx {

using wvp::FilterSettings;
using wvp::FilterChange;
namespace F = wvp::filters;

const std::vector<EqPreset>& EqPresets() {
    static const std::vector<EqPreset> p = {
        {Tr(L"Flat", L"평탄"),         {0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
        {Tr(L"Bass boost", L"저음 강조"),   {6, 6, 5, 3, 1, 0, 0, 0, 0, 0}},
        {Tr(L"Treble boost", L"고음 강조"), {0, 0, 0, 0, 0, 1, 3, 5, 6, 6}},
        {Tr(L"Vocal", L"보컬"),        {-2, -2, -1, 1, 3, 4, 3, 1, 0, -1}},
        {Tr(L"Rock", L"록"),         {5, 4, 2, -1, -2, 0, 2, 4, 5, 5}},
        {Tr(L"Pop", L"팝"),          {-1, 1, 3, 4, 3, 0, -1, -1, 1, 2}},
        {Tr(L"Classical", L"클래식"),    {0, 0, 0, 0, 0, 0, -2, -3, -3, -4}},
        {Tr(L"Loudness", L"라우드니스"),     {6, 4, 0, 0, -1, 0, -1, 0, 4, 2}},
    };
    return p;
}

int MatchEqPreset(const std::array<double, F::kEqBandCount>& g) {
    const auto& ps = EqPresets();
    for (size_t i = 0; i < ps.size(); i++) {
        bool same = true;
        for (size_t b = 0; b < g.size(); b++) if (std::fabs(g[b] - ps[i].gainsDb[b]) > 0.01) { same = false; break; }
        if (same) return (int)i;
    }
    return -1;
}

int SpeedIndex(double speed) {
    for (size_t i = 0; i < F::kSpeedPresets.size(); i++) if (std::fabs(F::kSpeedPresets[i] - speed) < 1e-6) return (int)i;
    return -1;
}

std::wstring SpeedLabel(double speed) { return wvp::ToWide(wvp::FilterManager::Num(speed)) + L"x"; }

std::wstring BandLabel(int hz) {
    if (hz >= 1000) return std::to_wstring(hz / 1000) + L" kHz";
    return std::to_wstring(hz) + L" Hz";
}

std::wstring Fixed(double v, int decimals) {
    wchar_t buf[64];
    swprintf(buf, 64, L"%.*f", decimals, v);
    return buf;
}

std::wstring DbLabel(double db) {
    int v = (int)std::lround(db);
    return std::wstring(v > 0 ? L"+" : L"") + std::to_wstring(v) + L" dB";
}

DeinterlaceControls DeinterlaceControlsFor(wvp::DeinterlaceMode mode, bool progressiveContent) {
    DeinterlaceControls c;
    c.enabled = !progressiveContent;
    c.priority = c.rate = c.onlyInterlaced = mode == wvp::DeinterlaceMode::On;
    c.ivtcTuning = mode == wvp::DeinterlaceMode::Ivtc;
    return c;
}

const std::vector<std::wstring>& DeinterlaceLabels() {
    static const std::vector<std::wstring> v = { Tr(L"Off", L"끄기"), Tr(L"On", L"켜기"), L"IVTC" };
    return v;
}

const std::vector<std::wstring>& DeinterlacePriorityLabels() {
    static const std::vector<std::wstring> v = { Tr(L"Efficiency", L"효율"), Tr(L"Quality", L"품질") };
    return v;
}

FilterSettings ResetVideoPart(FilterSettings s) {
    FilterSettings d;
    s.deinterlace = d.deinterlace; s.deinterlaceRate = d.deinterlaceRate; s.deinterlaceOnlyInterlaced = d.deinterlaceOnlyInterlaced;
    s.deinterlacePriority = d.deinterlacePriority;
    return s;
}

FilterSettings ResetAudioPart(FilterSettings s) {
    FilterSettings d;
    s.normalize = d.normalize; s.eqGainsDb = d.eqGainsDb; s.speed = d.speed;
    return s;
}

FilterSettings WithEqPreset(FilterSettings s, int idx) {
    const auto& ps = EqPresets();
    if (idx >= 0 && idx < (int)ps.size()) s.eqGainsDb = ps[(size_t)idx].gainsDb;
    return s;
}

bool HasEffect(const FilterChange& c) {
    return c.videoRebuild || c.audioRebuild || c.reopen;
}

ZoomState& Zoom() { static ZoomState z; return z; }

std::string ZoomCropFilter(double a) {
    if (!(a > 0.05) || !std::isfinite(a)) a = 16.0 / 9.0;
    std::string A = wvp::FilterManager::Num(std::round(a * 10000) / 10000);
    return std::string(kZoomInstance) + "=w=min(iw\\,ih*" + A + "/sar):h=min(ih\\,iw*sar/" + A + ")";
}

std::string StripZoom(const std::string& vf) {
    std::string key = std::string(kZoomInstance) + "=";
    size_t p = vf.find(key);
    if (p == std::string::npos) return vf;
    size_t e = p + key.size();
    while (e < vf.size()) {
        if (vf[e] == '\\') { e += 2; continue; }
        if (vf[e] == ',') break;
        e++;
    }
    e = std::min(e, vf.size());
    size_t b = p;
    if (e < vf.size()) e++;
    else if (b > 0 && vf[b - 1] == ',') b--;
    return vf.substr(0, b) + vf.substr(e);
}

std::string ComposeCustomVideo(const std::string& user, bool zoomOn, double aspect) {
    std::string u = StripZoom(user);
    if (!zoomOn) return u;
    return u.empty() ? ZoomCropFilter(aspect) : u + "," + ZoomCropFilter(aspect);
}

double VideoAreaAspect(float w, float h, float strip, bool full) {
    if (!full) h -= strip;
    if (w < 1 || h < 1) return 16.0 / 9.0;
    return (double)w / (double)h;
}

FilterSettings UserView(FilterSettings s) {
    s.customVideoFilter = StripZoom(s.customVideoFilter);
    return s;
}

FilterChange ApplyUser(wvp::FilterManager& fm, FilterSettings user, const ZoomState& z) {
    user.customVideoFilter = ComposeCustomVideo(user.customVideoFilter, z.on, z.aspect);
    return fm.Apply(user);
}

bool IsCycleableSubtitle(const wvp::StreamInfo& stream) {
    return stream.Type == wvp::StreamType::Subtitle && stream.AribType != "superimpose";
}

std::vector<wvp::StreamInfo> CycleableSubtitles(const std::vector<wvp::StreamInfo>& streams) {
    std::vector<wvp::StreamInfo> cycleable;
    cycleable.reserve(streams.size());
    for (const auto& stream : streams) if (IsCycleableSubtitle(stream)) cycleable.push_back(stream);
    return cycleable;
}

bool ToggleSubtitleVisibility(const wvp::MediaInfo& media, bool& visible) {
    if (!std::any_of(media.Streams.begin(), media.Streams.end(), IsCycleableSubtitle)) return false;
    visible = !visible;
    return true;
}

std::optional<int> NextSubtitleIndex(const std::vector<wvp::StreamInfo>& streams) {
    const auto subs = CycleableSubtitles(streams);
    if (subs.empty()) return std::nullopt;
    size_t cur = subs.size();
    for (size_t i = 0; i < subs.size(); i++) if (subs[i].Selected) { cur = i; break; }
    if (cur == subs.size()) return subs.front().Index;
    return cur + 1 < subs.size() ? subs[cur + 1].Index : -1;
}

TrackRef CaptureTrackRef(const std::vector<wvp::StreamInfo>& streams, bool subtitle) {
    TrackRef ref;
    int pathOrdinal = 0;
    std::string lastPath;
    for (const auto& s : streams) {
        if (s.external) {
            if (s.path != lastPath) { lastPath = s.path; pathOrdinal = 0; } else pathOrdinal++;
        }
        if (!s.Selected) continue;
        ref.index = s.Index;
        if (s.external) { ref.kind = TrackRef::Kind::External; ref.path = s.path; ref.pathOrdinal = pathOrdinal; }
        else ref.kind = TrackRef::Kind::Embedded;
        return ref;
    }
    if (subtitle) ref.kind = TrackRef::Kind::Off;
    return ref;
}

std::optional<int> ResolveTrackRef(const std::vector<wvp::StreamInfo>& streams, const TrackRef& ref) {
    switch (ref.kind) {
    case TrackRef::Kind::Off: return -1;
    case TrackRef::Kind::Embedded:
        for (const auto& s : streams) if (!s.external && s.Index == ref.index) return s.Index;
        return std::nullopt;
    case TrackRef::Kind::External: {
        int ordinal = 0;
        for (const auto& s : streams) {
            if (!s.external || s.path != ref.path) continue;
            if (ordinal++ == ref.pathOrdinal) return s.Index;
        }
        return std::nullopt;
    }
    default: return std::nullopt;
    }
}

std::wstring LanguageName(const std::string& c) {
    struct L { const char* code; const wchar_t* name; };
    static const L t[] = {
        {"eng", Tr(L"English", L"영어")}, {"jpn", Tr(L"Japanese", L"일본어")}, {"fre", Tr(L"French", L"프랑스어")}, {"fra", Tr(L"French", L"프랑스어")}, {"ger", Tr(L"German", L"독일어")},
        {"deu", Tr(L"German", L"독일어")}, {"spa", Tr(L"Spanish", L"스페인어")}, {"ita", Tr(L"Italian", L"이탈리아어")}, {"kor", Tr(L"Korean", L"한국어")}, {"chi", Tr(L"Chinese", L"중국어")},
        {"zho", Tr(L"Chinese", L"중국어")}, {"por", Tr(L"Portuguese", L"포르투갈어")}, {"rus", Tr(L"Russian", L"러시아어")}, {"dut", Tr(L"Dutch", L"네덜란드어")}, {"nld", Tr(L"Dutch", L"네덜란드어")},
        {"swe", Tr(L"Swedish", L"스웨덴어")}, {"pol", Tr(L"Polish", L"폴란드어")}, {"tur", Tr(L"Turkish", L"터키어")}, {"ara", Tr(L"Arabic", L"아랍어")}, {"hin", Tr(L"Hindi", L"힌디어")},
    };
    std::string lc;
    for (char ch : c) lc.push_back((char)std::tolower((unsigned char)ch));
    if (lc.empty() || lc == "und") return L"";
    for (auto& e : t) if (lc == e.code) return e.name;
    std::wstring up;
    for (char ch : lc) up.push_back((wchar_t)std::toupper((unsigned char)ch));
    return up;
}

std::wstring TrackName(const wvp::StreamInfo& s, int ordinal) {
    if (!s.Title.empty()) return wvp::ToWide(s.Title);
    std::wstring l = LanguageName(s.Language);
    if (!l.empty()) return l;
    return Tr(L"Track ", L"트랙 ") + std::to_wstring(ordinal);
}

std::wstring SubtitleLabel(const wvp::StreamInfo& s, int ordinal) {
    if (!s.external) return TrackName(s, ordinal);
    std::wstring leaf = wvp::ToWide(s.path);
    if (size_t slash = leaf.find_last_of(L"\\/"); slash != std::wstring::npos) leaf.erase(0, slash + 1);
    if (leaf.empty()) return TrackName(s, ordinal);
    std::wstring info = !s.Title.empty() ? wvp::ToWide(s.Title) : LanguageName(s.Language);
    return info.empty() ? leaf : leaf + L" (" + info + L")";
}

void AribAudioNames(const wvp::StreamInfo& audio1, bool has1, const wvp::StreamInfo& audio2, bool has2,
                    std::wstring& name1, std::wstring& name2) {
    name1 = TrackName(has1 ? audio1 : wvp::StreamInfo{}, 1);
    name2 = TrackName(has2 ? audio2 : wvp::StreamInfo{}, 2);
    if (name1 == name2) {
        name1 += Tr(L" (Audio 1)", L" (소리 1)");
        name2 += Tr(L" (Audio 2)", L" (소리 2)");
    }
}

std::wstring TrackToast(wvp::StreamType type, const std::vector<wvp::StreamInfo>& streams) {
    bool sub = type == wvp::StreamType::Subtitle;
    if (streams.empty() || (type != wvp::StreamType::Audio && !sub)) return L"";
    std::wstring head = sub ? Tr(L"Subtitles: ", L"자막: ") : Tr(L"Audio: ", L"소리: ");
    for (size_t i = 0; i < streams.size(); i++) {
        if (streams[i].Selected)
            return head + (sub ? SubtitleLabel(streams[i], (int)i + 1) : TrackName(streams[i], (int)i + 1)) + L" (" + std::to_wstring(i + 1) + L"/" + std::to_wstring(streams.size()) + L")";
    }
    return sub ? head + Tr(L"Off", L"끄기") : L"";
}

}
