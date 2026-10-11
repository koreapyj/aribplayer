// Pure logic; no engine needed.
#include "wvptest.h"

#include <algorithm>
#include <cmath>

#include "wvp/CapabilitySet.h"
#include "wvp/FilterManager.h"

using namespace wvp;

namespace {

FilterSettings Fs() { return FilterSettings{}; }

// Plans on a machine without a hardware device: CPU filters, software decoding.
FilterManager Fm() {
    FilterManager f;
    f.SetCapabilities(fakecaps::None());
    return f;
}

std::string PlanVf(const FilterProvider& p) {
    std::optional<OpenOptions> o = p.planOptions();
    if (!o) return "<no plan>";
    for (auto& e : *o) if (e.first == "vf") return e.second;
    return "";
}

}

TEST_CASE(Filter_default_is_empty) {
    FilterManager f = Fm();
    CHECK_EQ(FilterSettings{}.deinterlace, DeinterlaceMode::Off);
    CHECK_EQ(f.Settings().deinterlace, DeinterlaceMode::Off);
    CHECK_EQ(f.VideoFilter(), "");
    CHECK_EQ(f.AudioFilter(), "");
}

TEST_CASE(Filter_deinterlace_presets) {
    struct Row { DeinterlacePriority p; DeinterlaceRate r; bool only; const char* expected; };
    const Row rows[] = {
        { DeinterlacePriority::Quality, DeinterlaceRate::Frame, false, "bwdif=mode=send_frame:parity=auto:deint=all" },
        { DeinterlacePriority::Quality, DeinterlaceRate::Field, true, "bwdif=mode=send_field:parity=auto:deint=interlaced" },
        { DeinterlacePriority::Efficiency, DeinterlaceRate::Frame, true, "yadif=mode=send_frame:parity=auto:deint=interlaced" },
        { DeinterlacePriority::Efficiency, DeinterlaceRate::Field, false, "yadif=mode=send_field:parity=auto:deint=all" },
    };
    for (auto& row : rows) {
        FilterManager f = Fm();
        auto s = Fs(); s.deinterlace = DeinterlaceMode::On; s.deinterlacePriority = row.p; s.deinterlaceRate = row.r; s.deinterlaceOnlyInterlaced = row.only;
        f.Apply(s);
        CHECK_EQ(f.VideoFilter(), std::string(row.expected));
        CHECK_EQ(f.AudioFilter(), "");
    }
}

TEST_CASE(Filter_ivtc_without_gpu_is_the_cpu_recipe) {
    FilterManager f = Fm();
    auto s = Fs(); s.deinterlace = DeinterlaceMode::Ivtc;
    f.Apply(s);
    const std::string recipe = "fieldmatch,bwdif=mode=send_frame:parity=auto:deint=interlaced,decimate";
    CHECK_EQ(f.VideoFilter(), recipe);
    IvtcTuning t; t.fieldOrder = 1; t.combThreshold = 12.5;
    f.SetIvtcTuning(t);   // the tuning is for the GPU variants: the CPU recipe has no such options
    CHECK_EQ(f.VideoFilter(), recipe);
    s.deinterlaceRate = DeinterlaceRate::Field; s.deinterlaceOnlyInterlaced = true;   // rate/only-interlaced do not apply to Ivtc
    f.Apply(s);
    CHECK_EQ(f.VideoFilter(), recipe);
    s.deinterlace = DeinterlaceMode::On; s.deinterlacePriority = DeinterlacePriority::Quality;
    f.Apply(s);
    CHECK_EQ(f.VideoFilter(), "bwdif=mode=send_field:parity=auto:deint=interlaced");
    s.deinterlace = DeinterlaceMode::Ivtc;
    f.Apply(s);
    CHECK_EQ(f.VideoFilter(), recipe);
}

TEST_CASE(Filter_speed_video_and_audio) {
    struct Row { double speed; const char* vf; const char* af; };
    const Row rows[] = {
        { 1.5, "setpts=PTS/1.5", "atempo=1.5" },
        { 0.75, "setpts=PTS/0.75", "atempo=0.75" },
        { 2.0, "setpts=PTS/2", "atempo=2" },
        { 4.0, "setpts=PTS/4", "atempo=2,atempo=2" },
        { 3.0, "setpts=PTS/3", "atempo=2,atempo=1.5" },
        { 0.25, "setpts=PTS/0.25", "atempo=0.5,atempo=0.5" },
        { 0.5, "setpts=PTS/0.5", "atempo=0.5" },
        { 0.4, "setpts=PTS/0.4", "atempo=0.5,atempo=0.8" },
    };
    for (auto& row : rows) {
        FilterManager f = Fm();
        auto s = Fs(); s.speed = row.speed;
        f.Apply(s);
        CHECK_EQ(f.VideoFilter(), std::string(row.vf));
        CHECK_EQ(f.AudioFilter(), std::string(row.af));
    }
}

TEST_CASE(Filter_speed_presets_all_compose) {
    for (double sp : FilterManager::kSpeedPresets) {
        FilterManager f = Fm();
        auto s = Fs(); s.speed = sp;
        f.Apply(s);
        if (sp == 1.0) { CHECK_EQ(f.VideoFilter(), ""); CHECK_EQ(f.AudioFilter(), ""); }
        else { CHECK(!f.VideoFilter().empty()); CHECK(!f.AudioFilter().empty()); }
    }
    CHECK_EQ(FilterManager::kSpeedPresets.size(), (size_t)8);
}

TEST_CASE(Filter_speed_is_clamped) {
    FilterManager f = Fm();
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
    FilterManager f = Fm();
    auto s = Fs(); s.normalize = true;
    f.Apply(s);
    CHECK_EQ(f.AudioFilter(), "dynaudnorm");
    s.eqGainsDb[5] = 6;   // +6 dB at 1 kHz
    f.Apply(s);
    std::string af = f.AudioFilter();
    const std::string prefix = "dynaudnorm,superequalizer=1b=1:";
    CHECK(af.compare(0, prefix.size(), prefix) == 0);
    std::string body = af.substr(std::string("dynaudnorm,superequalizer=").size());
    int colons = (int)std::count(body.begin(), body.end(), ':');
    CHECK_EQ(colons, 17);   // 18 bands
    size_t p9 = body.find("9b=1.9");
    CHECK(p9 != std::string::npos);
    CHECK(body.size() > 5 && body.compare(body.size() - 5, 5, "18b=1") == 0);
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
    FilterManager f = Fm();
    auto s = Fs();
    s.deinterlace = DeinterlaceMode::On;
    s.speed = 2; s.normalize = true; s.customVideoFilter = "hue=s=0"; s.customAudioFilter = "volume=2";
    f.Apply(s);
    CHECK_EQ(f.VideoFilter(), "yadif=mode=send_frame:parity=auto:deint=all,setpts=PTS/2,hue=s=0");
    CHECK_EQ(f.AudioFilter(), "dynaudnorm,atempo=2,volume=2");
}

TEST_CASE(Filter_raw_mode_appends_by_default_and_replaces_in_advanced) {
    FilterManager f = Fm();
    auto s = Fs();
    s.speed = 2; s.customVideoFilter = "  negate  "; s.normalize = true; s.customAudioFilter = "aecho=0.8:0.9:1000:0.3";
    f.Apply(s);
    CHECK_EQ(f.VideoFilter(), "setpts=PTS/2,negate");
    CHECK_EQ(f.AudioFilter(), "dynaudnorm,atempo=2,aecho=0.8:0.9:1000:0.3");
    s.speed = 1;
    f.Apply(s);
    CHECK_EQ(f.VideoFilter(), "negate");
    CHECK_EQ(f.AudioFilter(), "dynaudnorm,aecho=0.8:0.9:1000:0.3");
    s.advancedMode = true;
    f.Apply(s);
    CHECK_EQ(f.VideoFilter(), "negate");
    CHECK_EQ(f.AudioFilter(), "aecho=0.8:0.9:1000:0.3");
    s.customVideoFilter = "   ";
    f.Apply(s);
    CHECK_EQ(f.VideoFilter(), "");
}

TEST_CASE(Filter_escape) {
    CHECK_EQ(FilterManager::Escape("plain"), "plain");
    CHECK_EQ(FilterManager::Escape("a:b"), "a\\:b");
    CHECK_EQ(FilterManager::Escape("it's"), "it\\'s");
    CHECK_EQ(FilterManager::Escape("a,b;c=d"), "a\\,b\\;c\\=d");
    CHECK_EQ(FilterManager::Escape("C:\\f.txt"), "C\\:\\\\f.txt");
    CHECK_EQ(FilterManager::Escape("[x] y"), "\\[x\\]\\ y");
}

TEST_CASE(Filter_num_formatting) {
    CHECK_EQ(FilterManager::Num(1), "1");
    CHECK_EQ(FilterManager::Num(0.1), "0.1");
    CHECK_EQ(FilterManager::Num(-0.2), "-0.2");
    CHECK_EQ(FilterManager::Num(12.5), "12.5");
    CHECK_EQ(FilterManager::Num(0.0000001), "0");
    CHECK_EQ(FilterManager::Num(1.0 / 3.0), "0.333333");
}


TEST_CASE(Filter_structural_changes_and_noops) {
    FilterManager f = Fm();
    auto s = Fs();
    FilterChange c = f.Apply(s);
    CHECK(!c.videoRebuild && !c.audioRebuild);

    s.normalize = true;
    c = f.Apply(s);
    CHECK(c.audioRebuild);
    CHECK(!c.videoRebuild);
    CHECK_EQ(c.audioFilter, "dynaudnorm");

    s.speed = 1.25;
    c = f.Apply(s);
    CHECK(c.videoRebuild && c.audioRebuild);

    c = f.Apply(s);
    CHECK(!c.videoRebuild && !c.audioRebuild);

    s.deinterlace = DeinterlaceMode::On; s.deinterlacePriority = DeinterlacePriority::Quality;
    c = f.Apply(s);
    CHECK(c.videoRebuild);
    CHECK(!c.audioRebuild);

    s.eqGainsDb[3] = 3;
    c = f.Apply(s);
    CHECK(c.audioRebuild);
    CHECK(!c.videoRebuild);
}

TEST_CASE(Filter_advanced_mode_reports_rebuild) {
    FilterManager f = Fm();
    auto s = Fs(); s.deinterlace = DeinterlaceMode::On; s.customVideoFilter = "negate";
    f.Apply(s);
    s.advancedMode = true;
    FilterChange c = f.Apply(s);
    CHECK(c.videoRebuild);
    CHECK_EQ(c.videoFilter, "negate");
    s.deinterlace = DeinterlaceMode::Ivtc;
    c = f.Apply(s);
    CHECK(!c.videoRebuild);
}

TEST_CASE(Filter_reset_and_provider) {
    FilterManager f = Fm();
    auto s = Fs();
    s.deinterlace = DeinterlaceMode::On; s.speed = 1.5; s.normalize = true; s.customVideoFilter = "negate";
    s.eqGainsDb[2] = 4;
    f.Apply(s);
    FilterProvider p = f.Provider();
    CHECK_EQ(PlanVf(p), f.VideoFilter());
    CHECK_EQ(p.audioFilter(), f.AudioFilter());
    FilterChange c = f.Reset();
    CHECK(c.videoRebuild && c.audioRebuild);
    CHECK_EQ(f.VideoFilter(), "");
    CHECK_EQ(f.AudioFilter(), "");
    CHECK(f.Settings() == FilterSettings{});
    CHECK_EQ(f.Settings().deinterlace, DeinterlaceMode::Off);
    CHECK_EQ(PlanVf(p), "");
}

TEST_CASE(Filter_export_import_equivalence) {
    auto s = Fs();
    s.deinterlace = DeinterlaceMode::On; s.speed = 1.5; s.normalize = true; s.customVideoFilter = "negate";
    s.eqGainsDb[2] = 4;
    FilterManager a = Fm(), b = Fm();
    a.Apply(s);
    b.Apply(a.Settings());
    CHECK_EQ(a.VideoFilter(), b.VideoFilter());
    CHECK_EQ(a.AudioFilter(), b.AudioFilter());
    CHECK(a.Settings() == b.Settings());
}

TEST_CASE(Filter_sanitize_handles_garbage) {
    auto s = Fs();
    s.deinterlace = static_cast<DeinterlaceMode>(99);
    s.speed = std::nan("");
    s.eqGainsDb[0] = 99;
    s.eqGainsDb[1] = -99;
    auto r = FilterManager::Sanitize(s);
    CHECK(r.deinterlace == DeinterlaceMode::Off);
    CHECK_EQ(r.speed, 1.0);
    CHECK_EQ(r.eqGainsDb[0], 20.0);
    CHECK_EQ(r.eqGainsDb[1], -20.0);
}
