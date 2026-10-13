#include <windows.h>

#include <cmath>
#include <filesystem>

#include "LogicTest.h"
#include "wvp/CapabilitySet.h"
#include "features/FxModel.h"
#include "features/FxProps.h"
#include "features/FxUtil.h"
#include "features/DialogGuard.h"
#include "features/Menus.h"
#include "features/NativeMenu.h"
#include "features/PlaybackExtras.h"
#include "features/PropertiesDialog.h"
#include "Services.h"
#include "pui/Renderer.h"

namespace {
using namespace app;
using wvp::FilterManager;
using wvp::FilterSettings;

bool Has(const std::wstring& s, const wchar_t* sub) { return s.find(sub) != std::wstring::npos; }
bool Has(const std::string& s, const char* sub) { return s.find(sub) != std::string::npos; }

const char* kSampleJson = R"({
  "url":"C:\\media\\clip.mp4","title":"My Clip","format":"mov,mp4,m4a,3gp,3g2,mj2","duration":3725.5,"seekable":true,
  "bit_rate":4500000,
  "streams":[
    {"index":0,"type":"video","codec":"h264","width":1920,"height":1080,"sar_num":1,"sar_den":1,"fps":29.97,"pix_fmt":"yuv420p","selected":true,"language":"und"},
    {"index":1,"type":"audio","codec":"aac","sample_rate":48000,"channels":2,"selected":true,"language":"eng"},
    {"index":2,"type":"audio","codec":"ac3","sample_rate":44100,"channels":6,"selected":false,"language":"jpn","title":"Commentary"},
    {"index":3,"type":"subtitle","codec":"subrip","selected":false,"language":"eng","title":"Full"}
  ]})";

const fx::PropSection* Sec(const std::vector<fx::PropSection>& v, const wchar_t* title) {
    for (auto& s : v) if (s.title == title) return &s;
    return nullptr;
}
std::wstring Row(const fx::PropSection* s, const wchar_t* label) {
    if (!s) return L"<no section>";
    for (auto& r : s->rows) if (r.label == label) return r.value;
    return L"<no row>";
}
}

LOGIC_TEST(fx_filter_noop_edit_has_no_effect) {
    FilterManager fm;
    fm.SetCapabilities(wvp::fakecaps::None());
    FilterSettings s;
    s.speed = 1.5;
    LT_CHECK(fx::HasEffect(fm.Apply(s)));
    LT_CHECK(!fx::HasEffect(fm.Apply(s)));   // a no-op edit must not reach the player
}

LOGIC_TEST(fx_filter_structure_changes_rebuild) {
    FilterManager fm;
    fm.SetCapabilities(wvp::fakecaps::None());
    FilterSettings s;
    s.deinterlace = wvp::DeinterlaceMode::On;
    s.deinterlacePriority = wvp::DeinterlacePriority::Quality;
    auto c = fm.Apply(s);
    LT_CHECK(c.videoRebuild);
    LT_CHECK(!c.audioRebuild);
    LT_CHECK(Has(c.videoFilter, "bwdif"));
    s.deinterlacePriority = wvp::DeinterlacePriority::Efficiency;
    c = fm.Apply(s);
    LT_CHECK(c.videoRebuild && Has(c.videoFilter, "yadif") && !Has(c.videoFilter, "bwdif"));
    s.normalize = true;
    c = fm.Apply(s);
    LT_CHECK(c.audioRebuild && !c.videoRebuild && Has(c.audioFilter, "dynaudnorm"));
    s.deinterlace = wvp::DeinterlaceMode::Ivtc;
    c = fm.Apply(s);
    LT_CHECK(c.videoRebuild && Has(c.videoFilter, "fieldmatch,bwdif=mode=send_frame:parity=auto:deint=interlaced,decimate") &&
             !Has(c.videoFilter, "yadif"));
}

LOGIC_TEST(fx_filter_speed_presets) {
    LT_CHECK_EQ(wvp::filters::kSpeedPresets.size(), (size_t)8);
    FilterManager fm;
    fm.SetCapabilities(wvp::fakecaps::None());
    for (size_t i = 0; i < wvp::filters::kSpeedPresets.size(); i++) {
        double sp = wvp::filters::kSpeedPresets[i];
        LT_CHECK_EQ(fx::SpeedIndex(sp), (int)i);
        FilterSettings s;
        s.speed = sp;
        auto c = fm.Apply(s);
        if (sp == 1.0) {
            LT_CHECK(fm.VideoFilter().empty() && fm.AudioFilter().empty());
        } else {
            LT_CHECK(c.videoRebuild && c.audioRebuild);
            LT_CHECK(Has(c.videoFilter, "setpts=PTS/"));
            LT_CHECK(Has(c.audioFilter, "atempo="));
        }
    }
    LT_CHECK_EQ(fx::SpeedIndex(1.1), -1);
    LT_CHECK_EQ(fx::SpeedLabel(0.25), std::wstring(L"0.25x"));
    LT_CHECK_EQ(fx::SpeedLabel(1.0), std::wstring(L"1x"));
    LT_CHECK_EQ(fx::SpeedLabel(1.5), std::wstring(L"1.5x"));
}

LOGIC_TEST(fx_filter_eq_presets) {
    const auto& ps = fx::EqPresets();
    LT_CHECK(ps.size() >= 6);
    LT_CHECK_EQ(std::wstring(ps[0].name), std::wstring(L"Flat"));
    FilterManager fm;
    fm.SetCapabilities(wvp::fakecaps::None());
    FilterSettings s;
    for (size_t i = 0; i < ps.size(); i++) {
        s = fx::WithEqPreset(s, (int)i);
        LT_CHECK_EQ(fx::MatchEqPreset(s.eqGainsDb), (int)i);
        for (double g : s.eqGainsDb) LT_CHECK(g >= -12 && g <= 12);
        fm.Apply(s);
        if (i == 0) LT_CHECK(fm.AudioFilter().empty());
        else LT_CHECK(Has(fm.AudioFilter(), "superequalizer"));
    }
    s.eqGainsDb[3] += 1;
    LT_CHECK_EQ(fx::MatchEqPreset(s.eqGainsDb), -1);
    // out of range index leaves the gains alone
    auto t = fx::WithEqPreset(s, 99);
    LT_CHECK(t.eqGainsDb == s.eqGainsDb);
    LT_CHECK_EQ(fx::BandLabel(31), std::wstring(L"31 Hz"));
    LT_CHECK_EQ(fx::BandLabel(1000), std::wstring(L"1 kHz"));
    LT_CHECK_EQ(fx::DbLabel(3.4), std::wstring(L"+3 dB"));
    LT_CHECK_EQ(fx::DbLabel(-6), std::wstring(L"-6 dB"));
    LT_CHECK_EQ(fx::DbLabel(0), std::wstring(L"0 dB"));
}

LOGIC_TEST(fx_deinterlace_group_control_visibility_per_mode) {
    using M = wvp::DeinterlaceMode;
    LT_CHECK(FilterSettings{}.deinterlace == M::Off);
    auto off = fx::DeinterlaceControlsFor(M::Off, false);
    LT_CHECK(!off.rate && !off.onlyInterlaced && !off.ivtcTuning);
    LT_CHECK(!off.priority);
    auto on = fx::DeinterlaceControlsFor(M::On, false);
    LT_CHECK(on.priority && on.rate && on.onlyInterlaced && !on.ivtcTuning);
    auto iv = fx::DeinterlaceControlsFor(M::Ivtc, false);
    LT_CHECK(!iv.priority && !iv.rate && !iv.onlyInterlaced && iv.ivtcTuning);
    LT_CHECK(on.enabled && iv.enabled && !fx::DeinterlaceControlsFor(M::On, true).enabled);
    const auto& labels = fx::DeinterlaceLabels();
    LT_CHECK_EQ(labels.size(), (size_t)3);
    LT_CHECK(labels[(int)M::Off] == L"Off" && labels[(int)M::On] == L"On" && labels[(int)M::Ivtc] == L"IVTC");
    const auto& pl = fx::DeinterlacePriorityLabels();
    LT_CHECK_EQ(pl.size(), (size_t)2);
    LT_CHECK(pl[(int)wvp::DeinterlacePriority::Efficiency] == L"Efficiency" && pl[(int)wvp::DeinterlacePriority::Quality] == L"Quality");
}

LOGIC_TEST(fx_deinterlace_ivtc_ignores_rate_and_only_interlaced_in_the_graph) {
    FilterManager fm;
    fm.SetCapabilities(wvp::fakecaps::None());
    FilterSettings s;
    s.deinterlace = wvp::DeinterlaceMode::Ivtc;
    s.deinterlaceRate = wvp::DeinterlaceRate::Field;
    s.deinterlaceOnlyInterlaced = true;
    auto c = fm.Apply(s);
    LT_CHECK(Has(c.videoFilter, "mode=send_frame") && !Has(c.videoFilter, "send_field") && !Has(c.videoFilter, "deint=all"));
    LT_CHECK(fm.Settings().deinterlaceRate == wvp::DeinterlaceRate::Frame && !fm.Settings().deinterlaceOnlyInterlaced);
}

LOGIC_TEST(fx_filter_reset_parts) {
    FilterManager fm;
    fm.SetCapabilities(wvp::fakecaps::None());
    FilterSettings s;
    s.deinterlace = wvp::DeinterlaceMode::On; s.deinterlacePriority = wvp::DeinterlacePriority::Quality;
    s.normalize = true; s.speed = 1.5; s.eqGainsDb[0] = 5; s.customVideoFilter = "hue=s=0";
    fm.Apply(s);
    auto v = fx::ResetVideoPart(s);
    LT_CHECK(v.deinterlace == wvp::DeinterlaceMode::Off && v.deinterlacePriority == wvp::DeinterlacePriority::Efficiency);
    LT_CHECK(v.normalize && v.speed == 1.5 && v.customVideoFilter == "hue=s=0");
    auto a = fx::ResetAudioPart(s);
    LT_CHECK(!a.normalize && a.speed == 1.0 && a.eqGainsDb[0] == 0 && a.deinterlace == wvp::DeinterlaceMode::On);
    auto c = fm.Apply(v);
    LT_CHECK(c.videoRebuild);
    LT_CHECK_EQ(fm.VideoFilter(), std::string("setpts=PTS/1.5,hue=s=0"));
    c = fm.Reset();
    LT_CHECK(c.videoRebuild && c.audioRebuild);
    LT_CHECK(fm.VideoFilter().empty() && fm.AudioFilter().empty());
    LT_CHECK(fm.Settings() == FilterSettings{});
}

LOGIC_TEST(fx_zoom_to_fill_crop) {
    std::string z = fx::ZoomCropFilter(16.0 / 9.0);
    LT_CHECK(Has(z, "crop@wvpzoom=w=min(iw\\,ih*1.7778/sar)"));
    LT_CHECK(Has(z, ":h=min(ih\\,iw*sar/1.7778)"));
    LT_CHECK_EQ(fx::StripZoom(z), std::string());
    LT_CHECK_EQ(fx::ComposeCustomVideo("hue=s=0", true, 1.0), "hue=s=0," + fx::ZoomCropFilter(1.0));
    LT_CHECK_EQ(fx::ComposeCustomVideo("hue=s=0", false, 1.0), std::string("hue=s=0"));
    LT_CHECK_EQ(fx::StripZoom("hue=s=0," + z), std::string("hue=s=0"));
    LT_CHECK_EQ(fx::StripZoom(z + ",hue=s=0"), std::string("hue=s=0"));
    LT_CHECK_EQ(fx::StripZoom("a," + z + ",b"), std::string("a,b"));
    // window aspect: strip height is excluded unless full screen
    LT_CHECK(std::abs(fx::VideoAreaAspect(1600, 932, 32, false) - 1600.0 / 900.0) < 1e-6);
    LT_CHECK(std::abs(fx::VideoAreaAspect(1920, 1080, 32, true) - 1920.0 / 1080.0) < 1e-6);
    LT_CHECK(std::abs(fx::VideoAreaAspect(0, 0, 32, true) - 16.0 / 9.0) < 1e-9);

    FilterManager fm;
    fm.SetCapabilities(wvp::fakecaps::None());
    FilterSettings user;
    fx::ZoomState zs{true, 16.0 / 9.0};
    auto c = fx::ApplyUser(fm, user, zs);
    LT_CHECK(c.videoRebuild && Has(c.videoFilter, "crop@wvpzoom"));
    LT_CHECK(fx::UserView(fm.Settings()).customVideoFilter.empty());   // panel never shows the crop
    LT_CHECK(!fx::HasEffect(fx::ApplyUser(fm, user, zs)));
    zs.aspect = 4.0 / 3.0;
    c = fx::ApplyUser(fm, user, zs);
    LT_CHECK(c.videoRebuild && Has(c.videoFilter, "1.3333"));
    zs.on = false;
    c = fx::ApplyUser(fm, user, zs);
    LT_CHECK(c.videoRebuild && c.videoFilter.empty());
}

LOGIC_TEST(fx_track_cycle_math_and_toast) {
    auto mk = [](std::initializer_list<bool> sel, bool sub) {
        std::vector<wvp::StreamInfo> v;
        int i = 0;
        for (bool s : sel) {
            wvp::StreamInfo si;
            si.Index = i++;
            si.Type = sub ? wvp::StreamType::Subtitle : wvp::StreamType::Audio;
            si.Selected = s;
            v.push_back(si);
        }
        return v;
    };
    auto a = mk({true, false, false}, false);
    auto s = mk({false, false}, true);
    a[1].Language = "eng";
    a[0].Selected = false; a[1].Selected = true;
    LT_CHECK_EQ(fx::TrackToast(wvp::StreamType::Audio, a), std::wstring(L"Audio: English (2/3)"));
    a[1].Title = "Director";
    LT_CHECK_EQ(fx::TrackToast(wvp::StreamType::Audio, a), std::wstring(L"Audio: Director (2/3)"));
    a[1].Selected = false; a[2].Selected = true;
    LT_CHECK_EQ(fx::TrackToast(wvp::StreamType::Audio, a), std::wstring(L"Audio: Track 3 (3/3)"));
    LT_CHECK_EQ(fx::TrackToast(wvp::StreamType::Subtitle, s), std::wstring(L"Subtitles: Off"));
    LT_CHECK_EQ(fx::TrackToast(wvp::StreamType::Subtitle, {}), std::wstring());
    LT_CHECK_EQ(fx::LanguageName("und"), std::wstring());
    LT_CHECK_EQ(fx::LanguageName("JPN"), std::wstring(L"Japanese"));
    LT_CHECK_EQ(fx::LanguageName("xyz"), std::wstring(L"XYZ"));
}

LOGIC_TEST(fx_next_subtitle_cycles_through_embedded_external_and_off) {
    auto mk = [](std::initializer_list<int> selectedPos, int n) {
        std::vector<wvp::StreamInfo> v;
        for (int i = 0; i < n; i++) {
            wvp::StreamInfo si;
            si.Index = 10 + i;
            si.Type = wvp::StreamType::Subtitle;
            si.external = i >= n - 2;
            si.path = si.external ? "C:\\v\\a.smi" : "";
            for (int p : selectedPos) si.Selected = si.Selected || p == i;
            v.push_back(si);
        }
        return v;
    };
    LT_CHECK(!fx::NextSubtitleIndex({}).has_value());
    LT_CHECK_EQ(*fx::NextSubtitleIndex(mk({}, 4)), 10);          // off -> first
    LT_CHECK_EQ(*fx::NextSubtitleIndex(mk({0}, 4)), 11);
    LT_CHECK_EQ(*fx::NextSubtitleIndex(mk({2}, 4)), 13);         // embedded -> external
    LT_CHECK_EQ(*fx::NextSubtitleIndex(mk({3}, 4)), -1);         // last external -> off
    auto withSuperimpose = mk({}, 2);
    withSuperimpose[0].AribType = "superimpose";
    LT_CHECK_EQ(*fx::NextSubtitleIndex(withSuperimpose), 11);

    auto streams = mk({3}, 4);
    auto ref = fx::CaptureTrackRef(streams, true);
    LT_CHECK(ref.kind == fx::TrackRef::Kind::External && ref.pathOrdinal == 1);
    auto reopened = mk({}, 3);                                   // one stream fewer: indexes shift, the path keys the track
    LT_CHECK_EQ(*fx::ResolveTrackRef(reopened, ref), 12);
    LT_CHECK_EQ(*fx::ResolveTrackRef(mk({}, 2), ref), 11);
    LT_CHECK(!fx::ResolveTrackRef(mk({}, 1), ref).has_value());
    LT_CHECK_EQ(*fx::ResolveTrackRef(reopened, fx::CaptureTrackRef(mk({}, 3), true)), -1);   // nothing selected = off
    LT_CHECK_EQ(*fx::ResolveTrackRef(reopened, fx::CaptureTrackRef(mk({1}, 3), true)), 11);
}

LOGIC_TEST(subtitle_selection_excludes_superimpose_and_toggle_covers_every_kind) {
    wvp::StreamInfo superimpose, caption, ordinary, caption2;
    superimpose.Type = caption.Type = ordinary.Type = caption2.Type = wvp::StreamType::Subtitle;
    superimpose.AribType = "superimpose";
    superimpose.Selected = true;  // selected in the separate layer, not in the cycleable subtitle list
    caption.AribType = "caption";
    caption2.AribType = "caption";
    caption2.Selected = true;
    auto streams = std::vector<wvp::StreamInfo>{superimpose, caption, ordinary, caption2};

    const auto cycleable = fx::CycleableSubtitles(streams);
    LT_CHECK_EQ(cycleable.size(), (size_t)3);
    wvp::MediaInfo media;
    media.Streams = streams;
    bool visible = true;
    LT_CHECK(fx::ToggleSubtitleVisibility(media, visible));
    LT_CHECK(!visible);
    LT_CHECK(fx::ToggleSubtitleVisibility(media, visible));
    LT_CHECK(visible);

    wvp::MediaInfo superimposeOnly;
    superimposeOnly.Streams = {superimpose};
    LT_CHECK(!fx::ToggleSubtitleVisibility(superimposeOnly, visible));
    LT_CHECK(visible);
}

LOGIC_TEST(fx_properties_model_from_json) {
    auto mi = wvp::MediaInfo::Parse(kSampleJson);
    LT_CHECK(mi.has_value());
    if (!mi) return;
    auto secs = fx::BuildProperties(*mi, std::int64_t(12345678));
    LT_CHECK_EQ(secs.size(), (size_t)4);
    auto* file = Sec(secs, L"File");
    LT_CHECK_EQ(Row(file, L"Title"), std::wstring(L"My Clip"));
    LT_CHECK_EQ(Row(file, L"Path"), std::wstring(L"C:\\media\\clip.mp4"));
    LT_CHECK_EQ(Row(file, L"Container"), std::wstring(L"mov,mp4,m4a,3gp,3g2,mj2"));
    LT_CHECK_EQ(Row(file, L"Duration"), std::wstring(L"1:02:05"));
    LT_CHECK_EQ(Row(file, L"Bit rate"), std::wstring(L"4.5 Mbps"));
    LT_CHECK_EQ(Row(file, L"Size"), std::wstring(L"11.8 MB (12,345,678 bytes)"));
    LT_CHECK_EQ(Row(file, L"Seekable"), std::wstring(L"Yes"));
    auto* v = Sec(secs, L"Video");
    LT_CHECK_EQ(Row(v, L"Codec"), std::wstring(L"h264"));
    LT_CHECK_EQ(Row(v, L"Resolution"), std::wstring(L"1920 x 1080"));
    LT_CHECK_EQ(Row(v, L"Pixel aspect (SAR)"), std::wstring(L"1:1"));
    LT_CHECK_EQ(Row(v, L"Display aspect (DAR)"), std::wstring(L"16:9"));
    LT_CHECK_EQ(Row(v, L"Frame rate"), std::wstring(L"29.97 fps"));
    LT_CHECK_EQ(Row(v, L"Pixel format"), std::wstring(L"yuv420p"));
    LT_CHECK_EQ(Row(v, L"Language"), std::wstring(L"<no row>"));   // "und" is not shown
    auto* a = Sec(secs, L"Audio");
    LT_CHECK_EQ(Row(a, L"Stream 1"), std::wstring(L"(playing)"));
    LT_CHECK_EQ(Row(a, L"Sample rate"), std::wstring(L"48 kHz"));
    LT_CHECK_EQ(Row(a, L"Channels"), std::wstring(L"Stereo"));
    LT_CHECK_EQ(Row(a, L"Language"), std::wstring(L"English"));
    LT_CHECK_EQ(Row(a, L"Stream title"), std::wstring(L"Commentary"));
    auto* sub = Sec(secs, L"Subtitles");
    LT_CHECK_EQ(Row(sub, L"Codec"), std::wstring(L"subrip"));
    LT_CHECK_EQ(Row(sub, L"Stream title"), std::wstring(L"Full"));
    wvp::MediaInfo u;
    u.Url = "https://example.com/a.m3u8";
    auto us = fx::BuildProperties(u, std::nullopt);
    LT_CHECK_EQ(us.size(), (size_t)1);
    LT_CHECK_EQ(Row(&us[0], L"URL"), std::wstring(L"https://example.com/a.m3u8"));
    LT_CHECK_EQ(Row(&us[0], L"Duration"), std::wstring(L"Unknown"));
    LT_CHECK_EQ(Row(&us[0], L"Size"), std::wstring(L"<no row>"));
}

LOGIC_TEST(fx_properties_formatting) {
    LT_CHECK_EQ(fx::FormatDurationText(7), std::wstring(L"0:07"));
    LT_CHECK_EQ(fx::FormatDurationText(125), std::wstring(L"2:05"));
    LT_CHECK_EQ(fx::FormatDurationText(0), std::wstring(L"Unknown"));
    LT_CHECK_EQ(fx::FormatBitRateText(128000), std::wstring(L"128 kbps"));
    LT_CHECK_EQ(fx::FormatFileSizeText(532), std::wstring(L"532 bytes"));
    LT_CHECK_EQ(fx::FormatFileSizeText(2048), std::wstring(L"2.0 KB (2,048 bytes)"));
    LT_CHECK_EQ(fx::FormatDar(720, 576, 16, 15), std::wstring(L"4:3"));   // PAL 4:3 anamorphic-ish
    LT_CHECK_EQ(fx::FormatDar(1920, 800, 1, 1), std::wstring(L"2.39:1"));
    LT_CHECK_EQ(fx::FormatSar(32, 27), std::wstring(L"32:27"));
    LT_CHECK_EQ(fx::FormatSar(0, 0), std::wstring(L"1:1"));
    LT_CHECK_EQ(fx::FormatFrameRate(25), std::wstring(L"25 fps"));
    LT_CHECK_EQ(fx::FormatChannels(6), std::wstring(L"5.1"));
    LT_CHECK_EQ(fx::FormatSampleRate(44100), std::wstring(L"44.1 kHz"));
    LT_CHECK(fx::IsLocalPath(L"D:\\a\\b.mkv"));
    LT_CHECK(fx::IsLocalPath(L"\\\\server\\share\\a.mkv"));
    LT_CHECK(!fx::IsLocalPath(L"http://x/y"));
}

LOGIC_TEST(fx_properties_layout_wraps_values) {
    auto mi = wvp::MediaInfo::Parse(kSampleJson);
    LT_CHECK(mi.has_value());
    if (!mi) return;
    auto secs = fx::BuildProperties(*mi, std::nullopt);
    pui::RecordingRenderer r;
    auto wide = fx::LayoutProps(secs, r, 520);
    auto narrow = fx::LayoutProps(secs, r, 240);
    LT_CHECK_EQ(wide.size(), narrow.size());
    size_t rows = 0;
    for (auto& s : secs) rows += s.rows.size();
    LT_CHECK_EQ(wide.size(), rows + secs.size());
    float prevEnd = 0;
    for (auto& ln : wide) { LT_CHECK(ln.y >= prevEnd - 0.001f); LT_CHECK(ln.h > 0); prevEnd = ln.y + ln.h; }
    LT_CHECK(fx::PropsTotalHeight(narrow) >= fx::PropsTotalHeight(wide));
    LT_CHECK_EQ(fx::PropsTotalHeight({}), 0.f);
}

LOGIC_TEST(main_menu_structure_and_state) {
    using namespace app::menus;
    MenuState s;
    s.hasMedia = true;
    s.speed = 1.5;
    s.zoom = true;
    s.alwaysOnTop = wvp::AlwaysOnTop::WhenPlaying;
    s.bar.shuffle = true; s.bar.repeat = 1; s.bar.hasPrev = false; s.bar.hasNext = true;
    wvp::StreamInfo a1, a2, s1;
    a1.Type = a2.Type = wvp::StreamType::Audio; a1.Language = "eng"; a2.Language = "jpn"; a2.Selected = true;
    a1.Index = 1; a2.Index = 2; s1.Index = 3;
    s1.Type = wvp::StreamType::Subtitle; s1.Language = "eng";
    s.audio = {a1, a2};
    s.subs = {s1};
    std::vector<MenuCmd> got;
    auto items = BuildMenu(s, [&](const MenuCmd& c) { got.push_back(c); });
    auto find = [&](std::vector<pui::MenuItemDesc>& v, const wchar_t* text) -> pui::MenuItemDesc* {
        for (auto& i : v) if (i.text == text) return &i;
        return nullptr;
    };
    // open / close, the submenus, panels, exit
    for (const wchar_t* t : {L"Open file(s)…", L"Open folder…", L"Open URL…", L"Close", L"Playback", L"Subtitles",
                             L"Video", L"Audio track", L"Always on top", L"Equalizer…", L"Copy frame", L"Properties…", L"Playlist panel",
                             L"Control panel", L"Settings", L"Exit"})
        LT_CHECK(find(items, t) != nullptr);
    LT_CHECK(find(items, L"Go to current playlist") == nullptr && find(items, L"Playlists") == nullptr && find(items, L"Full screen") == nullptr);
    LT_CHECK_EQ(find(items, L"Open folder…")->accelerator, std::wstring(L"F2"));
    LT_CHECK_EQ(find(items, L"Close")->accelerator, std::wstring(L"F4"));
    LT_CHECK_EQ(find(items, L"Playlist panel")->accelerator, std::wstring(L"F6"));
    LT_CHECK_EQ(find(items, L"Settings")->accelerator, std::wstring(L"F5"));
    LT_CHECK_EQ(find(items, L"Copy frame")->accelerator, std::wstring(L"Ctrl+Shift+C"));
    find(items, L"Open folder…")->onInvoke();
    LT_CHECK(got.back().Command() == CommandId::OpenFolder);
    find(items, L"Settings")->onInvoke();
    LT_CHECK(got.back().Command() == CommandId::OpenSettings);

    auto* pb = find(items, L"Playback");
    LT_CHECK(pb && pb->kind == pui::MenuItemKind::SubMenu);
    if (pb) {
        auto* speed = find(pb->sub, L"Speed");
        LT_CHECK(speed && speed->kind == pui::MenuItemKind::SubMenu && speed->sub.size() == 8);
        int checked = 0;
        for (auto& i : speed->sub) { LT_CHECK(i.kind == pui::MenuItemKind::Radio); if (i.checked) { checked++; LT_CHECK_EQ(i.text, std::wstring(L"1.5x")); } }
        LT_CHECK_EQ(checked, 1);
        speed->sub[0].onInvoke();
        LT_CHECK(got.back().cmd == Cmd::SetSpeed && got.back().value == 0.25);
        auto* shuffle = find(pb->sub, L"Shuffle");
        auto* repeat = find(pb->sub, L"Repeat");
        LT_CHECK(shuffle && shuffle->kind == pui::MenuItemKind::Check && shuffle->checked);
        LT_CHECK(repeat && repeat->sub.size() == 3 && repeat->sub[1].checked);
        shuffle->onToggled(false);
        LT_CHECK(got.back().cmd == Cmd::SetShuffle && !got.back().flag);
        repeat->sub[2].onInvoke();
        LT_CHECK(got.back().cmd == Cmd::SetRepeat && got.back().index == 2);
        LT_CHECK(!find(pb->sub, L"Previous file")->enabled && find(pb->sub, L"Next file")->enabled);
        find(pb->sub, L"Mini player")->onInvoke();
        LT_CHECK(got.back().Command() == CommandId::ToggleMiniPlayer);
        find(pb->sub, L"Forward 5 seconds")->onInvoke();
        LT_CHECK(got.back().Command() == CommandId::SeekForward5);
    }

    auto* aot = find(items, L"Always on top");
    LT_CHECK(aot && aot->kind == pui::MenuItemKind::SubMenu && aot->sub.size() == 6);
    if (aot && aot->sub.size() == 6) {
        for (int i = 0; i < 4; i++) LT_CHECK(aot->sub[(size_t)i].kind == pui::MenuItemKind::Radio && aot->sub[(size_t)i].checked == (i == 2));
        aot->sub[3].onInvoke();
        LT_CHECK(got.back().cmd == Cmd::SetAlwaysOnTop && got.back().index == 3);
        auto* cycle = find(aot->sub, L"Cycle always on top");
        LT_CHECK(cycle && cycle->accelerator == L"Ctrl+T");
        if (cycle) {
            cycle->onInvoke();
            LT_CHECK(got.back().Command() == CommandId::CycleAlwaysOnTop);
        }
    }

    auto* at = find(items, L"Audio track");
    LT_CHECK(at && at->kind == pui::MenuItemKind::SubMenu && at->sub.size() == 2);
    LT_CHECK(!at->sub[0].checked && at->sub[1].checked);
    LT_CHECK_EQ(at->sub[1].text, std::wstring(L"Japanese"));
    at->sub[0].onInvoke();
    LT_CHECK(got.back().cmd == Cmd::SelectAudio && got.back().index == 1);

    auto* st = find(items, L"Subtitles");
    LT_CHECK(st && st->sub.size() == 6);
    LT_CHECK_EQ(st->sub[0].text, std::wstring(L"Off"));
    LT_CHECK(st->sub[0].checked && !st->sub[1].checked);
    st->sub[1].onInvoke();
    LT_CHECK(got.back().cmd == Cmd::SelectSubtitle && got.back().index == 3);
    LT_CHECK(find(st->sub, L"Load subtitle file…") && find(st->sub, L"Show subtitles") && find(st->sub, L"Sync"));
    if (auto* show = find(st->sub, L"Show subtitles")) {
        show->onToggled(false);
        LT_CHECK(got.back().cmd == Cmd::SetSubtitleVisible && !got.back().flag);
    }
    if (auto* sync = find(st->sub, L"Sync")) {
        LT_CHECK_EQ(sync->sub.size(), (size_t)3);
        sync->sub[0].onInvoke();
        LT_CHECK(got.back().Command() == CommandId::SubtitleDelayLater);
        sync->sub[1].onInvoke();
        LT_CHECK(got.back().Command() == CommandId::SubtitleDelayEarlier);
        sync->sub[2].onInvoke();
        LT_CHECK(got.back().Command() == CommandId::SubtitleDelayReset);
    }

    auto* vs = find(items, L"Video");
    LT_CHECK(vs && vs->kind == pui::MenuItemKind::SubMenu);
    LT_CHECK(!find(vs->sub, L"Rotate") && !find(vs->sub, L"Flip horizontally") && !find(vs->sub, L"Flip vertically"));
    LT_CHECK(find(vs->sub, L"Zoom to fill")->checked);
    find(vs->sub, L"Zoom to fill")->onToggled(false);
    LT_CHECK(got.back().cmd == Cmd::Zoom && !got.back().flag);
    find(vs->sub, L"Enhancements…")->onInvoke();
    LT_CHECK(got.back().Command() == CommandId::VideoEnhancements);

    MenuState e;
    e.fullscreen = true;
    e.mini = true;
    auto items2 = BuildMenu(e, {});
    LT_CHECK(!find(items2, L"Copy frame")->enabled && !find(items2, L"Close")->enabled && !find(items2, L"Properties…")->enabled);
    LT_CHECK(!find(items2, L"Audio track")->enabled && !find(items2, L"Subtitles")->enabled && !find(items2, L"Playlist panel")->enabled);
    LT_CHECK(find(items2, L"Audio track")->kind == pui::MenuItemKind::Item);
    LT_CHECK(find(items2, L"Open file(s)…")->enabled && find(items2, L"Exit")->enabled);
    auto* pb2 = find(items2, L"Playback");
    LT_CHECK(pb2 && find(pb2->sub, L"Exit mini player") && !find(pb2->sub, L"Mini player"));
}

LOGIC_TEST(fx_subtitle_menu_lists_superimpose_as_nonselectable_and_maps_cycle_indexes) {
    using namespace app::menus;
    MenuState state;
    wvp::StreamInfo superimpose, caption, caption2;
    superimpose.Type = caption.Type = caption2.Type = wvp::StreamType::Subtitle;
    superimpose.Title = "Superimpose";
    superimpose.AribType = "superimpose";
    caption.Title = "Caption 1";
    caption.AribType = "caption";
    caption.Selected = true;
    caption2.Title = "Caption 2";
    caption2.AribType = "caption";
    superimpose.Index = 5; caption.Index = 6; caption2.Index = 7;
    caption2.external = true;
    caption2.path = "C:\\v\\clip.srt";
    state.hasMedia = true;
    state.subs = {superimpose, caption, caption2};
    std::vector<MenuCmd> got;
    auto sub = BuildSubtitleItems(state, [&](const MenuCmd& c) { got.push_back(c); });
    LT_CHECK_EQ(sub.size(), (size_t)8);
    if (sub.size() != 8) return;
    LT_CHECK(sub[0].checked == false);
    LT_CHECK_EQ(sub[1].text, std::wstring(L"Superimpose (superimpose)"));
    LT_CHECK(sub[1].kind == pui::MenuItemKind::Item && !sub[1].enabled);
    LT_CHECK(sub[2].checked);
    sub[2].onInvoke();
    LT_CHECK(!got.empty() && got.back().cmd == Cmd::SelectSubtitle && got.back().index == 6);
    LT_CHECK_EQ(sub[3].text, std::wstring(L"clip.srt (Caption 2)"));
    sub[3].onInvoke();
    LT_CHECK(got.back().cmd == Cmd::SelectSubtitle && got.back().index == 7);

    state.subs = {superimpose};
    auto onlySuperimpose = BuildSubtitleItems(state, {});
    LT_CHECK(onlySuperimpose.size() == 6);
    if (onlySuperimpose.size() == 6) LT_CHECK(!onlySuperimpose[0].enabled && !onlySuperimpose[1].enabled);
    state.hasMedia = false;
    LT_CHECK(BuildSubtitleItems(state, {}).empty());
}

LOGIC_TEST(fx_native_menu_is_recorded_not_shown_in_tests) {
    menus::MenuState s;
    s.hasMedia = true;
    auto items = menus::BuildMenu(s, {});
    LT_CHECK(items.size() > 10);
    nativemenu::ClearLastShown();
    LT_CHECK(OsDialogsBlocked());
    LT_CHECK(!nativemenu::ShowAtPoint(items, pui::PointF{100, 100}));   // never a visible OS menu in tests
    LT_CHECK(!nativemenu::IsOpen());
    LT_CHECK(nativemenu::LastShown().size() == items.size());
}

LOGIC_TEST(fx_filters_persist_and_filter_error_store) {
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() / (L"aribplayer-fx-test-" + std::to_wstring(GetCurrentProcessId()));
    fs::create_directories(dir);
    {
        Services svc(dir.wstring());
        FilterSettings d;
        d.normalize = true; d.speed = 1.25; d.deinterlace = wvp::DeinterlaceMode::On; d.customVideoFilter = "hue=s=0";
        svc.filters.Apply(d);
        svc.settings.filters = svc.filters.Settings();
        LT_CHECK(svc.settingsStore.Save(svc.settings));
        Services svc2(dir.wstring());
        svc2.settings = svc2.settingsStore.Load();
        LT_CHECK(svc2.settings.filters == svc.settings.filters);
        // startup applies the saved state before the first Open
        svc2.filters.Apply(svc2.settings.filters);
        svc2.filters.SetCapabilities(wvp::fakecaps::None());
        LT_CHECK(Has(svc2.filters.VideoFilter(), "yadif"));
        LT_CHECK(Has(svc2.filters.Provider().audioFilter(), "dynaudnorm"));
    }
    std::error_code ec;
    fs::remove_all(dir, ec);

    int events = 0;
    auto tok = fx::FilterErrorChanged().add([&] { events++; });
    fx::ClearFilterError();
    LT_CHECK(!fx::LastFilterError().has_value());
    wvp::FilterErrorInfo e;
    e.Type = "video"; e.Filter = "bogus"; e.Code = -22; e.Message = "Invalid argument";
    fx::SetFilterError(e);
    LT_CHECK(fx::LastFilterError().has_value());
    LT_CHECK_EQ(fx::FormatFilterError(e), std::wstring(L"Video filter failed: Invalid argument"));
    e.Type = "audio";
    LT_CHECK(Has(fx::FormatFilterError(e), L"Audio filter failed"));
    fx::ClearFilterError();
    LT_CHECK(!fx::LastFilterError().has_value());
    LT_CHECK_EQ(events, 2);
    fx::FilterErrorChanged().remove(tok);
}

LOGIC_TEST(fx_filter_error_text_maps_averror_codes) {
    wvp::FilterErrorInfo e;
    e.Type = "video"; e.Filter = "bogus_filter"; e.Code = -1279870712; e.Message = "";
    LT_CHECK_EQ(fx::FormatFilterError(e), std::wstring(L"Video filter failed: Filter not found ('bogus_filter')"));
    e.Message = "Filter not found";   // the engine text is replaced by the same table text, never duplicated
    LT_CHECK_EQ(fx::FormatFilterError(e), std::wstring(L"Video filter failed: Filter not found ('bogus_filter')"));
    e.Filter = "[in]bogus_filter=a=1:b=2[out]";
    LT_CHECK_EQ(fx::FirstFilterName(e.Filter), std::string("bogus_filter"));
    e.Filter = "eq=brightness=1,bogus_filter";
    LT_CHECK_EQ(fx::FormatFilterError(e), std::wstring(L"Video filter failed: Filter not found"));   // culprit unknown in a chain
    e.Code = -(int)(0xF8u | ((unsigned)'O' << 8) | ((unsigned)'P' << 16) | ((unsigned)'T' << 24));
    e.Filter = "scale=bogus=1";
    LT_CHECK_EQ(fx::FormatFilterError(e), std::wstring(L"Video filter failed: Option not found ('scale')"));
    e.Type = "audio"; e.Code = -22; e.Filter = "volume=";
    LT_CHECK_EQ(fx::FormatFilterError(e), std::wstring(L"Audio filter failed: Invalid argument"));
    e.Code = -12345678; e.Message = "Weird";
    LT_CHECK_EQ(fx::FormatFilterError(e), std::wstring(L"Audio filter failed: Weird"));
    e.Message = "";
    LT_CHECK_EQ(fx::FormatFilterError(e), std::wstring(L"Audio filter failed (error -12345678)"));
    LT_CHECK_EQ(fx::AvErrorText(-1279870712), std::wstring(L"Filter not found"));
    LT_CHECK_EQ(fx::AvErrorText(-5), std::wstring(L"Input/output error"));
    LT_CHECK(fx::AvErrorText(0).empty());
}

LOGIC_TEST(fx_menu_arib_audio_is_a_preference_pair) {
    using namespace app::menus;
    MenuState s;
    s.hasMedia = true;
    wvp::StreamInfo a1, a2;
    a1.Type = a2.Type = wvp::StreamType::Audio; a1.Index = 1; a2.Index = 2; a1.Language = a2.Language = "jpn";
    s.audio = {a1, a2};
    s.aribAudio = true;
    s.arib1 = a1; s.arib1Present = true;
    s.arib2 = a2; s.arib2Present = false;          // audio 2 not in the file right now: still listed and selectable
    s.aribPref = 0;
    std::vector<MenuCmd> got;
    auto items = BuildMenu(s, [&](const MenuCmd& c) { got.push_back(c); });
    pui::MenuItemDesc* at = nullptr;
    for (auto& i : items) if (i.text == L"Audio track") at = &i;
    LT_CHECK(at && at->kind == pui::MenuItemKind::SubMenu && at->sub.size() == 2);
    LT_CHECK(at->sub[0].checked && !at->sub[1].checked);
    LT_CHECK(at->sub[0].enabled && at->sub[1].enabled);
    LT_CHECK_EQ(at->sub[0].text, std::wstring(L"Japanese"));
    LT_CHECK_EQ(at->sub[1].text, std::wstring(L"Track 2"));      // no stream for audio 2 yet
    at->sub[1].onInvoke();
    LT_CHECK(got.back().cmd == Cmd::SelectAribAudio && got.back().index == 1);
    at->sub[0].onInvoke();
    LT_CHECK(got.back().cmd == Cmd::SelectAribAudio && got.back().index == 0);
    s.arib2Present = true;                             // two equal names are told apart
    items = BuildMenu(s, {});
    for (auto& i : items) if (i.text == L"Audio track") at = &i;
    LT_CHECK_EQ(at->sub[0].text, std::wstring(L"Japanese (Audio 1)"));
    LT_CHECK_EQ(at->sub[1].text, std::wstring(L"Japanese (Audio 2)"));
    s.aribPref = 1;                                   // the check mark follows the selection, whatever plays
    s.arib2Present = true;
    s.arib2.Language = "eng";
    items = BuildMenu(s, {});
    for (auto& i : items) if (i.text == L"Audio track") at = &i;
    LT_CHECK(!at->sub[0].checked && at->sub[1].checked);
    LT_CHECK_EQ(at->sub[0].text, std::wstring(L"Japanese"));
    LT_CHECK_EQ(at->sub[1].text, std::wstring(L"English"));
}

LOGIC_TEST(media_info_parses_arib_audio_roles) {
    auto mi = wvp::MediaInfo::Parse(R"({"streams":[{"index":0,"type":"video"},{"index":1,"type":"audio"},{"index":2,"type":"audio"}],"arib_audio":{"audio1":1,"audio2":-1}})");
    LT_CHECK(mi && mi->AribAudio.Present && mi->AribAudio.Audio1 == 1 && mi->AribAudio.Audio2 == -1);
    auto plain = wvp::MediaInfo::Parse(R"({"streams":[{"index":0,"type":"audio"}]})");
    LT_CHECK(plain && !plain->AribAudio.Present);
}
