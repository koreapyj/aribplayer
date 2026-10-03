// Pure logic; no engine needed.
#include "wvptest.h"

#include <algorithm>
#include <cmath>

#include "wvp/FilterManager.h"

using namespace wvp;

namespace {

FilterSettings Fs() { return FilterSettings{}; }

}

TEST_CASE(Filter_default_is_empty) {
    FilterManager f;
    CHECK_EQ(FilterSettings{}.deinterlace, DeinterlaceMode::Off);
    CHECK_EQ(f.Settings().deinterlace, DeinterlaceMode::Off);
    CHECK_EQ(f.VideoFilter(), L"");
    CHECK_EQ(f.AudioFilter(), L"");
}

TEST_CASE(Filter_deinterlace_presets) {
    struct Row { DeinterlacePriority p; DeinterlaceRate r; bool only; const wchar_t* expected; };
    const Row rows[] = {
        { DeinterlacePriority::Quality, DeinterlaceRate::Frame, false, L"bwdif=mode=send_frame:parity=auto:deint=all" },
        { DeinterlacePriority::Quality, DeinterlaceRate::Field, true, L"bwdif=mode=send_field:parity=auto:deint=interlaced" },
        { DeinterlacePriority::Efficiency, DeinterlaceRate::Frame, true, L"yadif=mode=send_frame:parity=auto:deint=interlaced" },
        { DeinterlacePriority::Efficiency, DeinterlaceRate::Field, false, L"yadif=mode=send_field:parity=auto:deint=all" },
    };
    for (auto& row : rows) {
        FilterManager f;
        auto s = Fs(); s.deinterlace = DeinterlaceMode::On; s.deinterlacePriority = row.p; s.deinterlaceRate = row.r; s.deinterlaceOnlyInterlaced = row.only;
        f.Apply(s);
        CHECK_EQ(f.VideoFilter(), std::wstring(row.expected));
        CHECK_EQ(f.AudioFilter(), L"");
    }
}

TEST_CASE(Filter_ivtc_legacy_is_the_cpu_recipe) {
    FilterManager f;
    auto s = Fs(); s.deinterlace = DeinterlaceMode::Ivtc;
    f.Apply(s);
    const std::wstring recipe = L"fieldmatch,bwdif=mode=send_frame:parity=auto:deint=interlaced,decimate";
    CHECK_EQ(f.VideoFilter(), recipe);
    IvtcTuning t; t.fieldOrder = 1; t.combThreshold = 12.5;
    f.SetIvtcTuning(t);   // the tuning is for the GPU variants: the CPU recipe has no such options
    CHECK_EQ(f.VideoFilter(), recipe);
    s.deinterlaceRate = DeinterlaceRate::Field; s.deinterlaceOnlyInterlaced = true;   // rate/only-interlaced do not apply to Ivtc
    f.Apply(s);
    CHECK_EQ(f.VideoFilter(), recipe);
    s.deinterlace = DeinterlaceMode::On; s.deinterlacePriority = DeinterlacePriority::Quality;
    f.Apply(s);
    CHECK_EQ(f.VideoFilter(), L"bwdif=mode=send_field:parity=auto:deint=interlaced");
    s.deinterlace = DeinterlaceMode::Ivtc;
    f.Apply(s);
    CHECK_EQ(f.VideoFilter(), recipe);
}

TEST_CASE(Filter_rotate_and_flip) {
    struct Row { Rotation r; bool h, v; const wchar_t* expected; };
    const Row rows[] = {
        { Rotation::Cw90, false, false, L"transpose=1" },
        { Rotation::Ccw90, false, false, L"transpose=2" },
        { Rotation::Rotate180, false, false, L"hflip,vflip" },
        { Rotation::None, true, false, L"hflip" },
        { Rotation::None, false, true, L"vflip" },
        { Rotation::None, true, true, L"hflip,vflip" },
        { Rotation::Rotate180, true, false, L"vflip" },
        { Rotation::Rotate180, true, true, L"" },
        { Rotation::Cw90, true, false, L"transpose=1,hflip" },
    };
    for (auto& row : rows) {
        FilterManager f;
        auto s = Fs(); s.rotation = row.r; s.flipHorizontal = row.h; s.flipVertical = row.v;
        f.Apply(s);
        CHECK_EQ(f.VideoFilter(), std::wstring(row.expected));
    }
}

TEST_CASE(Filter_eq_added_only_when_non_default_and_named) {
    FilterManager f;
    auto s = Fs(); s.contrast = 1;
    f.Apply(s);
    CHECK_EQ(f.VideoFilter(), L"");
    s.brightness = 0.1;
    f.Apply(s);
    CHECK_EQ(f.VideoFilter(), L"eq@color=brightness=0.1:contrast=1:saturation=1:gamma=1");
    s.brightness = 0;
    f.Apply(s);
    CHECK_EQ(f.VideoFilter(), L"");
}

TEST_CASE(Filter_eq_values_are_clamped) {
    FilterManager f;
    auto s = Fs(); s.brightness = 5; s.saturation = 1.5; s.gamma = 0.01;
    f.Apply(s);
    CHECK_EQ(f.VideoFilter(), L"eq@color=brightness=1:contrast=1:saturation=1.5:gamma=0.1");
    CHECK_EQ(f.Settings().brightness, 1.0);
    CHECK_EQ(f.Settings().gamma, 0.1);
}

TEST_CASE(Filter_speed_video_and_audio) {
    struct Row { double speed; const wchar_t* vf; const wchar_t* af; };
    const Row rows[] = {
        { 1.5, L"setpts=PTS/1.5", L"atempo=1.5" },
        { 0.75, L"setpts=PTS/0.75", L"atempo=0.75" },
        { 2.0, L"setpts=PTS/2", L"atempo=2" },
        { 4.0, L"setpts=PTS/4", L"atempo=2,atempo=2" },
        { 3.0, L"setpts=PTS/3", L"atempo=2,atempo=1.5" },
        { 0.25, L"setpts=PTS/0.25", L"atempo=0.5,atempo=0.5" },
        { 0.5, L"setpts=PTS/0.5", L"atempo=0.5" },
        { 0.4, L"setpts=PTS/0.4", L"atempo=0.5,atempo=0.8" },
    };
    for (auto& row : rows) {
        FilterManager f;
        auto s = Fs(); s.speed = row.speed;
        f.Apply(s);
        CHECK_EQ(f.VideoFilter(), std::wstring(row.vf));
        CHECK_EQ(f.AudioFilter(), std::wstring(row.af));
    }
}

TEST_CASE(Filter_speed_presets_all_compose) {
    for (double sp : FilterManager::kSpeedPresets) {
        FilterManager f;
        auto s = Fs(); s.speed = sp;
        f.Apply(s);
        if (sp == 1.0) { CHECK_EQ(f.VideoFilter(), L""); CHECK_EQ(f.AudioFilter(), L""); }
        else { CHECK(!f.VideoFilter().empty()); CHECK(!f.AudioFilter().empty()); }
    }
    CHECK_EQ(FilterManager::kSpeedPresets.size(), (size_t)8);
}

TEST_CASE(Filter_speed_is_clamped) {
    FilterManager f;
    auto s = Fs(); s.speed = 1000;
    f.Apply(s);
    CHECK_EQ(f.Settings().speed, FilterManager::kMaxSpeed);
    CHECK_EQ(FilterManager::kMaxSpeed, 100.0);
    CHECK_EQ(FilterManager::kMinSpeed, 0.1);
    s.speed = 0;
    f.Apply(s);
    CHECK_EQ(f.Settings().speed, FilterManager::kMinSpeed);
}

TEST_CASE(Filter_audio_normalize_and_equalizer) {
    FilterManager f;
    auto s = Fs(); s.normalize = true;
    f.Apply(s);
    CHECK_EQ(f.AudioFilter(), L"dynaudnorm");
    s.eqGainsDb[5] = 6;   // +6 dB at 1 kHz
    f.Apply(s);
    std::wstring af = f.AudioFilter();
    const std::wstring prefix = L"dynaudnorm,superequalizer=1b=1:";
    CHECK(af.compare(0, prefix.size(), prefix) == 0);
    std::wstring body = af.substr(std::wstring(L"dynaudnorm,superequalizer=").size());
    int colons = (int)std::count(body.begin(), body.end(), L':');
    CHECK_EQ(colons, 17);   // 18 bands
    size_t p9 = body.find(L"9b=1.9");
    CHECK(p9 != std::wstring::npos);
    CHECK(body.size() > 5 && body.compare(body.size() - 5, 5, L"18b=1") == 0);
}

TEST_CASE(Filter_equalizer_mapping) {
    std::array<double, 10> flat{};
    for (double v : FilterManager::MapEqualizer(flat)) CHECK_EQ(v, 1.0);
    std::array<double, 10> g{};
    g[1] = 12; g[9] = -12;
    auto m = FilterManager::MapEqualizer(g);
    CHECK(m[0] >= 3.5 && m[0] <= 3.98);
    CHECK_EQ(m[17], std::round(std::pow(10.0, -12 / 20.0) * 1e4) / 1e4);
    CHECK_EQ(m.size(), (size_t)18);
}

TEST_CASE(Filter_full_composition_order_is_deterministic) {
    FilterManager f;
    auto s = Fs();
    s.deinterlace = DeinterlaceMode::On; s.rotation = Rotation::Cw90; s.flipVertical = true;
    s.brightness = -0.2; s.speed = 2; s.normalize = true; s.customVideoFilter = L"hue=s=0"; s.customAudioFilter = L"volume=2";
    f.Apply(s);
    CHECK_EQ(f.VideoFilter(), L"yadif=mode=send_frame:parity=auto:deint=all,transpose=1,vflip,"
                              L"eq@color=brightness=-0.2:contrast=1:saturation=1:gamma=1,setpts=PTS/2,hue=s=0");
    CHECK_EQ(f.AudioFilter(), L"dynaudnorm,atempo=2,volume=2");
}

TEST_CASE(Filter_raw_mode_appends_by_default_and_replaces_in_advanced) {
    FilterManager f;
    auto s = Fs();
    s.rotation = Rotation::Rotate180; s.customVideoFilter = L"  negate  "; s.normalize = true; s.customAudioFilter = L"aecho=0.8:0.9:1000:0.3";
    f.Apply(s);
    CHECK_EQ(f.VideoFilter(), L"hflip,vflip,negate");
    CHECK_EQ(f.AudioFilter(), L"dynaudnorm,aecho=0.8:0.9:1000:0.3");
    s.advancedMode = true;
    f.Apply(s);
    CHECK_EQ(f.VideoFilter(), L"negate");
    CHECK_EQ(f.AudioFilter(), L"aecho=0.8:0.9:1000:0.3");
    s.customVideoFilter = L"   ";
    f.Apply(s);
    CHECK_EQ(f.VideoFilter(), L"");
}

TEST_CASE(Filter_escape) {
    CHECK_EQ(FilterManager::Escape(L"plain"), L"plain");
    CHECK_EQ(FilterManager::Escape(L"a:b"), L"a\\:b");
    CHECK_EQ(FilterManager::Escape(L"it's"), L"it\\'s");
    CHECK_EQ(FilterManager::Escape(L"a,b;c=d"), L"a\\,b\\;c\\=d");
    CHECK_EQ(FilterManager::Escape(L"C:\\f.txt"), L"C\\:\\\\f.txt");
    CHECK_EQ(FilterManager::Escape(L"[x] y"), L"\\[x\\]\\ y");
}

TEST_CASE(Filter_num_formatting) {
    CHECK_EQ(FilterManager::Num(1), L"1");
    CHECK_EQ(FilterManager::Num(0.1), L"0.1");
    CHECK_EQ(FilterManager::Num(-0.2), L"-0.2");
    CHECK_EQ(FilterManager::Num(12.5), L"12.5");
    CHECK_EQ(FilterManager::Num(0.0000001), L"0");
    CHECK_EQ(FilterManager::Num(1.0 / 3.0), L"0.333333");
}


TEST_CASE(Filter_eq_tweak_while_active_is_command_only) {
    FilterManager f;
    auto s = Fs(); s.brightness = 0.1;
    f.Apply(s);

    s.brightness = 0.3;
    FilterChange c = f.Apply(s);
    CHECK(!c.videoRebuild);
    CHECK(!c.audioRebuild);
    REQUIRE(c.commands.size() == 1);
    CHECK_EQ(c.commands[0].target, L"eq@color");
    CHECK_EQ(c.commands[0].command, L"brightness");
    CHECK_EQ(c.commands[0].argument, L"0.3");
    CHECK(c.videoFilter.find(L"brightness=0.3") != std::wstring::npos);

    s.saturation = 2;
    c = f.Apply(s);
    CHECK(!c.videoRebuild);
    REQUIRE(c.commands.size() == 1);
    CHECK_EQ(c.commands[0].command, L"saturation");
    CHECK_EQ(c.commands[0].argument, L"2");

    s.contrast = 1.5; s.gamma = 1.2; s.brightness = -0.1;
    c = f.Apply(s);
    CHECK(!c.videoRebuild && !c.audioRebuild);
    CHECK_EQ(c.commands.size(), (size_t)3);
}

TEST_CASE(Filter_eq_activation_and_deactivation_rebuild) {
    FilterManager f;
    auto s = Fs(); s.gamma = 1.2;
    FilterChange c = f.Apply(s);
    CHECK(c.videoRebuild);
    CHECK(c.commands.empty());
    s.gamma = 1;
    c = f.Apply(s);
    CHECK(c.videoRebuild);
    CHECK(c.commands.empty());
    CHECK_EQ(c.videoFilter, L"");
}

TEST_CASE(Filter_structural_changes_and_noops) {
    FilterManager f;
    auto s = Fs();
    FilterChange c = f.Apply(s);
    CHECK(!c.videoRebuild && !c.audioRebuild && c.commands.empty());

    s.normalize = true;
    c = f.Apply(s);
    CHECK(c.audioRebuild);
    CHECK(!c.videoRebuild);
    CHECK_EQ(c.audioFilter, L"dynaudnorm");

    s.speed = 1.25;
    c = f.Apply(s);
    CHECK(c.videoRebuild && c.audioRebuild);

    c = f.Apply(s);
    CHECK(!c.videoRebuild && !c.audioRebuild && c.commands.empty());

    s.rotation = Rotation::Cw90; s.deinterlace = DeinterlaceMode::On; s.deinterlacePriority = DeinterlacePriority::Quality;
    c = f.Apply(s);
    CHECK(c.videoRebuild);
    CHECK(!c.audioRebuild);

    s.eqGainsDb[3] = 3;
    c = f.Apply(s);
    CHECK(c.audioRebuild);
    CHECK(!c.videoRebuild);
}

TEST_CASE(Filter_eq_change_with_structural_change_is_rebuild_without_commands) {
    FilterManager f;
    auto s = Fs(); s.brightness = 0.1;
    f.Apply(s);
    s.brightness = 0.5; s.rotation = Rotation::Cw90;
    FilterChange c = f.Apply(s);
    CHECK(c.videoRebuild);
    CHECK(c.commands.empty());
}

TEST_CASE(Filter_advanced_mode_hides_eq_and_reports_rebuild) {
    FilterManager f;
    auto s = Fs(); s.brightness = 0.1; s.customVideoFilter = L"negate";
    f.Apply(s);
    s.advancedMode = true;
    FilterChange c = f.Apply(s);
    CHECK(c.videoRebuild);
    CHECK_EQ(c.videoFilter, L"negate");
    s.brightness = 0.2;
    c = f.Apply(s);
    CHECK(!c.videoRebuild && c.commands.empty());
}

TEST_CASE(Filter_reset_and_provider) {
    FilterManager f;
    auto s = Fs();
    s.deinterlace = DeinterlaceMode::On; s.speed = 1.5; s.brightness = 0.2; s.normalize = true; s.customVideoFilter = L"negate";
    s.eqGainsDb[2] = 4;
    f.Apply(s);
    FilterProvider p = f.Provider();
    CHECK_EQ(p.videoFilter(), f.VideoFilter());
    CHECK_EQ(p.audioFilter(), f.AudioFilter());
    FilterChange c = f.Reset();
    CHECK(c.videoRebuild && c.audioRebuild);
    CHECK_EQ(f.VideoFilter(), L"");
    CHECK_EQ(f.AudioFilter(), L"");
    CHECK(f.Settings() == FilterSettings{});
    CHECK_EQ(f.Settings().deinterlace, DeinterlaceMode::Off);
    CHECK_EQ(p.videoFilter(), L"");
}

TEST_CASE(Filter_export_import_equivalence) {
    auto s = Fs();
    s.deinterlace = DeinterlaceMode::On; s.speed = 1.5; s.brightness = 0.2; s.normalize = true; s.customVideoFilter = L"negate";
    s.eqGainsDb[2] = 4;
    FilterManager a, b;
    a.Apply(s);
    b.Apply(a.Settings());
    CHECK_EQ(a.VideoFilter(), b.VideoFilter());
    CHECK_EQ(a.AudioFilter(), b.AudioFilter());
    CHECK(a.Settings() == b.Settings());
}

TEST_CASE(Filter_sanitize_handles_garbage) {
    auto s = Fs();
    s.rotation = (Rotation)45;
    s.deinterlace = static_cast<DeinterlaceMode>(99);
    s.speed = std::nan("");
    s.eqGainsDb[0] = 99;
    s.eqGainsDb[1] = -99;
    auto r = FilterManager::Sanitize(s);
    CHECK(r.rotation == Rotation::None);
    CHECK(r.deinterlace == DeinterlaceMode::Off);
    CHECK_EQ(r.speed, 1.0);
    CHECK_EQ(r.eqGainsDb[0], 20.0);
    CHECK_EQ(r.eqGainsDb[1], -20.0);
}
