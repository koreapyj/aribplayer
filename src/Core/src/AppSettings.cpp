#include "wvp/AppSettings.h"

#include <algorithm>
#include <cmath>

#include "wvp/JsonLite.h"
#include "wvp/JsonWriter.h"
#include "wvp/Utf.h"

namespace wvp {

namespace {

using json::Value;

const Value* Member(const Value& o, std::string_view k, Value::Kind kind) {
    const Value* v = o.Find(k);
    return v && v->kind == kind ? v : nullptr;
}
void GetBool(const Value& o, std::string_view k, bool& out) { if (auto* v = Member(o, k, Value::Kind::Bool)) out = v->b; }
void GetNum(const Value& o, std::string_view k, double& out) {
    if (auto* v = Member(o, k, Value::Kind::Number)) if (std::isfinite(v->n)) out = v->n;
}
void GetInt(const Value& o, std::string_view k, int& out, int lo, int hi) {
    if (auto* v = Member(o, k, Value::Kind::Number)) if (std::isfinite(v->n)) out = (int)std::clamp(v->n, (double)lo, (double)hi);
}
void GetStr(const Value& o, std::string_view k, std::wstring& out) {
    if (auto* v = Member(o, k, Value::Kind::String)) out = ToWide(v->s);
}

const char* ThemeName(ThemeSetting t) {
    switch (t) { case ThemeSetting::Light: return "Light"; case ThemeSetting::Dark: return "Dark"; default: return "System"; }
}
const char* AlwaysOnTopName(AlwaysOnTop m) {
    switch (m) { case AlwaysOnTop::Enable: return "Enable"; case AlwaysOnTop::WhenPlaying: return "WhenPlaying"; case AlwaysOnTop::WhenFullscreen: return "WhenFullscreen"; default: return "Disable"; }
}
const char* DeintName(DeinterlaceMode m) {
    switch (m) { case DeinterlaceMode::On: return "on"; case DeinterlaceMode::Ivtc: return "ivtc"; default: return "off"; }
}

void WriteFilters(JsonWriter& w, const FilterSettings& f) {
    w.BeginObject();
    w.Prop("deinterlace", DeintName(f.deinterlace));
    w.Prop("deinterlaceRate", f.deinterlaceRate == DeinterlaceRate::Field ? "Field" : "Frame");
    w.Prop("deinterlaceOnlyInterlaced", f.deinterlaceOnlyInterlaced);
    w.Prop("deinterlacePriority", f.deinterlacePriority == DeinterlacePriority::Quality ? "quality" : "efficiency");
    w.Prop("brightness", f.brightness);
    w.Prop("contrast", f.contrast);
    w.Prop("saturation", f.saturation);
    w.Prop("gamma", f.gamma);
    w.Prop("rotation", (int)f.rotation);
    w.Prop("flipHorizontal", f.flipHorizontal);
    w.Prop("flipVertical", f.flipVertical);
    w.Prop("normalize", f.normalize);
    w.Key("eqGainsDb").BeginArray();
    for (double g : f.eqGainsDb) w.Number(g);
    w.EndArray();
    w.Prop("speed", f.speed);
    w.Prop("advancedMode", f.advancedMode);
    w.Prop("customVideoFilter", f.customVideoFilter);
    w.Prop("customAudioFilter", f.customAudioFilter);
    w.EndObject();
}

void ReadFilters(const Value& o, FilterSettings& f) {
    if (auto* v = Member(o, "deinterlace", Value::Kind::String)) {
        if (v->s == "on") f.deinterlace = DeinterlaceMode::On;
        else if (v->s == "ivtc") f.deinterlace = DeinterlaceMode::Ivtc;
        else f.deinterlace = DeinterlaceMode::Off;
    }
    if (auto* v = Member(o, "deinterlacePriority", Value::Kind::String))
        f.deinterlacePriority = v->s == "quality" ? DeinterlacePriority::Quality : DeinterlacePriority::Efficiency;
    if (auto* v = Member(o, "deinterlaceRate", Value::Kind::String)) f.deinterlaceRate = v->s == "Field" ? DeinterlaceRate::Field : DeinterlaceRate::Frame;
    GetBool(o, "deinterlaceOnlyInterlaced", f.deinterlaceOnlyInterlaced);
    GetNum(o, "brightness", f.brightness);
    GetNum(o, "contrast", f.contrast);
    GetNum(o, "saturation", f.saturation);
    GetNum(o, "gamma", f.gamma);
    int rot = (int)f.rotation;
    GetInt(o, "rotation", rot, -100000, 100000);
    f.rotation = (Rotation)rot;
    GetBool(o, "flipHorizontal", f.flipHorizontal);
    GetBool(o, "flipVertical", f.flipVertical);
    GetBool(o, "normalize", f.normalize);
    if (auto* a = Member(o, "eqGainsDb", Value::Kind::Array)) {
        for (size_t i = 0; i < f.eqGainsDb.size() && i < a->arr.size(); i++)
            if (a->arr[i].kind == Value::Kind::Number && std::isfinite(a->arr[i].n)) f.eqGainsDb[i] = a->arr[i].n;
    }
    GetNum(o, "speed", f.speed);
    GetBool(o, "advancedMode", f.advancedMode);
    GetStr(o, "customVideoFilter", f.customVideoFilter);
    GetStr(o, "customAudioFilter", f.customAudioFilter);
    f = FilterManager::Sanitize(f);
}

void WritePlacement(JsonWriter& w, std::string_view key, const WindowPlacement& p) {
    w.Key(key).BeginObject();
    w.Prop("x", p.x); w.Prop("y", p.y); w.Prop("w", p.w); w.Prop("h", p.h);
    w.Prop("maximized", p.maximized); w.Prop("valid", p.valid); w.Prop("dpi", p.dpi);
    w.EndObject();
}

void ReadPlacement(const Value& root, std::string_view key, WindowPlacement& p) {
    auto* o = Member(root, key, Value::Kind::Object);
    if (!o) return;
    constexpr int kLim = 100000;
    GetInt(*o, "x", p.x, -kLim, kLim);
    GetInt(*o, "y", p.y, -kLim, kLim);
    GetInt(*o, "w", p.w, 0, kLim);
    GetInt(*o, "h", p.h, 0, kLim);
    GetBool(*o, "maximized", p.maximized);
    GetBool(*o, "valid", p.valid);
    GetInt(*o, "dpi", p.dpi, 24, 1000);
}

}

bool IsValidHwDecoder(const std::wstring& v) {
    return v == L"auto" || v == L"gpu" || v == L"d3d11va" || v == L"d3d12va" || v == L"cuda" || v == L"dxva2" || v == L"off";
}

bool IsValidPresenter(const std::wstring& v) { return v == L"auto" || v == L"d3d11" || v == L"sdl"; }

PlanPrefs PlanPrefsFor(const std::wstring& hwDecoder, const std::wstring& presenter) {
    PlanPrefs p;
    if (hwDecoder == L"gpu") p.decoder = DecoderPref::PreferGpu;
    else if (hwDecoder == L"off") p.decoder = DecoderPref::Software;
    else if (hwDecoder == L"d3d11va" || hwDecoder == L"d3d12va" || hwDecoder == L"cuda" || hwDecoder == L"dxva2") {
        p.decoder = DecoderPref::Forced;
        for (wchar_t ch : hwDecoder) p.forcedDecoder.push_back(static_cast<char>(ch));   // ASCII device type names
    }
    if (presenter == L"d3d11") p.presenter = PresenterPref::D3D11;
    else if (presenter == L"sdl") p.presenter = PresenterPref::Sdl;
    return p;
}

SettingsStore::SettingsStore(std::wstring path) : path_(path.empty() ? DefaultPath() : std::move(path)) {}

std::wstring SettingsStore::AppDataDir() { return fileio::LocalAppDataApp(); }
std::wstring SettingsStore::DefaultPath() { return AppDataDir() + L"\\settings.json"; }

std::string SettingsStore::ToJson(const AppSettings& s) {
    JsonWriter w;
    w.BeginObject();
    w.Prop("theme", ThemeName(s.theme));
    w.Prop("accentColor", s.accentColor);
    w.Prop("alwaysOnTop", AlwaysOnTopName(s.alwaysOnTop));
    w.Prop("hwDecoder", s.hwDecoder);
    w.Prop("hwDecoderLastOn", s.hwDecoderLastOn);
    w.Prop("presenter", s.presenter);
    if (s.videoGpu.Automatic()) w.Prop("videoGpu", "auto");
    else {
        w.Key("videoGpu").BeginObject();
        w.Prop("luid", s.videoGpu.luid).Prop("vendorId", s.videoGpu.vendorId).Prop("deviceId", s.videoGpu.deviceId).Prop("name", s.videoGpu.name);
        w.EndObject();
    }
    w.Key("filters");
    WriteFilters(w, s.filters);
    WritePlacement(w, "window", s.window);
    WritePlacement(w, "miniWindow", s.miniWindow);
    WritePlacement(w, "settingsWindow", s.settingsWindow);
    w.Prop("shuffle", s.shuffle);
    w.Prop("repeat", s.repeat);
    w.Prop("playlistPanel", s.playlistPanel);
    w.Prop("playlistPanelWidthDip", s.playlistPanelWidthDip);
    w.Prop("timeRemaining", s.timeRemaining);
    w.Prop("debugLogging", s.debugLogging);
    w.Prop("showCaptions", s.showCaptions);
    w.Prop("showSuperimpose", s.showSuperimpose);
    w.Prop("aribFont", s.aribFont);
    w.Prop("currentPlaylist", s.currentPlaylist);
    w.EndObject();
    return w.Str();
}

bool SettingsStore::FromJson(const std::string& utf8, AppSettings& out) {
    out = AppSettings{};
    std::string_view text = utf8;
    if (text.size() >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF) text.remove_prefix(3);
    auto root = json::Parse(text);
    if (!root || !root->IsObject()) return false;
    const Value& r = *root;

    if (auto* v = Member(r, "theme", Value::Kind::String)) {
        if (v->s == "Light") out.theme = ThemeSetting::Light;
        else if (v->s == "Dark") out.theme = ThemeSetting::Dark;
    }
    if (auto* v = Member(r, "accentColor", Value::Kind::Number))
        if (std::isfinite(v->n)) out.accentColor = (std::uint32_t)std::clamp(v->n, 0.0, 4294967295.0);
    if (auto* v = Member(r, "alwaysOnTop", Value::Kind::String)) {
        if (v->s == "Enable") out.alwaysOnTop = AlwaysOnTop::Enable;
        else if (v->s == "WhenPlaying") out.alwaysOnTop = AlwaysOnTop::WhenPlaying;
        else if (v->s == "WhenFullscreen") out.alwaysOnTop = AlwaysOnTop::WhenFullscreen;
    }
    {
        std::wstring dec;
        GetStr(r, "hwDecoder", dec);
        out.hwDecoder = IsValidHwDecoder(dec) ? dec : std::wstring(L"auto");
    }
    {
        std::wstring last;
        GetStr(r, "hwDecoderLastOn", last);
        out.hwDecoderLastOn = IsValidHwDecoder(last) && last != L"off" ? last : std::wstring(L"auto");
    }
    {
        std::wstring pr;
        if (Member(r, "presenter", Value::Kind::String)) GetStr(r, "presenter", pr);
        out.presenter = IsValidPresenter(pr) ? pr : std::wstring(L"auto");
    }
    if (auto* g = Member(r, "videoGpu", Value::Kind::Object)) {
        if (auto* v = Member(*g, "luid", Value::Kind::String)) out.videoGpu.luid = v->s;
        if (auto* v = Member(*g, "name", Value::Kind::String)) out.videoGpu.name = v->s;
        GetInt(*g, "vendorId", out.videoGpu.vendorId, 0, 0xFFFF);
        GetInt(*g, "deviceId", out.videoGpu.deviceId, 0, 0xFFFF);
    }
    if (auto* f = Member(r, "filters", Value::Kind::Object)) ReadFilters(*f, out.filters);
    ReadPlacement(r, "window", out.window);
    ReadPlacement(r, "miniWindow", out.miniWindow);
    ReadPlacement(r, "settingsWindow", out.settingsWindow);
    GetBool(r, "shuffle", out.shuffle);
    GetInt(r, "repeat", out.repeat, 0, 2);
    GetBool(r, "playlistPanel", out.playlistPanel);
    GetInt(r, "playlistPanelWidthDip", out.playlistPanelWidthDip, 240, 4000);
    GetBool(r, "timeRemaining", out.timeRemaining);
    GetBool(r, "debugLogging", out.debugLogging);
    GetBool(r, "showCaptions", out.showCaptions);
    GetBool(r, "showSuperimpose", out.showSuperimpose);
    GetStr(r, "aribFont", out.aribFont);
    GetStr(r, "currentPlaylist", out.currentPlaylist);
    return true;
}

AppSettings SettingsStore::Load() const {
    AppSettings s;
    try {
        std::string bytes;
        if (!fileio::ReadAllBytes(path_, bytes)) return AppSettings{};
        if (!FromJson(bytes, s)) return AppSettings{};
    } catch (...) {
        return AppSettings{};
    }
    return s;
}

bool SettingsStore::Save(const AppSettings& s) const {
    try { return fileio::AtomicWriteFile(path_, ToJson(s)); }
    catch (...) { return false; }
}

}
