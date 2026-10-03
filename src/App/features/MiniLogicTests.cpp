// Headless: no window, no clipboard.
#include <cmath>
#include <vector>

#include "LogicTest.h"
#include "features/MiniBar.h"
#include "features/MiniLogic.h"
#include "features/MiniPlayer.h"

namespace app {
namespace {
const float kScales[] = {1.f, 1.25f, 1.5f, 2.f, 3.f};
bool InsideRect(const PxRect& r, const PxRect& w) { return r.l >= w.l && r.t >= w.t && r.r <= w.r && r.b <= w.b; }
}

LOGIC_TEST(mini_default_placement_bottom_right) {
    for (float s : kScales) {
        PxRect work{0, 0, 1920, 1040};
        PxRect r = MiniDefaultRect(work, s);
        int m = (int)std::lround(16 * s);
        LT_CHECK_EQ(r.r, work.r - m);
        LT_CHECK_EQ(r.b, work.b - m);
        LT_CHECK_EQ(r.w(), (int)std::lround(400 * s));
        LT_CHECK_EQ(r.h(), (int)std::lround(225 * s));
        LT_CHECK(InsideRect(r, work));
        PxRect second{-1920, 100, 0, 1180};
        PxRect r2 = MiniDefaultRect(second, s);
        LT_CHECK(InsideRect(r2, second));
        LT_CHECK_EQ(r2.r, second.r - m);
    }
    PxRect tiny{0, 0, 300, 170};
    PxRect r = MiniDefaultRect(tiny, 1.f);
    LT_CHECK(InsideRect(r, tiny));
    LT_CHECK(!r.Empty());
}

LOGIC_TEST(mini_clamp_work_area_and_dpi) {
    for (float s : kScales) {
        PxRect work{100, 50, 1700, 1000};
        PxSize mn = MiniMinSizePx(s);
        LT_CHECK_EQ(mn.w, (int)std::lround(320 * s));
        LT_CHECK_EQ(mn.h, (int)std::lround(180 * s));
        PxRect small = MiniClampRect({0, 0, 10, 10}, work, s);
        LT_CHECK_EQ(small.w(), mn.w);
        LT_CHECK_EQ(small.h(), mn.h);
        LT_CHECK(InsideRect(small, work));
        PxRect big = MiniClampRect({-500, -500, 9000, 9000}, work, s);
        LT_CHECK(big == work);
        PxRect off = MiniClampRect({1500, 900, 1500 + mn.w, 900 + mn.h}, work, s);
        LT_CHECK(InsideRect(off, work));
        LT_CHECK_EQ(off.w(), mn.w);
        PxRect ok{300, 200, 300 + mn.w + 50, 200 + mn.h + 50};
        LT_CHECK(MiniClampRect(ok, work, s) == ok);
    }
}

LOGIC_TEST(mini_restore_placement_multi_monitor) {
    std::vector<PxRect> works{{0, 0, 1920, 1040}, {1920, 0, 3840, 1040}};
    PxRect cur = works[0];
    wvp::WindowPlacement none;
    LT_CHECK(MiniResolvePlacement(none, works, cur, 1.f) == MiniDefaultRect(cur, 1.f));
    wvp::WindowPlacement p;
    p.x = 2000; p.y = 100; p.w = 400; p.h = 225; p.valid = true;
    PxRect r = MiniResolvePlacement(p, works, cur, 1.f);
    LT_CHECK_EQ(r.l, 2000);
    LT_CHECK_EQ(r.t, 100);
    LT_CHECK(InsideRect(r, works[1]));
    p.x = 5000;
    r = MiniResolvePlacement(p, works, cur, 1.f);
    LT_CHECK(InsideRect(r, cur));
    LT_CHECK_EQ(r.w(), 400);
    LT_CHECK_EQ(r.r, cur.r - 16);
    p.x = 1800;
    r = MiniResolvePlacement(p, works, cur, 1.f);
    LT_CHECK(InsideRect(r, works[0]) || InsideRect(r, works[1]));
    // size is stored in DIP, so a DPI change keeps the logical size
    PxRect saved{100, 100, 100 + 600, 100 + 300};   // at scale 1.5
    wvp::WindowPlacement q = MiniMakePlacement(saved, 1.5f);
    LT_CHECK(q.valid);
    LT_CHECK_EQ(q.w, 400);
    LT_CHECK_EQ(q.h, 200);
    PxRect back = MiniResolvePlacement(q, works, cur, 2.f);
    LT_CHECK_EQ(back.w(), 800);
    LT_CHECK_EQ(back.h(), 400);
}

LOGIC_TEST(mini_bar_layout_sane_and_progressive) {
    for (float s : kScales) {
        bool prevTime = true, prevPN = true, prevPlay = true;
        for (int wdip = 1200; wdip >= 4; wdip -= 4) {
            int wpx = (int)std::lround(wdip * s);
            MiniBarLayout l = ComputeMiniBarLayout(wpx, s);
            LT_CHECK(l.Sane());
            LT_CHECK(l.slider.w() > 0);
            // progressive: once an element is hidden it stays hidden at narrower widths (time -> prev/next -> play)
            if (!prevTime) LT_CHECK(!l.showTime);
            if (!prevPN) LT_CHECK(!l.showPrev && !l.showNext);
            if (!prevPlay) LT_CHECK(!l.showPlay);
            LT_CHECK_EQ(l.showPrev, l.showNext);
            if (l.showTime) LT_CHECK(l.showPrev);
            if (l.showPrev) LT_CHECK(l.showPlay);
            prevTime = l.showTime; prevPN = l.showPrev; prevPlay = l.showPlay;
            const pui::RectF* v[5]; int n = 0;
            if (l.showTime) v[n++] = &l.time;
            if (l.showPrev) v[n++] = &l.prev;
            if (l.showPlay) v[n++] = &l.play;
            if (l.showNext) v[n++] = &l.next;
            if (l.showExit) v[n++] = &l.exit;
            for (int i = 0; i < n; i++)
                for (int j = i + 1; j < n; j++) LT_CHECK(!v[i]->Intersects(*v[j]));
            auto snapped = [&](const pui::RectF& r) {
                return std::fabs(r.l * s - std::round(r.l * s)) < 1e-3 && std::fabs(r.r * s - std::round(r.r * s)) < 1e-3 &&
                       std::fabs(r.t * s - std::round(r.t * s)) < 1e-3 && std::fabs(r.b * s - std::round(r.b * s)) < 1e-3;
            };
            LT_CHECK(snapped(l.slider));
            if (l.showPlay) LT_CHECK(snapped(l.play));
        }
        // the minimum window (320 DIP) shows the buttons; a 200 DIP bar drops time and prev/next but keeps play and exit
        MiniBarLayout full = ComputeMiniBarLayout((int)std::lround(320 * s), s);
        LT_CHECK(full.showPrev && full.showPlay && full.showNext && full.showExit);
        MiniBarLayout narrow = ComputeMiniBarLayout((int)std::lround(200 * s), s);
        LT_CHECK(!narrow.showTime && !narrow.showPrev && narrow.showPlay && narrow.showExit);
        MiniBarLayout wide = ComputeMiniBarLayout((int)std::lround(600 * s), s);
        LT_CHECK(wide.showTime);
    }
}

LOGIC_TEST(mini_shell_layout) {
    for (float s : kScales)
        for (auto sz : {PxSize{400, 225}, PxSize{320, 180}, PxSize{1920, 1080}}) {
            int w = (int)std::lround(sz.w * s), h = (int)std::lround(sz.h * s);
            ShellLayout l = ComputeMiniShellLayout(w, h, s);
            LT_CHECK_EQ(l.video.l, 0);
            LT_CHECK_EQ(l.video.t, 0);
            LT_CHECK_EQ(l.video.w(), w);
            LT_CHECK_EQ(l.video.h(), h);
            LT_CHECK(l.title.Empty() && l.contentPx.Empty());
            LT_CHECK(l.barOverlaysVideo && !l.titleSlides);
            LT_CHECK_EQ(l.barPx.h(), MiniBarHeightPx(s));
            LT_CHECK_EQ(l.barPx.b, h);
            LT_CHECK_EQ(l.barPx.w(), w);
            LT_CHECK(std::fabs(l.bar.h() - kMiniBarDip) < 1.0f);
            LT_CHECK(std::fabs(l.widthDip - w / s) < 1e-3);
        }
    ShellLayout t = ComputeMiniShellLayout(100, 30, 1.f);
    LT_CHECK_EQ(t.barPx.h(), 30);
}

LOGIC_TEST(mini_hit_test_caption_vs_controls) {
    for (float s : kScales) {
        int cw = (int)std::lround(400 * s), ch = (int)std::lround(225 * s);
        auto z = [&](float xd, float yd, bool hittable = true) {
            return ClassifyMiniPoint((int)std::lround(xd * s), (int)std::lround(yd * s), cw, ch, s, hittable);
        };
        LT_CHECK(z(200, 80) == MiniZone::Caption);
        LT_CHECK(z(5, 5) == MiniZone::Caption);
        LT_CHECK(z(-1, 10) == MiniZone::None);
        LT_CHECK(ClassifyMiniPoint(cw, 10, cw, ch, s, true) == MiniZone::None);
        float barTop = (float)(ch - MiniBarHeightPx(s)) / s;
        MiniBarLayout l = ComputeMiniBarLayout(cw, s);
        LT_CHECK(z(200, barTop + l.slider.cy()) == MiniZone::Control);
        LT_CHECK(z(l.play.cx(), barTop + l.play.cy()) == MiniZone::Control);
        LT_CHECK(z(l.prev.cx(), barTop + l.prev.cy()) == MiniZone::Control);
        LT_CHECK(z(l.next.cx(), barTop + l.next.cy()) == MiniZone::Control);
        LT_CHECK(z(l.exit.cx(), barTop + l.exit.cy()) == MiniZone::Control);
        LT_CHECK(z(l.play.cx(), barTop + l.play.cy(), false) == MiniZone::Caption);
        LT_CHECK(z(l.exit.cx() - 40, barTop + l.exit.t + 1) == MiniZone::Caption);
    }
}

LOGIC_TEST(mini_enter_exit_round_trip) {
    MiniSession s;
    LT_CHECK(!s.active);
    s.Begin({100, 120, 1380, 840}, true, false);
    LT_CHECK(s.active && s.normalMaximized && !s.normalTopmost);
    LT_CHECK(s.normalRect == (PxRect{100, 120, 1380, 840}));
    s.End();
    LT_CHECK(!s.active);
    ModeState player;
    LT_CHECK(WithMini(player, true).Mode() == ShellMode::MiniPlayer);
    LT_CHECK(WithMini(WithMini(player, true), false).Mode() == ShellMode::NowPlaying);
    ModeState fs; fs.fullscreen = true;
    LT_CHECK(WithMini(fs, true).Mode() == ShellMode::MiniPlayer && !WithMini(fs, true).fullscreen);
    std::vector<PxRect> works{{0, 0, 1920, 1040}};
    PxRect usedRect{1200, 700, 1600, 925};
    wvp::WindowPlacement saved = MiniMakePlacement(usedRect, 1.f);
    LT_CHECK(MiniResolvePlacement(saved, works, works[0], 1.f) == usedRect);
}

LOGIC_TEST(mini_aspect_locked_sizing) {
    const double a169 = 16.0 / 9.0;
    LT_CHECK(std::fabs(MiniAspectForMedia(0, 0) - a169) < 1e-9);
    LT_CHECK(std::fabs(MiniAspectForMedia(768, 576) - 4.0 / 3.0) < 1e-9);
    LT_CHECK(std::fabs(MiniAspectForMedia(10000, 10) - a169) < 1e-9);
    PxSize mn{320, 180};
    auto ratioOk = [](const PxRect& r, double a) { return std::fabs(r.w() - r.h() * a) <= 1.0; };
    for (double asp : {a169, 4.0 / 3.0, 21.0 / 9.0, 1.0}) {
        for (int edge = 1; edge <= 8; edge++) {
            PxRect orig{100, 200, 100 + 640, 200 + 360};
            PxRect prop = orig;
            if (edge == 1 || edge == 4 || edge == 7) prop.l -= 150;
            if (edge == 2 || edge == 5 || edge == 8) prop.r += 90;
            if (edge == 3 || edge == 4 || edge == 5) prop.t -= 40;
            if (edge == 6 || edge == 7 || edge == 8) prop.b += 70;
            PxRect out = MiniConstrainSizing(prop, edge, asp, mn);
            LT_CHECK(ratioOk(out, asp));
            LT_CHECK(out.w() >= mn.w && out.h() >= mn.h);
            // the opposite edge/corner stays anchored
            if (edge == 1 || edge == 4 || edge == 7) LT_CHECK_EQ(out.r, prop.r);
            if (edge == 2 || edge == 5 || edge == 8) LT_CHECK_EQ(out.l, prop.l);
            if (edge == 3 || edge == 4 || edge == 5) LT_CHECK_EQ(out.b, prop.b);
            if (edge == 6 || edge == 7 || edge == 8 || edge == 1 || edge == 2) LT_CHECK_EQ(out.t, prop.t);
        }
    }
    PxRect r1 = MiniConstrainSizing({0, 0, 800, 300}, 2, a169, mn);
    LT_CHECK_EQ(r1.w(), 800); LT_CHECK_EQ(r1.h(), 450);
    PxRect r2 = MiniConstrainSizing({0, 0, 700, 270}, 6, a169, mn);
    LT_CHECK_EQ(r2.h(), 270); LT_CHECK_EQ(r2.w(), 480);
    // below the minimum: clamped on both axes, aspect kept (a 4:3 media needs 320x240 because 180 high would be too narrow)
    PxRect r3 = MiniConstrainSizing({0, 0, 100, 50}, 8, 4.0 / 3.0, mn);
    LT_CHECK(r3.w() >= 320 && r3.h() >= 180 && ratioOk(r3, 4.0 / 3.0));
    PxRect r4 = MiniConstrainSizing({0, 0, 100, 50}, 6, 1.0, mn);
    LT_CHECK(r4.w() >= 320 && r4.h() >= 180 && ratioOk(r4, 1.0));
    LT_CHECK(MiniConstrainSizing({0, 0, 640, 360}, 8, 0.0, mn).h() == 360);   // invalid aspect falls back to 16:9
}

LOGIC_TEST(mini_keyboard_remap) {
    using C = CommandId;
    LT_CHECK(MiniRemapCommand(C::Escape, true) == C::ToggleMiniPlayer);
    LT_CHECK(MiniRemapCommand(C::Escape, false) == C::Escape);
    LT_CHECK(MiniRemapCommand(C::ToggleControlPanel, true) == C::None);
    LT_CHECK(MiniRemapCommand(C::ToggleControlPanel, false) == C::ToggleControlPanel);
    LT_CHECK(MiniRemapCommand(C::PlayPause, true) == C::PlayPause);
    LT_CHECK(MiniRemapCommand(C::ToggleFullscreen, true) == C::ToggleFullscreen);
    LT_CHECK(MiniRemapCommand(C::ToggleMiniPlayer, true) == C::ToggleMiniPlayer);
    LT_CHECK(MiniRemapCommand(LookupKey(0x1B, 0), true) == C::ToggleMiniPlayer);
}

LOGIC_TEST(mini_bar_widget_shape) {
    pui::UiHost host;
    pui::Widget* r = host.SetRoot(std::make_unique<pui::Widget>(pui::Role::Window));
    MiniBar* bar = r->Add<MiniBar>();
    const float s = 1.5f;
    bar->SetScale(s);
    host.SetSize({400, 225});
    bar->SetBounds(pui::RectF::XYWH(0, 225 - kMiniBarDip, 400, kMiniBarDip));

    LT_CHECK(bar->GetRole() == pui::Role::Group);
    LT_CHECK_EQ(bar->Name(), std::wstring(L"Mini player controls"));
    LT_CHECK(bar->Seek()->GetRole() == pui::Role::Slider);
    LT_CHECK_EQ(bar->Seek()->Name(), std::wstring(L"Seek"));
    LT_CHECK_EQ(bar->PrevButton()->Name(), std::wstring(L"Previous"));
    LT_CHECK_EQ(bar->NextButton()->Name(), std::wstring(L"Next"));
    LT_CHECK_EQ(bar->PlayButton()->Name(), std::wstring(L"Play"));
    LT_CHECK_EQ(bar->ExitButton()->Name(), std::wstring(L"Back to full view"));
    LT_CHECK_EQ(bar->ExitButton()->ToolTip(), std::wstring(L"Back to full view"));
    LT_CHECK_EQ(bar->PlayButton()->ToolTip(), std::wstring(L"Play"));
    LT_CHECK_EQ(bar->ExitButton()->AutomationId(), std::wstring(L"MiniBackToFullView"));
    LT_CHECK_EQ(bar->PlayButton()->AutomationId(), std::wstring(L"MiniPlayPause"));
    LT_CHECK(bar->PlayButton()->SupportsInvoke() && bar->ExitButton()->SupportsInvoke());
    LT_CHECK(bar->Seek()->SupportsRangeValue());
    LT_CHECK_EQ(bar->Children().size(), (size_t)5);

    const MiniBarLayout& l = bar->Layout();
    LT_CHECK(l.Sane() && l.showPrev && l.showNext && l.showPlay && l.showExit);
    pui::RectF b = bar->Bounds();
    for (pui::Widget* w : {(pui::Widget*)bar->Seek(), (pui::Widget*)bar->PrevButton(), (pui::Widget*)bar->PlayButton(),
                            (pui::Widget*)bar->NextButton(), (pui::Widget*)bar->ExitButton()}) {
        LT_CHECK(w->Visible());
        LT_CHECK(w->Bounds().l >= b.l - 0.01f && w->Bounds().r <= b.r + 0.01f && w->Bounds().t >= b.t - 0.01f && w->Bounds().b <= b.b + 0.01f);
    }

    // Seek range = duration; slider callbacks receive a fraction of the duration.
    BarModel m;
    m.loaded = m.prepared = m.seekable = true;
    m.dur = 200; m.pos = 50; m.playing = true; m.hasPrev = false; m.hasNext = true;
    bar->SetModel(m);
    LT_CHECK_EQ(bar->PlayButton()->Name(), std::wstring(L"Pause"));
    LT_CHECK(!bar->PrevButton()->Enabled() && bar->NextButton()->Enabled() && bar->PlayButton()->Enabled());
    LT_CHECK(bar->Seek()->Enabled());
    pui::RangeInfo ri = bar->Seek()->GetRange();
    LT_CHECK_EQ(ri.maximum, 200.0);
    LT_CHECK_EQ(ri.value, 50.0);
    m.loaded = false;
    bar->SetModel(m);
    LT_CHECK(!bar->PlayButton()->Enabled() && !bar->Seek()->Enabled());

    CommandId got = CommandId::None;
    double seek = -1;
    MiniBar::Actions act;
    act.command = [&](CommandId c) { got = c; };
    act.seekTo = [&](double f) { seek = f; };
    bar->SetActions(act);
    m.loaded = true; m.hasPrev = true;
    bar->SetModel(m);
    bar->ExitButton()->Invoke();
    LT_CHECK(got == CommandId::ToggleMiniPlayer);
    bar->PlayButton()->Invoke();
    LT_CHECK(got == CommandId::PlayPause);
    bar->PrevButton()->Invoke();
    LT_CHECK(got == CommandId::Previous);
    bar->NextButton()->Invoke();
    LT_CHECK(got == CommandId::Next);
    bar->Seek()->SetRangeValue(100);
    LT_CHECK(std::fabs(seek - 0.5) < 1e-6);

    bar->SetBounds(pui::RectF::XYWH(0, 0, 200, kMiniBarDip));
    LT_CHECK(!bar->PrevButton()->Visible() && !bar->NextButton()->Visible() && bar->PlayButton()->Visible() && bar->ExitButton()->Visible());
    bar->SetBounds(pui::RectF::XYWH(0, 0, 320, kMiniBarDip));
    LT_CHECK(bar->PrevButton()->Visible() && bar->NextButton()->Visible());

    bar->SetBounds(pui::RectF::XYWH(0, 0, 600, kMiniBarDip));
    pui::RecordingRenderer rec;
    pui::Theme th = pui::Theme::Make(false);
    bar->PaintTree(rec, th);
    bool sawFill = false, sawTime = false;
    for (auto& c : rec.calls) {
        if (c.op == pui::RecordingRenderer::Op::FillRect && c.color == th.barFill) sawFill = true;
        if (c.op == pui::RecordingRenderer::Op::Text && c.text.find(L" / ") != std::wstring::npos) sawTime = true;
    }
    LT_CHECK(sawFill && sawTime);
}

}
