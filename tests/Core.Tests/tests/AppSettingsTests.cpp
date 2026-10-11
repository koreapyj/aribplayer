#include "wvptest.h"
#include "TempDir.h"

#include <cstring>
#include <string>

#include "wvp/AppSettings.h"
#include "wvp/JsonWriter.h"

using namespace wvp;
using wvptest::TempDir;

namespace {

void WriteText(const std::wstring& path, const std::string& text) {
    REQUIRE(fileio::AtomicWriteFile(path, text));
}

}

TEST_CASE(Settings_defaults_when_missing_or_corrupt_and_round_trip) {
    TempDir t;
    SettingsStore store(t.File(std::wstring(L"nested") + fileio::kSep + L"deeper" + fileio::kSep + L"settings.json"));
    AppSettings d = store.Load();
    CHECK(d == AppSettings{});
    CHECK(d.theme == ThemeSetting::System);
    CHECK(d.alwaysOnTop == AlwaysOnTop::Disable);
    CHECK(d.filters.deinterlace == DeinterlaceMode::Off);

    AppSettings s;
    s.theme = ThemeSetting::Dark;
    s.accentColor = 0xFF3A7BD5u;
    s.alwaysOnTop = AlwaysOnTop::WhenFullscreen;
    s.hwDecoder = L"off";
    s.filters.deinterlace = DeinterlaceMode::On;
    s.filters.deinterlacePriority = DeinterlacePriority::Quality;
    s.filters.speed = 1.25;
    s.filters.brightness = -0.25;
    s.filters.eqGainsDb[4] = 3.5;
    s.filters.customVideoFilter = L"hue=s=0";
    s.filters.rotation = Rotation::Ccw90;
    s.window = { 10, -20, 1280, 720, true, true, 144 };
    s.miniWindow = { 100, 200, 320, 180, false, true };
    s.shuffle = true;
    s.repeat = 2;
    REQUIRE(store.Save(s));
    AppSettings back = store.Load();
    CHECK(back == s);
    CHECK(back.window.maximized && back.window.valid);
    CHECK_EQ(back.window.y, -20);
    CHECK_EQ(back.window.dpi, 144);
    CHECK_EQ(back.miniWindow.dpi, 96);
    CHECK_EQ(back.accentColor, 0xFF3A7BD5u);
    CHECK(!fileio::IsRegularFile(store.Path() + L".tmp"));

    WriteText(store.Path(), "{ not json");
    CHECK(store.Load() == AppSettings{});
    WriteText(store.Path(), "[1,2,3]");
    CHECK(store.Load() == AppSettings{});
    WriteText(store.Path(), "");
    CHECK(store.Load() == AppSettings{});
}

TEST_CASE(Settings_file_uses_readable_enum_names) {
    TempDir t;
    SettingsStore store(t.File(L"s.json"));
    AppSettings s; s.theme = ThemeSetting::Light;
    REQUIRE(store.Save(s));
    std::string json;
    REQUIRE(fileio::ReadAllBytes(store.Path(), json));
    CHECK(json.find("\"theme\": \"Light\"") != std::string::npos);
    CHECK(json.find("\"deinterlace\": \"off\"") != std::string::npos);
    CHECK(json.find("\"filters\"") != std::string::npos);

    auto load = [](const char* filters) {
        AppSettings loaded;
        REQUIRE(SettingsStore::FromJson(std::string("{\"filters\": {") + filters + "}}", loaded));
        return loaded.filters;
    };
    CHECK(load("\"deinterlace\": \"on\"").deinterlace == DeinterlaceMode::On);
    CHECK(load("\"deinterlace\": \"ivtc\"").deinterlace == DeinterlaceMode::Ivtc);
    CHECK(load("\"deinterlace\": \"off\"").deinterlace == DeinterlaceMode::Off);
    for (const char* legacy : {"Bwdif", "Yadif", "Ivtc", "On", "IVTC"})
        CHECK(load((std::string("\"deinterlace\": \"") + legacy + "\"").c_str()).deinterlace == DeinterlaceMode::Off);
    s.filters.deinterlace = DeinterlaceMode::Ivtc;
    json = SettingsStore::ToJson(s);
    CHECK(json.find("\"deinterlace\": \"ivtc\"") != std::string::npos);
    AppSettings back;
    REQUIRE(SettingsStore::FromJson(json, back));
    CHECK(back.filters.deinterlace == DeinterlaceMode::Ivtc);
    for (DeinterlaceMode m : { DeinterlaceMode::Off, DeinterlaceMode::On, DeinterlaceMode::Ivtc }) {
        AppSettings a; a.filters.deinterlace = m;
        AppSettings b;
        std::string saved = SettingsStore::ToJson(a);
        REQUIRE(SettingsStore::FromJson(saved, b));
        CHECK(b.filters.deinterlace == m);
    }
    {
        AppSettings on; on.filters.deinterlace = DeinterlaceMode::On;
        const std::string onJson = SettingsStore::ToJson(on);
        CHECK(onJson.find("\"deinterlace\": \"on\"") != std::string::npos);
        AppSettings onBack;
        REQUIRE(SettingsStore::FromJson(onJson, onBack));
        CHECK(onBack.filters.deinterlace == DeinterlaceMode::On);
    }
}

TEST_CASE(Settings_deinterlace_priority_round_trip_and_default) {
    CHECK(AppSettings{}.filters.deinterlacePriority == DeinterlacePriority::Efficiency);
    for (DeinterlacePriority p : { DeinterlacePriority::Efficiency, DeinterlacePriority::Quality }) {
        AppSettings a; a.filters.deinterlacePriority = p;
        const std::string saved = SettingsStore::ToJson(a);
        CHECK(saved.find(p == DeinterlacePriority::Quality ? "\"deinterlacePriority\": \"quality\"" : "\"deinterlacePriority\": \"efficiency\"") != std::string::npos);
        AppSettings b;
        REQUIRE(SettingsStore::FromJson(saved, b));
        CHECK(b.filters.deinterlacePriority == p);
    }
    AppSettings missing; missing.filters.deinterlacePriority = DeinterlacePriority::Quality;
    REQUIRE(SettingsStore::FromJson("{\"filters\":{\"deinterlace\":\"on\"}}", missing));
    CHECK(missing.filters.deinterlacePriority == DeinterlacePriority::Efficiency);
}

TEST_CASE(Settings_alwaysOnTop_default_round_trip_and_invalid) {
    CHECK(AppSettings{}.alwaysOnTop == AlwaysOnTop::Disable);
    const struct { AlwaysOnTop v; const char* name; } all[] = {
        {AlwaysOnTop::Disable, "Disable"}, {AlwaysOnTop::Enable, "Enable"},
        {AlwaysOnTop::WhenPlaying, "WhenPlaying"}, {AlwaysOnTop::WhenFullscreen, "WhenFullscreen"}};
    for (const auto& a : all) {
        AppSettings s; s.alwaysOnTop = a.v;
        const std::string json = SettingsStore::ToJson(s);
        CHECK(json.find(std::string("\"alwaysOnTop\": \"") + a.name + "\"") != std::string::npos);
        AppSettings back;
        REQUIRE(SettingsStore::FromJson(json, back));
        CHECK(back.alwaysOnTop == a.v);
    }
    AppSettings s; s.alwaysOnTop = AlwaysOnTop::Enable;
    REQUIRE(SettingsStore::FromJson("{\"alwaysOnTop\": \"Sometimes\"}", s));
    CHECK(s.alwaysOnTop == AlwaysOnTop::Disable);
    s.alwaysOnTop = AlwaysOnTop::Enable;
    REQUIRE(SettingsStore::FromJson("{\"alwaysOnTop\": 2}", s));
    CHECK(s.alwaysOnTop == AlwaysOnTop::Disable);
    s.alwaysOnTop = AlwaysOnTop::Enable;
    REQUIRE(SettingsStore::FromJson("{}", s));
    CHECK(s.alwaysOnTop == AlwaysOnTop::Disable);
}

TEST_CASE(Settings_clamps_out_of_range_values) {
    AppSettings s;
    bool ok = SettingsStore::FromJson(
        "{\"repeat\": 9, \"theme\": \"Neon\","
        "\"window\": {\"x\": 99999999, \"w\": -5, \"h\": 400, \"valid\": true},"
        "\"filters\": {\"speed\": 500, \"brightness\": -50, \"gamma\": 0, \"rotation\": 45, \"deinterlace\": \"Foo\","
        " \"eqGainsDb\": [99, -99, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11]}}", s);
    CHECK(ok);
    CHECK_EQ(s.repeat, 2);
    CHECK(s.theme == ThemeSetting::System);
    CHECK_EQ(s.window.x, 100000);
    CHECK_EQ(s.window.w, 0);
    CHECK_EQ(s.window.h, 400);
    CHECK_EQ(s.window.dpi, 96);
    CHECK_EQ(s.filters.speed, 100.0);   // atempo tempo range cap
    CHECK_EQ(s.filters.brightness, -1.0);
    CHECK_EQ(s.filters.gamma, 0.1);
    CHECK(s.filters.rotation == Rotation::None);
    CHECK(s.filters.deinterlace == DeinterlaceMode::Off);
    CHECK_EQ(s.filters.eqGainsDb[0], 20.0);
    CHECK_EQ(s.filters.eqGainsDb[1], -20.0);
    CHECK_EQ(s.filters.eqGainsDb[9], 8.0);

    SettingsStore::FromJson("{\"window\": {\"w\": 100, \"dpi\": 100000}}", s);
    CHECK_EQ(s.window.dpi, 1000);
    SettingsStore::FromJson("{\"repeat\": -7}", s);
    CHECK_EQ(s.repeat, 0);
}

TEST_CASE(Settings_ignores_unknown_keys_and_wrong_types) {
    AppSettings s;
    bool ok = SettingsStore::FromJson(
        "{\"future\": {\"a\": [1,2]}, \"repeat\": \"loud\", \"debugLogging\": 1, \"shuffle\": true, \"someFutureFlag\": false}", s);
    CHECK(ok);
    CHECK_EQ(s.repeat, 0);
    CHECK(!s.debugLogging);
    CHECK(s.shuffle);
    for (const char* json : {"{\"filters\":{}}", "{\"filters\":null}"}) {
        AppSettings f;
        CHECK(SettingsStore::FromJson(json, f));
        CHECK(f.filters == FilterSettings{});
    }
}

TEST_CASE(Settings_partial_file_keeps_defaults_for_missing_keys) {
    TempDir t;
    SettingsStore store(t.File(L"p.json"));
    WriteText(store.Path(), "{\"repeat\": 1}");
    AppSettings s = store.Load();
    CHECK_EQ(s.repeat, 1);
    CHECK_EQ(s.hwDecoder, std::wstring(L"auto"));
    CHECK(s.theme == ThemeSetting::System);
}

TEST_CASE(Settings_utf8_bom_is_accepted) {
    AppSettings s;
    CHECK(SettingsStore::FromJson(std::string("\xEF\xBB\xBF{\"repeat\": 2}"), s));
    CHECK_EQ(s.repeat, 2);
}

TEST_CASE(Settings_default_path_is_under_localappdata) {
    std::wstring p = SettingsStore::DefaultPath();
    std::wstring suffix = std::wstring(L"aribplayer") + fileio::kSep + L"settings.json";
    REQUIRE(p.size() > suffix.size());
    CHECK_EQ(p.substr(p.size() - suffix.size()), suffix);
    CHECK_EQ(SettingsStore::AppDataDir() + fileio::kSep + L"settings.json", p);
    CHECK_EQ(SettingsStore().Path(), p);
}

TEST_CASE(Settings_debugLogging_defaults_off_and_round_trips) {
    AppSettings d;
    CHECK(!d.debugLogging);
    AppSettings s;
    s.debugLogging = true;
    AppSettings back;
    CHECK(SettingsStore::FromJson(SettingsStore::ToJson(s), back));
    CHECK(back.debugLogging);
    CHECK(back == s);
    AppSettings missing;
    CHECK(SettingsStore::FromJson("{}", missing));
    CHECK(!missing.debugLogging);
}

TEST_CASE(Settings_currentPlaylist_round_trips_and_defaults_empty_when_missing) {
    AppSettings d;
    CHECK(d.currentPlaylist.empty());
    AppSettings s;
    s.currentPlaylist = L"Road trip \x266A";
    AppSettings back;
    CHECK(SettingsStore::FromJson(SettingsStore::ToJson(s), back));
    CHECK(back.currentPlaylist == s.currentPlaylist);
    CHECK(back == s);
    AppSettings unnamed;
    CHECK(SettingsStore::FromJson(SettingsStore::ToJson(AppSettings{}), unnamed));
    CHECK(unnamed.currentPlaylist.empty());
    AppSettings old;
    old.currentPlaylist = L"stale";
    CHECK(SettingsStore::FromJson("{\"repeat\":1}", old));
    CHECK(old.currentPlaylist.empty());
}

TEST_CASE(Settings_presenter_and_gpu_decoder_round_trip) {
    TempDir t;
    SettingsStore store(t.File(L"pr.json"));
    AppSettings d;
    CHECK_EQ(d.presenter, std::wstring(L"auto"));
    WriteText(store.Path(), "{\"hwDecoder\": \"cuda\"}");
    CHECK_EQ(store.Load().presenter, std::wstring(L"auto"));
    WriteText(store.Path(), "{\"presenter\": \"vulkan\"}");
    CHECK_EQ(store.Load().presenter, std::wstring(L"auto"));
    WriteText(store.Path(), "{\"presenter\": 5}");
    CHECK_EQ(store.Load().presenter, std::wstring(L"auto"));
    for (const wchar_t* v : { L"gpu", L"dxva2" }) {
        WriteText(store.Path(), std::string("{\"hwDecoder\": \"") + (v[0] == L'g' ? "gpu" : "dxva2") + "\"}");
        CHECK_EQ(store.Load().hwDecoder, std::wstring(v));
    }
    for (const wchar_t* p : { L"auto", L"d3d11", L"sdl" }) {
        AppSettings s;
        s.presenter = p;
        s.hwDecoder = L"gpu";
        AppSettings back;
        CHECK(SettingsStore::FromJson(SettingsStore::ToJson(s), back));
        CHECK_EQ(back.presenter, std::wstring(p));
        CHECK_EQ(back.hwDecoder, std::wstring(L"gpu"));
        CHECK(back == s);
    }
    CHECK(IsValidHwDecoder(L"gpu") && IsValidHwDecoder(L"dxva2") && !IsValidHwDecoder(L"vulkan"));
    CHECK(IsValidPresenter(L"d3d11") && IsValidPresenter(L"sdl") && IsValidPresenter(L"auto") && !IsValidPresenter(L"gl"));
}

TEST_CASE(Settings_plan_prefs_mapping) {
    PlanPrefs p = PlanPrefsFor(L"auto", L"auto");
    CHECK(p.decoder == DecoderPref::Auto && p.forcedDecoder.empty() && p.presenter == PresenterPref::Auto);
    p = PlanPrefsFor(L"gpu", L"d3d11");
    CHECK(p.decoder == DecoderPref::PreferGpu && p.presenter == PresenterPref::D3D11);
    for (const char* name : { "d3d11va", "d3d12va", "cuda", "dxva2" }) {
        p = PlanPrefsFor(std::wstring(name, name + std::strlen(name)), L"sdl");
        CHECK(p.decoder == DecoderPref::Forced);
        CHECK_EQ(p.forcedDecoder, std::string(name));
        CHECK(p.presenter == PresenterPref::Sdl);
    }
    p = PlanPrefsFor(L"off", L"auto");
    CHECK(p.decoder == DecoderPref::Software && p.forcedDecoder.empty());
    p = PlanPrefsFor(L"bogus", L"bogus");
    CHECK(p == PlanPrefs{});
}

TEST_CASE(Settings_hwDecoder_defaults_valid_invalid_and_round_trip) {
    TempDir t;
    SettingsStore store(t.File(L"hw.json"));
    WriteText(store.Path(), "{}");
    AppSettings a = store.Load();
    CHECK_EQ(a.hwDecoder, std::wstring(L"auto"));
    WriteText(store.Path(), "{\"hwDecoder\": \"cuda\"}");
    a = store.Load();
    CHECK_EQ(a.hwDecoder, std::wstring(L"cuda"));
    WriteText(store.Path(), "{\"hwDecoder\": \"off\"}");
    a = store.Load();
    CHECK_EQ(a.hwDecoder, std::wstring(L"off"));
    WriteText(store.Path(), "{\"hwDecoder\": \"bogus\"}");
    a = store.Load();
    CHECK_EQ(a.hwDecoder, std::wstring(L"auto"));
    AppSettings s;
    s.hwDecoder = L"d3d12va";
    REQUIRE(store.Save(s));
    CHECK_EQ(store.Load().hwDecoder, std::wstring(L"d3d12va"));
}
