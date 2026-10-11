// Shell --logictest cases (headless, no window). Feature modules register their own cases with LOGIC_TEST.
#include <cmath>
#include <cstdlib>
#include <set>

#include "BarLogic.h"
#include "Commands.h"
#include "WindowSizing.h"
#include "WindowSnap.h"
#include "wvp/FilterSettings.h"
#include "LogicTest.h"
#include "NullShellContext.h"
#include "ShellLogic.h"
#include "features/SettingsLogic.h"
#include "shell/ShellRoot.h"

using namespace app;

namespace {
bool Inside(const pui::RectF& r, float w, float h) {
    const float e = 0.01f;
    return !r.Empty() && r.l >= -e && r.t >= -e && r.r <= w + e && r.b <= h + e;
}
bool InsidePx(const RectPx& r, int w, int h) { return !r.Empty() && r.l >= 0 && r.t >= 0 && r.r <= w && r.b <= h; }
const float kScales[] = {1.0f, 1.25f, 1.5f, 2.0f, 3.0f};
const int kSizesDip[][2] = {{500, 374}, {800, 450}, {1280, 720}, {1920, 1080}, {3840, 2160}};
}

LOGIC_TEST(time_format) {
    LT_CHECK(FormatTime(0) == L"00:00");
    LT_CHECK(FormatTime(59) == L"00:59");
    LT_CHECK(FormatTime(60) == L"01:00");
    LT_CHECK(FormatTime(3599) == L"59:59");
    LT_CHECK(FormatTime(3600) == L"1:00:00");
    LT_CHECK(FormatTime(3661) == L"1:01:01");
    LT_CHECK(FormatTime(43200) == L"12:00:00");
    LT_CHECK(FormatTime(std::nan("")) == L"00:00");
    LT_CHECK(FormatTime(-1) == L"00:00");
    LT_CHECK(FormatTime(429.9) == L"07:09");
}

LOGIC_TEST(volume_glyph_mapping) {
    LT_CHECK(std::wstring(VolumeGlyph(0, true)) == L"");
    LT_CHECK(std::wstring(VolumeGlyph(80, true)) == L"");
    LT_CHECK(std::wstring(VolumeGlyph(0, false)) == L"");
    LT_CHECK(std::wstring(VolumeGlyph(1, false)) == L"");
    LT_CHECK(std::wstring(VolumeGlyph(33, false)) == L"");
    LT_CHECK(std::wstring(VolumeGlyph(34, false)) == L"");
    LT_CHECK(std::wstring(VolumeGlyph(66, false)) == L"");
    LT_CHECK(std::wstring(VolumeGlyph(67, false)) == L"");
    LT_CHECK(std::wstring(VolumeGlyph(100, false)) == L"");
}

namespace {
const std::vector<float> kTestChips = {38.f, 52.f, 44.f, 34.f};
}

LOGIC_TEST(bar_layout_matches_dock_metrics) {
    BarLayout b;
    b.Compute(1280, 1.f, kTestChips);
    LT_CHECK_EQ(b.H, 86.f);
    LT_CHECK_EQ(b.btn[B_Play].w(), 40.f);
    LT_CHECK_EQ(b.btn[B_Play].h(), 40.f);
    LT_CHECK_EQ(b.btn[B_Play].l, 12.f);
    LT_CHECK_EQ(b.btn[B_Play].t, 36.f);                                  // 10 + 20 seek row + 6 pad
    LT_CHECK_EQ(b.btn[B_Stop].l - b.btn[B_Play].l, 42.f);                // 40 + 2 gap
    LT_CHECK_EQ(b.btn[B_Full].r, 1280.f - 12.f);
    LT_CHECK_EQ(b.btn[B_Subs].l - b.btn[B_Mute].l, 40.f + 2 + 96 + 8 + 2);   // mute, 96 slider + 8 margin, subtitle
    LT_CHECK_EQ(b.elapsed.l, 16.f);
    LT_CHECK_EQ(b.seek.t, 10.f);
    LT_CHECK_EQ(b.seek.h(), 20.f);
    LT_CHECK_EQ(b.total.r, 1280.f - 16.f);
    LT_CHECK_EQ(b.seek.l - b.elapsed.r, 12.f - 10.f);                    // 12 gap, the slider overhangs its track by half a thumb
    LT_CHECK_EQ(b.stroke.h(), 1.f);
    LT_CHECK_EQ(b.visibleChips, 4);
    LT_CHECK_EQ(b.chip[0].l, b.btn[B_Open].r + 16.f);                    // 8 group gap + 8 chip group padding
    LT_CHECK_EQ(b.chip[0].h(), 22.f);
    LT_CHECK_EQ(b.chip[1].l - b.chip[0].r, 6.f);
    LT_CHECK_EQ(b.volume.w(), 96.f);
    LT_CHECK_EQ(b.volume.h(), 20.f);
    LT_CHECK(b.tier == BarTier::Full);
    for (int i = 0; i < B_Count; i++) LT_CHECK(b.buttonVisible[i]);
}

LOGIC_TEST(bar_layout_tiers_hide_in_order_and_never_overlap) {
    for (float s : {1.f, 1.5f, 2.f}) {
        int prevChips = -1;
        for (int wdip = 400; wdip <= 1400; wdip += 3) {
            BarLayout b;
            b.Compute((float)wdip, s, kTestChips);
            LT_CHECK(b.Sane());
            LT_CHECK(b.visibleChips >= prevChips);                         // widening never hides more chips
            prevChips = b.visibleChips;
            for (int i = 0; i < b.chipCount; i++) LT_CHECK(b.chipVisible[i] == (i < b.visibleChips));   // chips hide last-first
            if (b.tier != BarTier::Full) LT_CHECK_EQ(b.visibleChips, 0);       // chips go before the volume slider shrinks
            if (b.tier == BarTier::Compact) {
                LT_CHECK(!b.buttonVisible[B_Open] && !b.buttonVisible[B_Subs] && !b.buttonVisible[B_Control]);
                LT_CHECK(b.buttonVisible[B_Panel] && b.buttonVisible[B_Full] && b.buttonVisible[B_Mute]);
            } else {
                LT_CHECK(b.buttonVisible[B_Open] && b.buttonVisible[B_Subs] && b.buttonVisible[B_Control]);
            }
            if (b.tier == BarTier::VolumeNarrow) LT_CHECK_EQ(b.volume.w(), b.R(64));
            if (b.tier == BarTier::Full) LT_CHECK_EQ(b.volume.w(), b.R(96));
        }
    }
    BarLayout none;
    none.Compute(1280, 1.f, {});
    LT_CHECK(none.Sane() && none.visibleChips == 0 && none.tier == BarTier::Full);
}

LOGIC_TEST(shell_layout_all_modes_scales_sizes) {
    for (float s : kScales)
        for (auto& sz : kSizesDip)
            for (ShellMode mode : {ShellMode::NowPlaying, ShellMode::FullScreen, ShellMode::MiniPlayer})
                {
                    ShellLayoutIn in;
                    in.cwPx = (int)std::lround(sz[0] * s);
                    in.chPx = (int)std::lround(sz[1] * s);
                    in.scale = s;
                    in.mode = mode;
                    in.panelWanted = true;
                    ShellLayout l = ComputeShellLayout(in);
                    float W = l.widthDip, H = l.heightDip;
                    LT_CHECK(Inside(l.bar, W, H));
                    LT_CHECK(mode == ShellMode::MiniPlayer || std::fabs(l.bar.h() - kDockDip) < 1.0f);
                    LT_CHECK_EQ(l.bar.b, H);
                    LT_CHECK(InsidePx(l.barPx, in.cwPx, in.chPx));
                    if (mode == ShellMode::FullScreen) {
                        LT_CHECK(Inside(l.title, W, H));
                        LT_CHECK_EQ(l.title.l, 0.f);
                        LT_CHECK_EQ(l.title.t, 0.f);
                        LT_CHECK_EQ(l.title.w(), W);
                        LT_CHECK_EQ(l.title.h(), kStripDip);
                        LT_CHECK(InsidePx(l.video, in.cwPx, in.chPx));
                        LT_CHECK_EQ(l.video.l, 0);
                        LT_CHECK_EQ(l.video.t, 0);
                        LT_CHECK_EQ(l.video.w(), in.cwPx);
                        LT_CHECK_EQ(l.video.h(), in.chPx);
                        LT_CHECK(l.barOverlaysVideo && l.titleSlides);
                        LT_CHECK(l.panel.Empty() && l.panelPx.Empty() && !l.panelOverlay);
                    } else if (mode == ShellMode::NowPlaying) {
                        // docked: title strip | video [+ panel] | bar, nothing overlapping
                        LT_CHECK(Inside(l.title, W, H));
                        LT_CHECK(InsidePx(l.video, in.cwPx, in.chPx));
                        LT_CHECK_EQ(l.video.t, (int)std::lround(kStripDip * s));
                        LT_CHECK(l.video.b <= l.barPx.t);
                        LT_CHECK(!l.barOverlaysVideo && !l.titleSlides);
                        if (!l.panelPx.Empty()) {
                            LT_CHECK(InsidePx(l.panelPx, in.cwPx, in.chPx));
                            LT_CHECK(l.video.r < l.panelPx.l && l.panelPx.l >= l.barPx.r && l.panelPx.b <= in.chPx);
                            LT_CHECK_EQ(l.barPx.l, l.video.l);                       // the dock sits under the video column only
                            LT_CHECK_EQ(l.barPx.r, l.video.r);
                        }
                    } else {
                        LT_CHECK(Inside(l.title, W, H));
                        LT_CHECK(InsidePx(l.video, in.cwPx, in.chPx));
                        LT_CHECK_EQ(l.video.t, (int)std::lround(kStripDip * s));
                        LT_CHECK_EQ(l.video.b, in.chPx);
                        LT_CHECK(l.barOverlaysVideo && !l.titleSlides);
                    }
                }
}

LOGIC_TEST(shell_layout_docked_body_row_video_and_panel) {
    for (float s : {1.f, 1.5f, 2.f}) {
        auto layoutFor = [&](int wdip, bool wanted, ShellMode mode = ShellMode::NowPlaying) {
            ShellLayoutIn in;
            in.cwPx = (int)std::lround(wdip * s); in.chPx = (int)std::lround(760 * s); in.scale = s; in.mode = mode;
            in.panelWanted = wanted;
            return ComputeShellLayout(in);
        };
        const int strip = (int)std::lround(kStripDip * s), dock = (int)std::lround(kDockDip * s), gut = (int)std::lround(8 * s);
        ShellLayout l = layoutFor(1280, true);
        const int H = (int)std::lround(760 * s), W = (int)std::lround(1280 * s);
        LT_CHECK_EQ(l.barPx.t, H - dock);
        LT_CHECK_EQ(l.video.l, gut);
        LT_CHECK_EQ(l.video.t, strip);
        LT_CHECK_EQ(l.video.b, H - dock);                                  // the video never reaches into the dock
        LT_CHECK_EQ(l.panelPx.r, W - gut);
        LT_CHECK_EQ(l.panelPx.t, strip);
        LT_CHECK_EQ(l.panelPx.b, H - gut);                                 // the panel spans the full body height
        LT_CHECK_EQ(l.barPx.l, l.video.l);                                 // the dock sits under the video column only
        LT_CHECK_EQ(l.barPx.r, l.video.r);
        LT_CHECK_EQ(l.barPx.b, H);
        LT_CHECK_EQ(l.panelPx.l - l.video.r, gut);                        // 8 DIP gap
        LT_CHECK(std::fabs(l.video.w() / s - 936.f) <= 1.5f);              // row 1264 - gap 8 - panel 320
        LT_CHECK_EQ(l.panelPx.w(), (int)std::lround(320 * s));
        LT_CHECK(!l.panelOverlay && !l.panel.Empty());
        // the splitter is the gap between video and panel, over the panel column's height
        LT_CHECK(l.splitterPx.l == l.video.r && l.splitterPx.r == l.panelPx.l && l.splitterPx.t == strip && l.splitterPx.b == l.panelPx.b);
        // window resizing changes only the video: the panel keeps its width
        ShellLayout mid = layoutFor(856, true);
        LT_CHECK(!mid.panelPx.Empty() && !mid.panelOverlay);
        LT_CHECK_EQ(mid.panelPx.w(), (int)std::lround(320 * s));
        LT_CHECK_EQ(mid.video.w(), mid.panelPx.l - gut - mid.video.l);
        // a user width is honoured, clamped to [kPanelMinDip, row - gap - kVideoMinDockDip]
        auto widthFor = [&](int wdip, float userDip) {
            ShellLayoutIn in;
            in.cwPx = (int)std::lround(wdip * s); in.chPx = (int)std::lround(760 * s); in.scale = s; in.mode = ShellMode::NowPlaying;
            in.panelWanted = true; in.panelWidthDip = userDip;
            return ComputeShellLayout(in).panelPx.w();
        };
        LT_CHECK_EQ(widthFor(1280, 450), (int)std::lround(450 * s));
        LT_CHECK_EQ(widthFor(1280, 100), (int)std::lround(kPanelMinDip * s));
        LT_CHECK_EQ(widthFor(1280, 5000), (int)std::lround(1264 * s) - (int)std::lround(kBodyGapDip * s) - (int)std::lround(kVideoMinDockDip * s));
        // ... until even the minimum panel leaves the video below kVideoMinDockDip: the panel may only open as an overlay
        ShellLayout narrow = layoutFor(503, true);
        LT_CHECK(narrow.panelPx.Empty() && narrow.panel.Empty() && narrow.splitterPx.Empty() && narrow.panelOverlay);
        LT_CHECK_EQ(narrow.video.w(), (int)std::lround(503 * s) - 2 * gut);
        ShellLayout edge = layoutFor(504, true);
        LT_CHECK(!edge.panelPx.Empty() && !edge.panelOverlay);
        ShellLayout off = layoutFor(1280, false);
        LT_CHECK(off.panelPx.Empty() && off.splitterPx.Empty() && !off.panelOverlay);
        LT_CHECK_EQ(off.video.w(), W - 2 * gut);
        ShellLayout fs = layoutFor(1280, true, ShellMode::FullScreen);
        LT_CHECK(fs.panelPx.Empty() && fs.splitterPx.Empty() && !fs.panelOverlay);
    }
}

LOGIC_TEST(title_layout_zones_and_snap_button) {
    for (int dpi : {96, 144}) {
        const float s = dpi / 96.f;
        const float W = 1280;
        TitleLayoutIn in;
        in.widthDip = W;
        // windowed player: [Menu] name ... [Pin][Min][Max][Close], the buttons 46 x 44 DIP flush right
        TitleLayout w = TitleLayout::Compute(in);
        LT_CHECK_EQ(w.close.r, W);
        LT_CHECK_EQ(w.close.w(), 46.f);
        LT_CHECK_EQ(w.close.h(), kStripDip);
        LT_CHECK_EQ(w.max.r, w.close.l);
        LT_CHECK_EQ(w.min.r, w.max.l);
        LT_CHECK_EQ(w.pin.r, w.min.l);
        LT_CHECK_EQ(w.mainMenu.l, 0.f);
        LT_CHECK_EQ(w.mainMenu.w(), 46.f);
        LT_CHECK_EQ(w.mainMenu.h(), kStripDip);
        LT_CHECK_EQ(w.name.l, w.mainMenu.r + 8.f);
        LT_CHECK(w.name.l >= w.mainMenu.r && w.name.r <= w.pin.l && !w.name.Empty());
        LT_CHECK(w.Classify({W - 1, 10}) == TitleZone::Close);
        LT_CHECK(w.Classify({W - 60, 10}) == TitleZone::Max);
        LT_CHECK(w.Classify({W - 110, 10}) == TitleZone::Min);
        LT_CHECK(w.Classify({W - 150, 10}) == TitleZone::Pin);
        LT_CHECK(w.Classify({20, 22}) == TitleZone::MainMenu);
        LT_CHECK(w.Classify({60, 22}) == TitleZone::Drag);
        LT_CHECK(w.Classify({300, 22}) == TitleZone::Drag);
        LT_CHECK(w.Classify({300, 0}) == TitleZone::Drag);
        LT_CHECK(w.Classify({300, kStripDip}) == TitleZone::None);
        LT_CHECK(w.Classify({-1, 10}) == TitleZone::None);
        LT_CHECK(w.Classify({W, 10}) == TitleZone::None);
        // the HTMAXBUTTON rect in device pixels at this DPI: a whole 46 x 44 DIP box next to the close button
        const RectPx m = ToPx(w.max, s), c = ToPx(w.close, s);
        LT_CHECK_EQ(m.t, 0);
        LT_CHECK_EQ(m.b, (int)std::lround(kStripDip * s));
        LT_CHECK_EQ(m.r, c.l);
        LT_CHECK_EQ(m.w(), (int)std::lround(46 * s));
        // full screen: no Pin; the Max slot holds "exit full screen"
        in.fullscreen = true;
        TitleLayout f = TitleLayout::Compute(in);
        LT_CHECK(f.pin.Empty());
        LT_CHECK_EQ(f.min.r, f.max.l);
        LT_CHECK(f.name.r <= f.min.l);
        // a narrow window never overlaps the elements
        in.fullscreen = false;
        in.widthDip = 500;
        TitleLayout n = TitleLayout::Compute(in);
        LT_CHECK(n.mainMenu.r <= n.pin.l && n.pin.l >= 0);
    }
    pui::UiHost host;
    const float W = 1280;
    host.SetSize({W, 720});
    auto* root = host.SetRoot(std::make_unique<pui::Widget>(pui::Role::Window));
    auto* tb = root->Add<TitleBar>();
    tb->SetBounds({0, 0, W, kStripDip});
    LT_CHECK(host.HitTest({W - 20, 10}) == tb->CloseButton());
    LT_CHECK(host.HitTest({W - 70, 10}) == tb->MaximizeButton());
    LT_CHECK(host.HitTest({W - 120, 10}) == tb->MinimizeButton());
    LT_CHECK(host.HitTest({W - 170, 10}) == tb->PinButton());
    LT_CHECK(host.HitTest({20, 22}) == tb->MainMenuButton());
    LT_CHECK(tb->HitTest({600, 10}) == nullptr);                 // the drag area is HTCAPTION, not a UI target
    // maximize / restore glyph, pin state per always-on-top mode
    namespace G = pui::glyph;
    LT_CHECK(tb->MaximizeButton()->Glyph() == G::Str(G::ChromeMaximize));
    tb->SetMaximized(true);
    LT_CHECK(tb->MaximizeButton()->Glyph() == G::Str(G::ChromeRestore));
    tb->SetMaximized(false);
    LT_CHECK(tb->MaximizeButton()->Glyph() == G::Str(G::ChromeMaximize));
    LT_CHECK(tb->PinButton()->Glyph() == G::Str(G::Pin));
    std::vector<std::wstring> tips;
    for (int i = 0; i < sl::kAlwaysOnTopCount; i++) {
        tb->SetTopmostMode(sl::AlwaysOnTopFromIndex(i));
        LT_CHECK(tb->PinButton()->Glyph() == G::Str(i == 0 ? G::Pin : G::Pinned));
        LT_CHECK_EQ(tb->PinButton()->ToolTip(), TitleBar::PinToolTip(sl::AlwaysOnTopFromIndex(i)));
        for (const auto& t : tips) LT_CHECK(t != tb->PinButton()->ToolTip());
        tips.push_back(tb->PinButton()->ToolTip());
    }
    tb->SetFullscreenMode(true);
    LT_CHECK(!tb->PinButton()->Visible() && !tb->MaximizeButton()->Visible() && tb->ExitFullscreenButton()->Visible());
    LT_CHECK(host.HitTest({W - 70, 10}) == tb->ExitFullscreenButton());
}

LOGIC_TEST(drop_route_limits_targets_to_the_video_view) {
    LT_CHECK(RouteDrop(true, false, false, false) == DropRoute::VideoView);
    LT_CHECK(RouteDrop(false, false, true, false) == DropRoute::VideoView);
    LT_CHECK(RouteDrop(false, false, false, true) == DropRoute::VideoView);
    LT_CHECK(RouteDrop(false, false, false, false) == DropRoute::Rejected);
    LT_CHECK(RouteDrop(true, true, false, false) == DropRoute::Rejected);
    ShellLayoutIn fullIn; fullIn.cwPx = 1920; fullIn.chPx = 1080; fullIn.mode = ShellMode::FullScreen;
    ShellLayout full = ComputeShellLayout(fullIn);
    const int stripPointY = (int)(kStripDip / 2);
    bool pointInFullscreenVideo = stripPointY >= full.video.t && stripPointY < full.video.b;
    LT_CHECK(pointInFullscreenVideo && RouteDrop(pointInFullscreenVideo, false, false, true) == DropRoute::VideoView);
}

LOGIC_TEST(playback_and_queue_completion_keep_video_and_restore_controls) {
    LT_CHECK(RestoreVideoControlsOnCompletion(true));
    LT_CHECK(!RestoreVideoControlsOnCompletion(false));
    AutoHide chrome;
    chrome.Touch(1000);
    LT_CHECK(chrome.Hide(5000) == AutoHide::Action::StartHide);
    // The completion path calls Activity and never leaves the player view: same video mode, controls restored.
    if (RestoreVideoControlsOnCompletion(true)) chrome.Activity(6000);
    LT_CHECK(!chrome.Hidden() && chrome.Hittable(6300));
}

LOGIC_TEST(mode_transitions) {
    ModeState s;
    LT_CHECK(s.Mode() == ShellMode::NowPlaying);
    s = WithFullscreen(s, true);
    LT_CHECK(s.Mode() == ShellMode::FullScreen);
    s = WithFullscreen(s, false);
    LT_CHECK(s.Mode() == ShellMode::NowPlaying);
    s = WithFullscreen(s, true);
    s = WithMini(s, true);
    LT_CHECK(s.Mode() == ShellMode::MiniPlayer && !s.fullscreen);
    s = WithMini(s, false);
    LT_CHECK(s.Mode() == ShellMode::NowPlaying);
    s = WithMini(s, true);
    s = WithFullscreen(s, true);
    LT_CHECK(s.Mode() == ShellMode::FullScreen && !s.mini);
}

LOGIC_TEST(keyboard_table_mapping) {
    using C = CommandId;
    LT_CHECK(LookupKey('1', 0) == C::WindowSize50);
    LT_CHECK(LookupKey('2', 0) == C::WindowSize100);
    LT_CHECK(LookupKey('3', 0) == C::WindowSize150);
    LT_CHECK(LookupKey('4', 0) == C::WindowSize200);
    LT_CHECK(LookupKey('5', 0) == C::WindowSizeMax);
    LT_CHECK(LookupKey(0x0D, 0) == C::ToggleFullscreen);
    LT_CHECK(LookupKey(0x20, 0) == C::PlayPause);
    LT_CHECK(ShortcutText(C::PlayPause) == L"Space");
    LT_CHECK(LookupKey(0x20, kModCtrl) == C::None);
    LT_CHECK(ShortcutText(C::ToggleFullscreen) == L"Enter");
    LT_CHECK(MiniRemapCommand(LookupKey(0x0D, 0), true) == C::ToggleFullscreen);
    LT_CHECK(LookupKey(0x1B, 0) == C::Escape);
    LT_CHECK(LookupKey('X', 0) == C::SpeedDown);
    LT_CHECK(LookupKey(0x22, 0) == C::SpeedDown);
    LT_CHECK(LookupKey('C', 0) == C::SpeedUp);
    LT_CHECK(LookupKey(0x21, 0) == C::SpeedUp);
    LT_CHECK(LookupKey('Z', 0) == C::SpeedReset);
    LT_CHECK(LookupKey(0x25, 0) == C::SeekBack5);
    LT_CHECK(LookupKey(0x27, 0) == C::SeekForward5);
    LT_CHECK(LookupKey(0x25, kModCtrl | kModShift) == C::KeyframePrevious);
    LT_CHECK(LookupKey(0x27, kModCtrl | kModShift) == C::KeyframeNext);
    LT_CHECK(LookupKey(0x25, kModAlt) == C::None);                // Alt+arrows are unbound
    LT_CHECK(LookupKey(0x76, 0) == C::ToggleControlPanel);        // F7
    LT_CHECK(LookupKey(0x75, 0) == C::TogglePlaylistPanel);       // F6
    LT_CHECK(LookupKey(0x73, 0) == C::Stop);                      // F4
    LT_CHECK(LookupKey(0x71, 0) == C::OpenFolder);                // F2
    LT_CHECK(LookupKey('T', kModCtrl) == C::CycleAlwaysOnTop);
    LT_CHECK(LookupKey(0x70, kModCtrl) == C::ShowProperties);     // Ctrl+F1
    LT_CHECK(LookupKey('I', kModCtrl) == C::None);
    LT_CHECK(LookupKey('E', kModShift) == C::ShowEqualizer);
    LT_CHECK(LookupKey('E', kModCtrl | kModShift) == C::None);
    LT_CHECK(LookupKey(0x25, kModCtrl) == C::SeekBack30);
    LT_CHECK(LookupKey(0x27, kModCtrl) == C::SkipForward30);
    LT_CHECK(LookupKey(0x25, kModShift) == C::SeekBack60);
    LT_CHECK(LookupKey(0x27, kModShift) == C::SeekForward60);
    LT_CHECK(LookupKey(0x25, kModCtrl | kModAlt) == C::SeekBack300);
    LT_CHECK(LookupKey(0x27, kModCtrl | kModAlt) == C::SeekForward300);
    LT_CHECK(LookupKey('F', 0) == C::FrameNext);
    LT_CHECK(LookupKey('D', 0) == C::FramePrevious);
    LT_CHECK(LookupKey('M', 0) == C::ToggleMute);
    LT_CHECK(LookupKey(0x26, 0) == C::VolumeUp);
    LT_CHECK(LookupKey(0x28, 0) == C::VolumeDown);
    LT_CHECK(LookupKey(0x09, 0) == C::ToggleDebugOverlay);
    LT_CHECK(LookupKey('C', kModCtrl) == C::CaptureFrameWithCaptions);
    LT_CHECK(LookupKey('C', kModCtrl | kModShift) == C::CaptureFrame);
    LT_CHECK(LookupKey('O', kModCtrl) == C::OpenFiles);
    LT_CHECK(LookupKey(0x72, 0) == C::OpenFiles);                       // F3
    LT_CHECK(LookupKey(0x74, 0) == C::OpenSettings);                      // F5
    LT_CHECK(LookupKey(0x75, 0) == C::TogglePlaylistPanel);             // F6
    LT_CHECK(LookupKey('U', kModCtrl) == C::OpenUrl);
    LT_CHECK(LookupKey(0x70, kModCtrl) == C::ShowProperties);           // Ctrl+F1
    LT_CHECK(LookupKey('E', kModShift) == C::ShowEqualizer);
    LT_CHECK(LookupKey('V', kModCtrl | kModShift) == C::VideoEnhancements);
    const std::pair<int, unsigned> removed[] = {
        {'P', kModCtrl}, {'B', kModCtrl}, {'F', kModCtrl}, {'H', kModCtrl}, {'I', kModCtrl},
        {'G', kModCtrl | kModShift}, {'H', kModCtrl | kModShift}, {'J', kModCtrl | kModShift}, {'K', kModCtrl | kModShift},
        {'L', kModCtrl | kModShift}, {'L', kModCtrl}, {'S', kModCtrl | kModShift}, {'A', kModCtrl | kModShift},
        {'0', kModCtrl}, {0x60, kModCtrl}, {0x77, 0}, {0x78, 0}, {0x6B, kModCtrl}, {0xBB, kModCtrl},
        {0x6D, kModCtrl}, {0xBD, kModCtrl}, {0x7A, 0}, {0x0D, kModAlt}, {'M', kModCtrl}, {'N', kModCtrl},
        {'R', kModCtrl | kModShift}, {'Z', kModCtrl | kModShift},
        {0x25, kModAlt | kModShift}, {0x27, kModAlt | kModShift},
        {0xA6, 0}, {0xAC, 0}, {'F', kModCtrl | kModShift}, {'Y', kModCtrl}, {'Q', kModCtrl}, {'G', kModCtrl},
        {'O', kModCtrl | kModShift}, {0x72, kModCtrl}, {0x74, kModCtrl}, {0x75, kModCtrl}, {'E', kModCtrl | kModShift}};
    for (auto [vk, mods] : removed) LT_CHECK(LookupKey(vk, mods) == C::None);
    for (C gone : {C::Previous, C::Next, C::CycleSubtitles, C::ToggleMiniPlayer, C::ZoomToFill})
        LT_CHECK(ShortcutText(gone).empty());
    LT_CHECK(LookupKey('C', kModCtrl | kModAlt) == C::None);
    LT_CHECK(LookupKey('X', kModCtrl) == C::None);
    LT_CHECK(LookupKey('Q', 0) == C::None);
    std::set<std::pair<int, unsigned>> seen;
    for (const KeyBinding& b : KeyTable()) {
        LT_CHECK(b.command != C::None);
        LT_CHECK(seen.insert({b.vk, b.mods}).second);
    }
    {   // exactly the navigation keys
        const std::pair<int, unsigned> nav[] = {{0x72, 0}, {'O', kModCtrl}, {'U', kModCtrl}, {0x74, 0}, {0x75, 0}};
        const C navCmd[] = {C::OpenFiles, C::OpenFiles, C::OpenUrl, C::OpenSettings, C::TogglePlaylistPanel};
        for (size_t i = 0; i < 5; i++) LT_CHECK(LookupKey(nav[i].first, nav[i].second) == navCmd[i]);
    }
    LT_CHECK(ShortcutText(C::OpenFiles) == L"Ctrl+O");
    LT_CHECK(ShortcutText(C::OpenUrl) == L"Ctrl+U");
    LT_CHECK(ShortcutText(C::OpenSettings) == L"F5");
    LT_CHECK(ShortcutText(C::TogglePlaylistPanel) == L"F6" && ShortcutText(C::ToggleControlPanel) == L"F7");
        LT_CHECK(ShortcutText(C::KeyframePrevious) == L"Ctrl+Shift+Left");
    LT_CHECK(ShortcutText(C::KeyframeNext) == L"Ctrl+Shift+Right");
    LT_CHECK(ShortcutText(C::SeekBack5) == L"Left");
    LT_CHECK(ShortcutText(C::SeekForward5) == L"Right");
    LT_CHECK(ShortcutText(C::CycleAlwaysOnTop) == L"Ctrl+T" && ShortcutText(C::Stop) == L"F4" && ShortcutText(C::OpenFolder) == L"F2");
    LT_CHECK(ShortcutText(C::ShowProperties) == L"Ctrl+F1" && ShortcutText(C::ShowEqualizer) == L"Shift+E");
    LT_CHECK(ShortcutText(C::SeekBack30) == L"Ctrl+Left");
    LT_CHECK(ShortcutText(C::SeekForward60) == L"Shift+Right");
    LT_CHECK(ShortcutText(C::SeekBack300) == L"Ctrl+Alt+Left");
    LT_CHECK(ShortcutText(C::SkipForward30) == L"Ctrl+Right");
    LT_CHECK(ShortcutText(C::SpeedUp) == L"C");
    LT_CHECK(ShortcutText(C::ToggleMute) == L"M");
    LT_CHECK(ShortcutText(C::VolumeUp) == L"Up");
    LT_CHECK(ShortcutText(C::Escape) == L"Esc");
    LT_CHECK(ShortcutText(C::CaptureFrame) == L"Ctrl+Shift+C");
    LT_CHECK(ShortcutText(C::CaptureFrameWithCaptions) == L"Ctrl+C");
    LT_CHECK(ShortcutText(C::ToggleDebugOverlay) == L"Tab");
}

LOGIC_TEST(keyboard_seek_amounts) {
    using C = CommandId;
    LT_CHECK_EQ(SeekSeconds(LookupKey(0x25, kModCtrl | kModShift)), 0);   // Ctrl+Shift+Left / Right step to the adjacent keyframe, no timed seek
    LT_CHECK_EQ(SeekSeconds(LookupKey(0x27, kModCtrl | kModShift)), 0);
    LT_CHECK_EQ(SeekSeconds(LookupKey(0x25, 0)), -5);
    LT_CHECK_EQ(SeekSeconds(LookupKey(0x27, 0)), 5);
    LT_CHECK_EQ(SeekSeconds(C::SeekBack5), -5);         // the bar's Prev button (left click)
    LT_CHECK_EQ(SeekSeconds(LookupKey(0x25, kModCtrl)), -30);
    LT_CHECK_EQ(SeekSeconds(LookupKey(0x27, kModCtrl)), 30);
    LT_CHECK_EQ(SeekSeconds(LookupKey(0x25, kModShift)), -60);
    LT_CHECK_EQ(SeekSeconds(LookupKey(0x27, kModShift)), 60);
    LT_CHECK_EQ(SeekSeconds(LookupKey(0x25, kModCtrl | kModAlt)), -300);
    LT_CHECK_EQ(SeekSeconds(LookupKey(0x27, kModCtrl | kModAlt)), 300);
    LT_CHECK_EQ(SeekSeconds(C::SpeedUp), 0);
}

LOGIC_TEST(keyboard_speed_steps_round_and_clamp) {
    const double maxS = wvp::filters::kMaxSpeed;
    LT_CHECK_EQ(maxS, 100.0);                                          // atempo's tempo range (af_atempo.c)
    LT_CHECK_EQ(wvp::filters::kMinSpeed, 0.1);
    LT_CHECK_EQ(SteppedSpeed(1.0, +1, maxS), 1.1);
    LT_CHECK_EQ(SteppedSpeed(1.0, -1, maxS), 0.9);
    double s = 1.0;
    for (int i = 0; i < 30; i++) s = SteppedSpeed(s, +1, maxS);         // no float drift after many steps
    LT_CHECK_EQ(s, 4.0);
    for (int i = 0; i < 30; i++) s = SteppedSpeed(s, -1, maxS);
    LT_CHECK_EQ(s, 1.0);
    LT_CHECK_EQ(SteppedSpeed(0.1, -1, maxS), 0.1);
    LT_CHECK_EQ(SteppedSpeed(0.2, -1, maxS), 0.1);
    LT_CHECK_EQ(SteppedSpeed(100.0, +1, maxS), 100.0);
    LT_CHECK_EQ(SteppedSpeed(99.95, +1, maxS), 100.0);
    LT_CHECK_EQ(SteppedSpeed(0.25, +1, maxS), 0.4);
    LT_CHECK_EQ(SteppedSpeed(1.75, -1, maxS), 1.7);
    LT_CHECK(LookupKey('Z', 0) == CommandId::SpeedReset);
}

LOGIC_TEST(window_size_from_video) {
    // SAR source: 1440x1080 SAR 4:3 -> 1920x1080
    auto sar = VideoDisplaySize(1440, 1080, 4, 3);
    LT_CHECK(sar && sar->w == 1920 && sar->h == 1080);
    { auto q0 = VideoDisplaySize(1920, 1080, 0, 0); LT_CHECK(q0 && q0->w == 1920 && q0->h == 1080); }
    LT_CHECK(!VideoDisplaySize(0, 0, 0, 0));
    static_assert(kFallbackVideoW == 1024 && kFallbackVideoH == 768);   // nominal size when no video is known
    { auto fb = VideoDisplaySize(kFallbackVideoW, kFallbackVideoH, 0, 0); LT_CHECK(fb && fb->w == 1024 && fb->h == 768); }
    LT_CHECK_EQ(WindowScale(LookupKey('1', 0)), 0.5);
    LT_CHECK_EQ(WindowScale(LookupKey('2', 0)), 1.0);
    LT_CHECK_EQ(WindowScale(LookupKey('3', 0)), 1.5);
    LT_CHECK_EQ(WindowScale(LookupKey('4', 0)), 2.0);
    LT_CHECK(WindowScale(LookupKey('5', 0)) < 0);

    // 96 DPI window: 8 px frame left/right/bottom (7 px of it invisible), 44 px title strip, no top frame
    VideoWindowIn in;
    in.video = *sar;
    in.extraW = 16; in.extraH = 32 + 8;
    in.invL = 7; in.invR = 7; in.invB = 7;
    in.minW = kMinWindowWidthDip; in.minH = kMinWindowHeightDip;
    in.work = {0, 0, 2560, 1380};                  // 2560x1440 monitor, 60 px taskbar
    in.window = {100, 100, 900, 700};
    auto visW = [&](const PxRect& r) { return r.w() - in.invL - in.invR; };
    auto visH = [&](const PxRect& r) { return r.h() - in.invT - in.invB; };
    auto inside = [&](const PxRect& r, const PxRect& w) {
        return r.l + in.invL >= w.l && r.t + in.invT >= w.t && r.r - in.invR <= w.r && r.b - in.invB <= w.b;
    };
    in.scale = 1.0;
    PxRect r = VideoWindowRect(in);
    LT_CHECK_EQ(r.w() - in.extraW, 1920);          // video rect 1920x1080 at 1x (fits)
    LT_CHECK_EQ(r.h() - in.extraH, 1080);
    LT_CHECK(inside(r, in.work));
    in.scale = 0.5;                                // key 1: 960x540 video, above the minimum
    r = VideoWindowRect(in);
    LT_CHECK_EQ(r.w() - in.extraW, 960);
    LT_CHECK_EQ(r.h() - in.extraH, 540);
    LT_CHECK_EQ((r.l + r.r) / 2, 500);
    LT_CHECK_EQ((r.t + r.b) / 2, 400);
    // one axis overflows: 1.5x = 2880x1620 video on a 3000x1400 work area -> full target width, height clamped only
    VideoWindowIn one = in;
    one.work = {0, 0, 3000, 1400};
    one.scale = 1.5;
    r = VideoWindowRect(one);
    LT_CHECK_EQ(r.w(), 2880 + one.extraW);         // width untouched (no aspect shrink)
    LT_CHECK_EQ(r.h() - one.invT - one.invB, 1400);
    // both overflow: 2x of 1920x1080 on 2560x1380 -> visible window 2560 x 1380 (each axis clamped on its own)
    in.scale = 2.0;
    r = VideoWindowRect(in);
    LT_CHECK_EQ(visW(r), 2560);
    LT_CHECK_EQ(visH(r), 1380);
    LT_CHECK(inside(r, in.work));
    in.scale = -1;                                 // key 5 "Max": the largest normal window on both axes
    LT_CHECK(VideoWindowRect(in) == r);
    // small video: Max still fills the work area (video letterboxes inside)
    VideoWindowIn sm = in;
    sm.video = {640, 480};
    LT_CHECK(VideoWindowRect(sm) == r);
    // minimum size: 0.5x of a 640x360 video = 320x180 -> the window stays at the minimum
    VideoWindowIn mn = in;
    mn.video = {640, 360};
    mn.scale = 0.5;
    r = VideoWindowRect(mn);
    LT_CHECK_EQ(r.w(), kMinWindowWidthDip);
    LT_CHECK_EQ(r.h(), kMinWindowHeightDip);
    // near the minimum: 0.5x of 1024x576 = 512x288 -> width target 528 (> 500), height raised to 374
    mn.video = {1024, 576};
    r = VideoWindowRect(mn);
    LT_CHECK_EQ(r.w(), 512 + mn.extraW);
    LT_CHECK_EQ(r.h(), kMinWindowHeightDip);
    // key 1 at 120 / 144 DPI on 1920x1080 (SAR 4:3 source): the 0.5x video rect is not blocked by the minimum
    for (int dpi : {96, 120, 144}) {
        double s = dpi / 96.0;
        int frame = (int)std::lround(8 * s), strip = (int)std::lround(kStripDip * s);
        VideoWindowIn k = in;
        k.extraW = 2 * frame; k.extraH = strip + frame;
        k.invL = k.invR = k.invB = frame - 1;
        k.minW = (int)std::lround(kMinWindowWidthDip * s); k.minH = (int)std::lround(kMinWindowHeightDip * s);
        k.work = {0, 0, 3840, 2100};
        k.scale = 0.5;
        PxRect q = VideoWindowRect(k);
        LT_CHECK_EQ(q.w() - k.extraW, 960);
        LT_CHECK_EQ(q.h() - k.extraH, 540);
        LT_CHECK(q.w() >= k.minW && q.h() >= k.minH);
    }
    // multi-monitor: a secondary monitor left of the primary (negative origin) and a top taskbar
    VideoWindowIn mm = in;
    mm.work = {-1920, 40, 0, 1080};
    mm.window = {-1900, 60, -1000, 600};
    mm.scale = 0.5;
    PxRect q = VideoWindowRect(mm);
    LT_CHECK_EQ(q.w() - mm.extraW, 960);
    LT_CHECK(q.l + mm.invL >= -1920 && q.r - mm.invR <= 0 && q.t + mm.invT >= 40 && q.b - mm.invB <= 1080);
    mm.scale = 1.0;                                // 1920x1080 + frame overflows a 1920x1040 work area: both clamp
    q = VideoWindowRect(mm);
    LT_CHECK_EQ(q.w() - mm.invL - mm.invR, 1920);
    LT_CHECK_EQ(q.h() - mm.invT - mm.invB, 1040);
    LT_CHECK(q.t + mm.invT >= 40 && q.b - mm.invB <= 1080);
    VideoWindowIn off = in;
    off.window = {2400, 1200, 2900, 1600};
    off.scale = 0.5;
    PxRect o = VideoWindowRect(off);
    LT_CHECK(o.r - off.invR <= 2560 && o.b - off.invB <= 1380);
}

LOGIC_TEST(window_snap_edges) {
    const std::vector<PxRect> areas = {{0, 0, 1920, 1040}};
    const std::vector<PxRect> wins = {{1000, 100, 1600, 500}};
    // one-shot snap of `vis` (a fresh loop whose first proposal is the raw rect)
    auto snap1 = [&](PxRect vis, bool move, SnapEdges drag, const std::vector<PxRect>& w, int minW = 0, int minH = 0) {
        SnapTracker t;
        t.Begin(vis, 0, 0);
        return t.Step(0, 0, vis, move, drag, areas, w, 12, minW, minH);
    };
    // move: left edge 8 px from the work-area left -> snaps; top 30 px away -> stays
    PxRect r = snap1({8, 30, 408, 330}, true, {}, {});
    LT_CHECK(r == (PxRect{0, 30, 400, 330}));
    // right edge near the work area's right edge snaps the whole rect
    r = snap1({1500, 200, 1914, 500}, true, {}, {});
    LT_CHECK_EQ(r.r, 1920);
    LT_CHECK_EQ(r.w(), 414);
    // adjacent to another player (our right edge near its left edge) and aligned top
    r = snap1({600, 94, 995, 400}, true, {}, wins);
    LT_CHECK_EQ(r.r, 1000);
    LT_CHECK_EQ(r.t, 100);
    // a window far away vertically is ignored
    r = snap1({600, 700, 995, 900}, true, {}, wins);
    LT_CHECK_EQ(r.r, 995);
    // resize: only the dragged edge snaps
    SnapEdges re; re.r = true;
    r = snap1({4, 4, 1912, 400}, false, re, {});
    LT_CHECK_EQ(r.r, 1920);
    LT_CHECK_EQ(r.l, 4);
    LT_CHECK_EQ(r.t, 4);
    // resize never snaps below the minimum size
    SnapEdges le; le.l = true;
    {
        SnapTracker t;
        t.Begin({1410, 0, 1920, 300}, 0, 0);
        r = t.Step(0, 0, {1410, 0, 1920, 300}, false, le, {{1420, 0, 2400, 1040}}, {}, 12, 505, 0);
        LT_CHECK_EQ(r.l, 1410);
    }
    // hysteresis, moving left (window 400 wide at x=100, cursor drives it): snaps at x<=12, holds for 12 px of travel, then frees
    {
        SnapTracker t;
        t.Begin({100, 200, 500, 500}, 1000, 1000);
        auto x = [&](int cx) { return t.Step(cx, 1000, {100 + (cx - 1000), 200, 500 + (cx - 1000), 500}, true, {}, areas, {}, 12).l; };
        LT_CHECK_EQ(x(920), 20);   // raw 20: out of reach
        LT_CHECK_EQ(x(910), 0);    // raw 10: snaps
        LT_CHECK_EQ(x(900), 0);    // raw 0: held
        LT_CHECK_EQ(x(898), 0);    // raw -2: 12 travelled from the engagement, still held
        LT_CHECK_EQ(x(897), -3);   // 13 travelled: free, and not re-grabbed
        LT_CHECK_EQ(x(890), -10);
        LT_CHECK_EQ(x(885), -15);  // left the target's reach
        LT_CHECK_EQ(x(895), -5);   // now moving right: the left edge is no longer leading, no grab
    }
    // hysteresis, moving right onto the work area's right edge (right edge snap): free after exactly thr travel
    {
        SnapTracker t;
        t.Begin({1500, 200, 1914, 500}, 0, 0);
        const std::vector<PxRect> a2 = {{0, 0, 1920, 1040}};
        auto step = [&](int dx) { return t.Step(dx, 0, {1500 + dx, 200, 1914 + dx, 500}, true, {}, a2, {}, 12).r; };
        LT_CHECK_EQ(step(0), 1920);        // raw right 1914: snaps (origin 1506, engaged at raw 1500)
        LT_CHECK_EQ(step(12), 1920);       // raw 1926: 12 travelled, held
        LT_CHECK_EQ(step(13), 1927);       // 13 travelled: free, and not re-grabbed
        LT_CHECK_EQ(step(10), 1924);       // the released target stays quiet while raw is within its reach
    }
    // leading edge: a window 10 px narrower than the work area, moved right then left, snaps right-edge then left-edge
    {
        SnapTracker t;
        t.Begin({-30, 200, 1880, 500}, 0, 0);
        auto x = [&](int p) { return t.Step(p + 30, 0, {p, 200, p + 1910, 500}, true, {}, areas, {}, 12).l; };
        LT_CHECK_EQ(x(-30), -30);
        LT_CHECK_EQ(x(-5), -5);
        LT_CHECK_EQ(x(0), 10);     // moving right: the right edge (origin 10) grabs, not the left one at 0
        LT_CHECK_EQ(x(12), 10);
        LT_CHECK_EQ(x(13), 13);    // released after 12 px of travel
        LT_CHECK_EQ(x(30), 30);
        LT_CHECK_EQ(x(20), 20);
        LT_CHECK_EQ(x(10), 0);     // moving left: the left edge (origin 0) grabs
        LT_CHECK_EQ(x(-2), 0);
        LT_CHECK_EQ(x(-3), -3);
    }
    // resize edge: right edge snaps at 1912, releases after 12 px travel
    {
        SnapTracker t;
        t.Begin({4, 4, 1912, 400}, 0, 0);
        auto rr = [&](int dx) { return t.Step(dx, 0, {4, 4, 1912 + dx, 400}, false, re, areas, {}, 12).r; };
        LT_CHECK_EQ(rr(0), 1920);
        LT_CHECK_EQ(rr(-12), 1920);
        LT_CHECK_EQ(rr(-13), 1899);
        LT_CHECK_EQ(rr(8), 1920);     // walking back into reach snaps again
    }
}

LOGIC_TEST(client_size_for_video_matches_the_shell_layout) {
    for (float sc : {1.f, 1.25f, 1.5f, 2.f}) {
        for (float panelDip : {240.f, 320.f, 450.f}) {
            for (bool panel : {false, true}) {
                for (int vw : {200, 239, 240, 320, 512, 559, 560, 640, 960, 1280, 1920}) {
                    const int cw = ClientWidthForVideo(vw, sc, panel, panelDip), ch = ClientHeightForVideo(540, sc);
                    ShellLayoutIn in;
                    in.cwPx = cw; in.chPx = ch; in.scale = sc; in.mode = ShellMode::NowPlaying; in.panelWanted = panel; in.panelWidthDip = panelDip;
                    const ShellLayout l = ComputeShellLayout(in);
                    LT_CHECK_EQ(l.video.w(), vw);
                    LT_CHECK_EQ(l.video.h(), 540);
                    LT_CHECK_EQ(l.panelPx.Empty(), !(panel && vw >= (int)std::lround(kVideoMinDockDip * sc)));
                    if (!l.panelPx.Empty()) LT_CHECK_EQ(l.panelPx.w(), (int)std::lround(panelDip * sc));
                }
            }
        }
    }
}

LOGIC_TEST(always_on_top_guard_keeps_the_topmost_band) {
    const unsigned SWP_NOZORDER_ = 0x0004, SWP_NOMOVE_ = 0x0002;
    LT_CHECK(ForceTopmostInsertAfter(true, SWP_NOMOVE_, false, false));
    LT_CHECK(!ForceTopmostInsertAfter(true, SWP_NOMOVE_, false, true));
    LT_CHECK(!ForceTopmostInsertAfter(true, SWP_NOMOVE_, true, false));
    LT_CHECK(!ForceTopmostInsertAfter(true, SWP_NOMOVE_ | SWP_NOZORDER_, false, false));
    LT_CHECK(!ForceTopmostInsertAfter(false, SWP_NOMOVE_, false, false));
}

LOGIC_TEST(video_click_rules) {
    LT_CHECK(VideoClickCommand(1) == CommandId::None);
    LT_CHECK(VideoClickCommand(2) == CommandId::PlayPause);
    LT_CHECK(VideoClickCommand(3) == CommandId::None);
    // Space: play/pause on key down in the video modes even with a focused (clicked) button; never over popups / text
    LT_CHECK(SpaceTogglesPlayback(false, false));
    LT_CHECK(!SpaceTogglesPlayback(true, false));
    LT_CHECK(!SpaceTogglesPlayback(false, true));
}

LOGIC_TEST(bar_button_enable_and_visibility_rules) {
    BarModel m;
    LT_CHECK(!ButtonInfo(m, B_Play).enabled && !ButtonInfo(m, B_Stop).enabled);
    LT_CHECK(!ButtonInfo(m, B_Prev).enabled && !ButtonInfo(m, B_Next).enabled);
    LT_CHECK(!ButtonInfo(m, B_Subs).enabled);
    LT_CHECK(ButtonInfo(m, B_Open).enabled && ButtonInfo(m, B_Mute).enabled && ButtonInfo(m, B_Control).enabled && ButtonInfo(m, B_Full).enabled);
    LT_CHECK(!ButtonInfo(m, B_Panel).enabled && !ButtonInfo(m, B_Panel).toggled);
    m.loaded = true; m.prepared = true; m.seekable = true; m.dur = 100; m.playing = true;
    LT_CHECK(ButtonInfo(m, B_Play).enabled && ButtonInfo(m, B_Stop).enabled && ButtonInfo(m, B_Subs).enabled);
    LT_CHECK(ButtonInfo(m, B_Play).glyph == L"\uE769");
    LT_CHECK(ButtonInfo(m, B_Prev).enabled && ButtonInfo(m, B_Next).enabled);   // left click seeks 5 s
    m.seekable = false;
    LT_CHECK(!ButtonInfo(m, B_Prev).enabled);
    m.hasPrev = true; m.hasNext = true;                                          // right click switches files
    LT_CHECK(ButtonInfo(m, B_Prev).enabled && ButtonInfo(m, B_Next).enabled);
    LT_CHECK(m.SliderEnabled() && !m.Seekable());
    m.playing = false;
    LT_CHECK(ButtonInfo(m, B_Play).glyph == L"\uE768");
    m.panelAvailable = true; m.panelOpen = true; m.controlPanelOpen = true;
    LT_CHECK(ButtonInfo(m, B_Panel).enabled && ButtonInfo(m, B_Panel).toggled && ButtonInfo(m, B_Control).toggled);
    m.panelAvailable = false;
    LT_CHECK(!ButtonInfo(m, B_Panel).enabled && !ButtonInfo(m, B_Panel).toggled);   // full screen: the playlist toggle is disabled
    m.fullscreen = true;
    LT_CHECK(ButtonInfo(m, B_Full).glyph == L"\uE73F");
    m.muted = true;
    LT_CHECK(ButtonInfo(m, B_Mute).glyph == L"\uE74F");
    m.dur = 0;
    LT_CHECK(m.Live());
}

LOGIC_TEST(auto_hide_state_machine) {
    AutoHide a;
    uint64_t t = 1000;
    LT_CHECK(a.Enabled());
    a.Touch(t);
    LT_CHECK(a.Tick(t + 2999, true, false, false) == AutoHide::Action::None);       // idle < 3 s
    LT_CHECK(a.Tick(t + 3000, true, true, false) == AutoHide::Action::None);
    LT_CHECK(a.Tick(t + 3000, false, false, false) == AutoHide::Action::None);
    LT_CHECK(a.Tick(t + 3000, true, false, true) == AutoHide::Action::None);
    LT_CHECK(a.Tick(t + 3000, true, false, false) == AutoHide::Action::StartHide);
    LT_CHECK_EQ(a.LastAnim().durMs, 500);
    LT_CHECK(!a.LastAnim().easeOut);
    LT_CHECK(a.Hidden());
    LT_CHECK_EQ(a.P(t + 3000), 0.f);
    LT_CHECK(a.P(t + 3250) > 0.f && a.P(t + 3250) < 0.5f);
    LT_CHECK_EQ(a.P(t + 3500), 1.f);
    LT_CHECK(a.Hittable(t + 3000) == false);
    LT_CHECK(a.Hide(t + 3600) == AutoHide::Action::None);
    LT_CHECK(a.Activity(t + 4000) == AutoHide::Action::StartShow);
    LT_CHECK_EQ(a.LastAnim().durMs, 300);
    LT_CHECK(a.LastAnim().easeOut);
    LT_CHECK_EQ(a.P(t + 4000), 1.f);
    LT_CHECK_EQ(a.P(t + 4300), 0.f);
    LT_CHECK(a.Hittable(t + 4300));
    a.Tick(t + 8000, true, false, false);
    LT_CHECK(a.Hidden());
    LT_CHECK(a.Tick(t + 9000, false, false, false) == AutoHide::Action::StartShow);
    AutoHide b;
    b.Touch(0);
    b.Hide(10000);
    float mid = b.P(10300);
    LT_CHECK(mid > 0.f && mid < 1.f);
    b.Show(10300);
    LT_CHECK(std::fabs(b.LastAnim().p0 - mid) < 1e-6f && b.LastAnim().p1 == 0.f);
    AutoHide d;
    d.SetEnabled(false, 0);
    LT_CHECK(d.Tick(100000, true, false, false) == AutoHide::Action::None);
    LT_CHECK(d.Hittable(0));
    LT_CHECK(d.P(0) == 0.f);
}

LOGIC_TEST(auto_hide_keyboard_never_reveals_the_osd) {
    AutoHide a;
    uint64_t t = 1000;
    a.Touch(t);
    LT_CHECK(a.Tick(t + 3000, true, false, false) == AutoHide::Action::StartHide);
    LT_CHECK(a.Hidden());
    LT_CHECK(a.KeyPress() == AutoHide::Action::None);
    LT_CHECK(a.Hidden());
    LT_CHECK(a.KeyQuiet());
    LT_CHECK(a.Reveal(t + 4000) == AutoHide::Action::None);
    LT_CHECK(a.Tick(t + 5000, false, false, false) == AutoHide::Action::None);
    LT_CHECK(a.Hidden());
    LT_CHECK(a.Activity(t + 6000) == AutoHide::Action::StartShow);
    LT_CHECK(!a.Hidden());
    LT_CHECK(!a.KeyQuiet());
    LT_CHECK(a.Reveal(t + 6100) == AutoHide::Action::None);
    LT_CHECK(a.Tick(t + 9100, true, false, false) == AutoHide::Action::StartHide);
    LT_CHECK(a.Hidden());
    LT_CHECK(a.Reveal(t + 9700) == AutoHide::Action::StartShow);
    AutoHide v;
    v.Touch(20000);
    v.KeyPress();
    LT_CHECK(v.LastActivity() == 20000);
    LT_CHECK(v.Tick(22900, true, false, false) == AutoHide::Action::None);
    v.KeyPress();
    LT_CHECK(v.Tick(23000, true, false, false) == AutoHide::Action::StartHide);    // hides 3 s after the last pointer activity
}

LOGIC_TEST(fullscreen_title_overlay_tracks_the_osd_hide_and_key_rules) {
    ShellLayoutIn in;
    in.cwPx = 1920; in.chPx = 1080; in.mode = ShellMode::FullScreen;
    const ShellLayout layout = ComputeShellLayout(in);
    LT_CHECK(layout.titleSlides && !layout.title.Empty());
    AutoHide osd;
    const uint64_t t = 1000;
    osd.Touch(t);
    LT_CHECK(osd.Tick(t + 3000, true, false, false) == AutoHide::Action::StartHide);
    LT_CHECK(osd.Hidden() && !osd.Hittable(t + 3000));
    const float hiddenP = osd.P(t + 3000);
    LT_CHECK_EQ(hiddenP, 0.f);
    LT_CHECK_EQ(osd.P(t + 3500), 1.f);
    LT_CHECK(osd.KeyPress() == AutoHide::Action::None);
    LT_CHECK(osd.Reveal(t + 4000) == AutoHide::Action::None);
    LT_CHECK(osd.Hidden() && !osd.Hittable(t + 4000));
    LT_CHECK(osd.Activity(t + 5000) == AutoHide::Action::StartShow);
    LT_CHECK_EQ(osd.P(t + 5000), 1.f);
    LT_CHECK_EQ(osd.P(t + 5300), 0.f);
    LT_CHECK(osd.Hittable(t + 5300));
    CursorAutoHide cursor;
    cursor.Moved(t);
    const bool overVisibleStrip = osd.Hittable(t + 5300) && layout.title.Contains(8, 8);
    LT_CHECK(overVisibleStrip && !cursor.ShouldHide(t + 5300, true, overVisibleStrip, false, false));
    LT_CHECK(osd.Tick(t + 8300, true, false, false) == AutoHide::Action::StartHide);
    const bool overHiddenStrip = osd.Hittable(t + 8300) && layout.title.Contains(8, 8);
    LT_CHECK(!overHiddenStrip && cursor.ShouldHide(t + 5300, true, overHiddenStrip, false, false));
}

LOGIC_TEST(auto_hide_pointer_leaving_the_window_hides_at_once) {
    AutoHide a;
    uint64_t t = 1000;
    a.Touch(t);
    LT_CHECK(!a.Hidden());
    LT_CHECK(a.PointerLeft(t + 100, false) == AutoHide::Action::StartHide);       // no 3 s wait
    LT_CHECK(a.Hidden() && a.PointerOut());
    LT_CHECK(a.Tick(t + 5000, false, false, false) == AutoHide::Action::None);
    LT_CHECK(a.Reveal(t + 5100) == AutoHide::Action::None);
    LT_CHECK(a.Activity(t + 6000) == AutoHide::Action::StartShow);
    LT_CHECK(!a.PointerOut());
    LT_CHECK(a.PointerLeft(t + 6100, true) == AutoHide::Action::None);
    LT_CHECK(!a.Hidden() && !a.PointerOut());
    AutoHide d;
    d.SetEnabled(false, 0);
    LT_CHECK(d.PointerLeft(100, false) == AutoHide::Action::None);
    LT_CHECK(!d.Hidden());
}

LOGIC_TEST(fullscreen_edge_zones_and_edge_autohide) {
    const float W = 1920, H = 1080;
    EdgeZones mid = EdgeZonesAt(900, 500, W, H);
    LT_CHECK(!mid.top && !mid.bottom && !mid.right);
    LT_CHECK(EdgeZonesAt(900, 10, W, H).top && !EdgeZonesAt(900, kStripDip, W, H).top);
    LT_CHECK(EdgeZonesAt(900, H - 5, W, H).bottom && !EdgeZonesAt(900, H - kDockDip - 1, W, H).bottom);
    LT_CHECK(EdgeZonesAt(W - 2, 500, W, H).right && !EdgeZonesAt(W - kEdgeRightDip - 1, 500, W, H).right);
    EdgeZones corner = EdgeZonesAt(W - 2, 5, W, H);
    LT_CHECK(corner.top && corner.right && !corner.bottom);
    AutoHide a;
    a.SetEdge(true);
    uint64_t t = 1000;
    a.Snap(1.f);
    LT_CHECK(a.Activity(t) == AutoHide::Action::None);                            // pointer anywhere else shows nothing
    LT_CHECK(a.Reveal(t) == AutoHide::Action::None);
    LT_CHECK(a.EdgeTick(t + 5000, false, false, false) == AutoHide::Action::None);   // already hidden
    LT_CHECK(a.Enter(t + 6000) == AutoHide::Action::Snapped && !a.Hidden());      // the edge zone shows it, no animation
    LT_CHECK(!a.Animating(t + 6000) && a.P(t + 6000) == 0.f);
    LT_CHECK(a.EdgeTick(t + 600000, true, false, false) == AutoHide::Action::None);   // pointer over it: stays indefinitely
    LT_CHECK(a.EdgeTick(t + 6001, false, true, false) == AutoHide::Action::None);     // menu / drag / search keeps it
    LT_CHECK(a.EdgeTick(t + 6001, false, false, false) == AutoHide::Action::Snapped); // pointer off, exception over: hides at once
    LT_CHECK(a.Hidden() && !a.Animating(t + 6001) && a.P(t + 6001) == 1.f);
    AutoHide w;   // windowed / mini keep the animated idle hide
    LT_CHECK(w.Hide(t) == AutoHide::Action::StartHide && w.Animating(t + 1));
}

LOGIC_TEST(cursor_autohide_is_independent_of_the_bar) {
    CursorAutoHide c;
    c.Moved(1000);
    LT_CHECK(!c.ShouldHide(1000 + kCursorIdleHideMs - 1, true, false, false, false));
    LT_CHECK(c.ShouldHide(1000 + kCursorIdleHideMs, true, false, false, false));
    LT_CHECK(!c.ShouldHide(9000, false, false, false, false));
    LT_CHECK(!c.ShouldHide(9000, true, true, false, false));
    LT_CHECK(!c.ShouldHide(9000, true, false, true, false));
    LT_CHECK(!c.ShouldHide(9000, true, false, false, true));
    c.Moved(9000);
    LT_CHECK(!c.ShouldHide(9000, true, false, false, false));
}

LOGIC_TEST(cursor_key_press_leaves_a_hidden_cursor_hidden) {
    CursorAutoHide c;
    c.Moved(1000);
    uint64_t t = 1000 + kCursorIdleHideMs;
    LT_CHECK(c.ShouldHide(t, true, false, false, false));
    c.KeyPress();
    LT_CHECK(c.ShouldHide(t, true, false, false, false));
    LT_CHECK(c.ShouldHide(t + 5000, true, false, false, false));
    LT_CHECK(!c.ShouldHide(t, true, false, false, true));
    LT_CHECK(!c.ShouldHide(t, false, false, false, false));
    c.Moved(t + 6000);
    LT_CHECK(!c.ShouldHide(t + 6000, true, false, false, false));
}

LOGIC_TEST(color_fade_83ms) {
    ColorFade f;
    f.Target(0x00FFFFFF, 100);
    LT_CHECK(f.Cur(100).v == 0x00FFFFFFu);
    f.Target(0x15FFFFFF, 1000);
    LT_CHECK(f.Active(1010));
    uint32_t mid = f.Cur(1041).v >> 24;
    LT_CHECK(mid > 0 && mid < 0x15);
    LT_CHECK(f.Cur(1083).v == 0x15FFFFFFu);
    LT_CHECK(!f.Active(1083));
}

namespace {
struct ShellFixture {
    NullShellContext ctx;   // outlives the host: the panel keeps a reference
    pui::UiHost host;
    ShellRoot* shell = nullptr;
    ShellLayout layout;
    explicit ShellFixture(ShellMode mode, float s = 1.f, int wdip = 1280, int hdip = 720) {
        host.SetSize({(float)wdip, (float)hdip});
        shell = static_cast<ShellRoot*>(host.SetRoot(std::make_unique<ShellRoot>(ctx)));
        shell->Title()->SetScale(s);
        shell->Bar()->SetScale(s);
        ShellLayoutIn in;
        in.cwPx = (int)std::lround(wdip * s); in.chPx = (int)std::lround(hdip * s); in.scale = s; in.mode = mode;
        layout = ComputeShellLayout(in);
        shell->ApplyLayout(layout, mode);
    }
};
}

LOGIC_TEST(title_bar_modes_in_the_shell) {
    ShellFixture f(ShellMode::NowPlaying);
    LT_CHECK(f.shell->Title()->Visible() && !f.shell->Title()->FullscreenMode());
    LT_CHECK(f.shell->Title()->MainMenuButton()->Visible() && f.shell->Title()->PinButton()->Visible());
    ShellFixture mini(ShellMode::MiniPlayer);
    LT_CHECK(!mini.shell->Title()->Visible());
    ShellFixture full(ShellMode::FullScreen);
    LT_CHECK(full.shell->Title()->Visible());
    LT_CHECK(full.shell->Title()->FullscreenMode());
    LT_CHECK(full.shell->Title()->MinimizeButton()->Visible() && full.shell->Title()->ExitFullscreenButton()->Visible() &&
             full.shell->Title()->CloseButton()->Visible() && full.shell->Title()->MainMenuButton()->Visible());
    LT_CHECK(!full.shell->Title()->PinButton()->Visible() && !full.shell->Title()->MaximizeButton()->Visible());
}

LOGIC_TEST(shell_focus_order_tab_cycle) {
    ShellFixture f(ShellMode::NowPlaying);
    BarModel m; m.loaded = true; m.prepared = true; m.seekable = true; m.dur = 60; m.playing = true; m.hasNext = true; m.subCount = 1;
    f.shell->Bar()->SetModel(m);
    auto order = f.host.FocusOrder();
    LT_CHECK(order.size() > 8);
    TitleBar* tb = f.shell->Title();
    LT_CHECK(order[0] == tb->MainMenuButton() && order[1] == tb->PinButton());
    LT_CHECK(order[2] == tb->MinimizeButton() && order[3] == tb->MaximizeButton() && order[4] == tb->CloseButton());
    for (size_t i = 5; i < order.size(); i++) LT_CHECK(f.shell->Bar()->IsAncestorOf(order[i]));
    for (size_t i = 0; i < order.size(); i++) {
        f.host.KeyDown(pui::key::Tab, 0);
        LT_CHECK(f.host.Focused() == order[i]);
    }
    f.host.KeyDown(pui::key::Tab, 0);
    LT_CHECK(f.host.Focused() == order[0]);
    f.host.KeyDown(pui::key::Tab, pui::ModShift);
    LT_CHECK(f.host.Focused() == order.back());
    ShellFixture fs(ShellMode::FullScreen);
    fs.shell->Bar()->SetModel(m);
    auto o3 = fs.host.FocusOrder();
    LT_CHECK(o3.size() > 4);
    LT_CHECK(o3[0] == fs.shell->Title()->MainMenuButton());
    LT_CHECK(o3[1] == fs.shell->Title()->MinimizeButton());
    LT_CHECK(o3[2] == fs.shell->Title()->ExitFullscreenButton());
    LT_CHECK(o3[3] == fs.shell->Title()->CloseButton());
    for (size_t i = 4; i < o3.size(); i++) LT_CHECK(fs.shell->Bar()->IsAncestorOf(o3[i]));
}

LOGIC_TEST(bar_widgets_invoke_commands_and_automation) {
    ShellFixture f(ShellMode::NowPlaying);
    std::vector<CommandId> got;
    int seeks = 0;
    double lastSeek = -1;
    PlayerBar::Actions a;
    a.command = [&](CommandId c) { got.push_back(c); };
    a.seekTo = [&](double v) { seeks++; lastSeek = v; };
    f.shell->Bar()->SetActions(a);
    BarModel m; m.loaded = true; m.prepared = true; m.seekable = true; m.dur = 100; m.pos = 20; m.hasNext = true; m.hasPrev = true;
    f.shell->Bar()->SetModel(m);
    auto click = [&](pui::Widget* w) {
        pui::PointerEvent e; e.pt = {w->Bounds().cx(), w->Bounds().cy()};
        f.host.PointerMove(e); f.host.PointerDown(e); f.host.PointerUp(e);
    };
    click(f.shell->Bar()->ButtonWidget(B_Play));
    click(f.shell->Bar()->ButtonWidget(B_Next));
    click(f.shell->Bar()->ButtonWidget(B_Full));
    LT_CHECK_EQ((int)got.size(), 3);
    LT_CHECK(got[0] == CommandId::PlayPause && got[1] == CommandId::SeekForward5 && got[2] == CommandId::ToggleFullscreen);   // Next: left click seeks
    LT_CHECK(f.shell->Bar()->ButtonWidget(B_Subs)->Visible());
    for (int b = 0; b < B_Count; b++) {
        pui::Widget* w = f.shell->Bar()->ButtonWidget(b);
        LT_CHECK(!w->Name().empty());
        LT_CHECK(!w->ToolTip().empty());
        LT_CHECK(w->SupportsInvoke());
        LT_CHECK(w->GetRole() == pui::Role::Button);
    }
    got.clear();
    f.shell->Bar()->ButtonWidget(B_Prev)->Invoke();
    LT_CHECK(got.size() == 1 && got[0] == CommandId::SeekBack5);
    {   // right click = previous / next file
        pui::Widget* prev = f.shell->Bar()->ButtonWidget(B_Prev);
        pui::PointerEvent e; e.pt = {prev->Bounds().cx(), prev->Bounds().cy()}; e.button = pui::PointerButton::Right;
        f.host.PointerMove(e); f.host.PointerDown(e); f.host.PointerUp(e);
        LT_CHECK(got.size() == 2 && got[1] == CommandId::Previous);
    }
    LT_CHECK(f.shell->Bar()->ButtonWidget(B_Play)->Name() == L"Play");
    auto r = f.shell->Bar()->Seek()->GetRange();
    LT_CHECK_EQ(r.maximum, 100.0);
    LT_CHECK_EQ(r.value, 20.0);
    f.shell->Bar()->Seek()->SetRangeValue(50);
    LT_CHECK_EQ(seeks, 1);
    LT_CHECK(std::fabs(lastSeek - 0.5) < 1e-9);
    f.host.SetFocus(f.shell->Bar()->Seek(), true);
    f.host.KeyDown(pui::key::Right, 0);
    LT_CHECK_EQ(seeks, 2);
    // Ctrl+Right is NOT consumed by the slider (global shortcut)
    LT_CHECK(!f.host.KeyDown(pui::key::Right, pui::ModCtrl));
    BarModel idle;
    f.shell->Bar()->SetModel(idle);
    LT_CHECK(!f.shell->Bar()->Seek()->Enabled());
    LT_CHECK(!f.shell->Bar()->ButtonWidget(B_Play)->Enabled());
}

LOGIC_TEST(bar_volume_slider_wheel_and_drag) {
    ShellFixture f(ShellMode::NowPlaying);
    int vol = -1;
    PlayerBar::Actions a;
    a.setVolume = [&](int v) { vol = v; };
    f.shell->Bar()->SetActions(a);
    BarModel m; m.volume = 50;
    f.shell->Bar()->SetModel(m);
    ShellSlider* s = f.shell->Bar()->Volume();
    LT_CHECK(s->Visible() && !s->Bounds().Empty());
    pui::PointerEvent e; e.pt = {s->Bounds().cx(), s->Bounds().cy()}; e.wheelDelta = 1.f;
    LT_CHECK(f.host.PointerWheel(e));
    LT_CHECK_EQ(vol, 55);                                // one notch = the 5 % volume step
    e.wheelDelta = -2.f;
    f.host.PointerWheel(e);
    LT_CHECK_EQ(vol, 40);
    pui::PointerEvent d; d.pt = {s->Track().r, s->Bounds().cy()};
    f.host.PointerMove(d); f.host.PointerDown(d); f.host.PointerUp(d);
    LT_CHECK_EQ(vol, 100);
}

LOGIC_TEST(shell_mode_visibility_and_hit_routing) {
    ShellFixture np(ShellMode::NowPlaying);
    LT_CHECK(np.shell->Title()->Visible());
    LT_CHECK(np.host.HitTest({600, 300}) == np.shell || np.host.HitTest({600, 300}) == nullptr);
    LT_CHECK(np.shell->Bar()->IsAncestorOf(np.host.HitTest({640, 700})));
    ShellFixture fs(ShellMode::FullScreen);
    LT_CHECK(fs.shell->Title()->Visible() && fs.shell->Title()->FullscreenMode());
    LT_CHECK(fs.shell->Title()->MinimizeButton()->Visible() && fs.shell->Title()->ExitFullscreenButton()->Visible() &&
             fs.shell->Title()->CloseButton()->Visible());
}

LOGIC_TEST(shell_layout_scales_pixel_snapped) {
    for (float s : kScales) {
        ShellLayoutIn in;
        in.cwPx = (int)std::lround(1280 * s); in.chPx = (int)std::lround(720 * s); in.scale = s; in.mode = ShellMode::NowPlaying;
        ShellLayout l = ComputeShellLayout(in);
        for (const pui::RectF* r : {&l.title, &l.bar})
            for (float v : {r->l, r->t, r->r, r->b}) LT_CHECK(std::fabs(v * s - std::round(v * s)) < 0.01f);
    }
}
