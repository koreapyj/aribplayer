#include "wvptest.h"

#include <string>

#include "wvp/AppSettings.h"
#include "wvp/Platform.h"
#include "TempDir.h"

using namespace wvp;
using wvptest::TempDir;

namespace {

void WriteText(const std::string& path, const std::string& text) {
    REQUIRE(platform::AtomicWriteFile(path, text));
}

}

TEST_CASE(Settings_defaults_when_missing_or_corrupt_and_round_trip) {
    TempDir t;
    SettingsStore store(t.File("nested/deeper/settings.json"));
    AppSettings d = store.Load();
    CHECK(d == AppSettings{});
    CHECK(d.theme == ThemeSetting::System);
    CHECK(d.alwaysOnTop == AlwaysOnTop::Disable);
    CHECK(d.filters.deinterlace == DeinterlaceMode::Off);

    AppSettings s;
    s.theme = ThemeSetting::Dark;
    s.accentColor = 0xFF3A7BD5u;
    s.alwaysOnTop = AlwaysOnTop::WhenFullscreen;
    s.filters.deinterlace = DeinterlaceMode::On;
    s.filters.deinterlacePriority = DeinterlacePriority::Quality;
    s.filters.speed = 1.25;
    s.filters.eqGainsDb[4] = 3.5;
    s.filters.customVideoFilter = "hue=s=0";
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
    CHECK(!platform::Exists(store.Path() + ".tmp"));

    WriteText(store.Path(), "{ not json");
    CHECK(store.Load() == AppSettings{});
    WriteText(store.Path(), "[1,2,3]");
    CHECK(store.Load() == AppSettings{});
    WriteText(store.Path(), "");
    CHECK(store.Load() == AppSettings{});
}

TEST_CASE(Settings_file_uses_readable_enum_names) {
    TempDir t;
    SettingsStore store(t.File("s.json"));
    AppSettings s; s.theme = ThemeSetting::Light;
    REQUIRE(store.Save(s));
    std::string json;
    REQUIRE(platform::ReadAllBytes(store.Path(), json, 1ull << 30) == platform::ReadResult::Ok);
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
        "\"filters\": {\"speed\": 500, \"deinterlace\": \"Foo\","
        " \"eqGainsDb\": [99, -99, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11]}}", s);
    CHECK(ok);
    CHECK_EQ(s.repeat, 2);
    CHECK(s.theme == ThemeSetting::System);
    CHECK_EQ(s.window.x, 100000);
    CHECK_EQ(s.window.w, 0);
    CHECK_EQ(s.window.h, 400);
    CHECK_EQ(s.window.dpi, 96);
    CHECK_EQ(s.filters.speed, 100.0);   // atempo tempo range cap
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
    SettingsStore store(t.File("p.json"));
    WriteText(store.Path(), "{\"repeat\": 1}");
    AppSettings s = store.Load();
    CHECK_EQ(s.repeat, 1);
    CHECK(s.theme == ThemeSetting::System);
}

TEST_CASE(Settings_utf8_bom_is_accepted) {
    AppSettings s;
    CHECK(SettingsStore::FromJson(std::string("\xEF\xBB\xBF{\"repeat\": 2}"), s));
    CHECK_EQ(s.repeat, 2);
}

TEST_CASE(Settings_default_path_is_under_the_app_data_dir) {
    const std::string p = SettingsStore::DefaultPath();
    CHECK_EQ(p, platform::Combine(platform::AppDataDir(), "settings.json"));
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
    s.currentPlaylist = "Road trip \u266a";
    AppSettings back;
    CHECK(SettingsStore::FromJson(SettingsStore::ToJson(s), back));
    CHECK(back.currentPlaylist == s.currentPlaylist);
    CHECK(back == s);
    AppSettings unnamed;
    CHECK(SettingsStore::FromJson(SettingsStore::ToJson(AppSettings{}), unnamed));
    CHECK(unnamed.currentPlaylist.empty());
    AppSettings old;
    old.currentPlaylist = "stale";
    CHECK(SettingsStore::FromJson("{\"repeat\":1}", old));
    CHECK(old.currentPlaylist.empty());
}

TEST_CASE(Settings_subtitle_style_defaults_round_trip_and_clamp) {
    AppSettings d;
    CHECK(d.subtitle.useSystemCaptionStyle);
    CHECK(d.subtitle.font == "Malgun Gothic");
    CHECK(d.subtitle.fontWeight == 400 && !d.subtitle.italic);
    CHECK(d.subtitle.fontSizePt == 45.0);
    CHECK(d.subtitle.primaryRgba == 0xFFFFFFFFu && d.subtitle.outlineRgba == 0x000000FFu);
    CHECK(!d.subtitle.overrideFont && !d.subtitle.overrideSize && !d.subtitle.overrideColors && !d.subtitle.overrideBorder && !d.subtitle.applyToAss);

    AppSettings s;
    s.subtitle.useSystemCaptionStyle = false;
    s.subtitle.font = "Noto Sans KR";
    s.subtitle.fontWeight = 700;
    s.subtitle.italic = true;
    s.subtitle.fontSizePt = 36.5;
    s.subtitle.primaryRgba = 0xFFFF00FFu;
    s.subtitle.outlineRgba = 0x112233FFu;
    s.subtitle.outlineWidth = 3.5;
    s.subtitle.overrideFont = s.subtitle.overrideSize = s.subtitle.overrideColors = s.subtitle.overrideBorder = s.subtitle.applyToAss = true;
    AppSettings back;
    CHECK(SettingsStore::FromJson(SettingsStore::ToJson(s), back));
    CHECK(back.subtitle == s.subtitle);
    CHECK(back == s);

    AppSettings clamped;
    CHECK(SettingsStore::FromJson("{\"subtitle\":{\"font\":\"\",\"fontSize\":999,\"fontWeight\":5000,\"outlineWidth\":-4}}", clamped));
    CHECK(clamped.subtitle.font == "Malgun Gothic");
    CHECK(clamped.subtitle.fontSizePt == 400.0);
    CHECK(clamped.subtitle.fontWeight == 900);
    CHECK(clamped.subtitle.outlineWidth == 0.0);
}

TEST_CASE(Settings_unknown_arib_font_and_font_scale_are_ignored_and_dropped) {
    AppSettings old;
    CHECK(SettingsStore::FromJson("{\"aribFont\":\"D:\\\\Fonts\\\\x.ttf\",\"showSuperimpose\":false,"
                                  "\"subtitle\":{\"fontScale\":1.5,\"font\":\"Gulim\"}}", old));
    CHECK(!old.showSuperimpose);
    CHECK(old.subtitle.font == "Gulim");
    CHECK(old.subtitle.fontSizePt == SubtitleStyle{}.fontSizePt);
    const std::string json = SettingsStore::ToJson(old);
    CHECK(json.find("aribFont") == std::string::npos);
    CHECK(json.find("fontScale") == std::string::npos);
}

TEST_CASE(Settings_old_presenter_and_decoder_keys_ignored) {
    TempDir t;
    SettingsStore store(t.File("pr.json"));
    WriteText(store.Path(), "{\"presenter\": \"sdl\", \"hwDecoder\": \"off\", \"hwDecoderLastOn\": \"d3d11va\"}");
    AppSettings old = store.Load();
    CHECK(old == AppSettings{});
    const std::string json = SettingsStore::ToJson(old);
    CHECK(json.find("presenter") == std::string::npos);
    CHECK(json.find("hwDecoder") == std::string::npos);
    WriteText(store.Path(), "{\"filters\": {\"brightness\": 0.5, \"rotation\": 90, \"flipHorizontal\": true}}");
    CHECK(store.Load() == AppSettings{});
}

TEST_CASE(AribFontOption_is_a_file_value_with_forward_slashes) {
    CHECK_EQ(AribFontOption("C:\\fonts\\WLCMARU2004ARIBU.TTF"), std::string("file:C:/fonts/WLCMARU2004ARIBU.TTF"));
    CHECK_EQ(AribFontOption("/usr/share/fonts/a.ttf"), std::string("file:/usr/share/fonts/a.ttf"));
    CHECK_EQ(AribFontOption("file:D:\\x.ttf"), std::string("file:D:/x.ttf"));
}
