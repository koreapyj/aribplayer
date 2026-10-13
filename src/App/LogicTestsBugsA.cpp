// Headless: no window, no clipboard.
#include <algorithm>
#include <cmath>
#include <vector>

#include "App.h"
#include "BarLogic.h"
#include "features/AcrylicLogic.h"
#include "Commands.h"
#include "LogicTest.h"
#include "NullShellContext.h"
#include "ShellLogic.h"
#include "features/MiniLogic.h"
#include "pui/Renderer.h"
#include "shell/ShellRoot.h"

using namespace app;

namespace {
struct BarFixture {
    NullShellContext ctx;   // outlives the host: the panel keeps a reference
    pui::UiHost host;
    ShellRoot* shell = nullptr;
    explicit BarFixture(ShellMode mode, float s = 1.f, int wdip = 1280, int hdip = 720) {
        host.SetSize({(float)wdip, (float)hdip});
        shell = static_cast<ShellRoot*>(host.SetRoot(std::make_unique<ShellRoot>(ctx)));
        shell->Title()->SetScale(s);
        shell->Bar()->SetScale(s);
        ShellLayoutIn in;
        in.cwPx = (int)std::lround(wdip * s); in.chPx = (int)std::lround(hdip * s); in.scale = s; in.mode = mode;
        shell->ApplyLayout(ComputeShellLayout(in), mode);
    }
};
}

LOGIC_TEST(acrylic_fallback_decision_covers_windows_disable_conditions) {
    acrylic::Conditions c;
    c.videoShowing = true;
    c.d3d11Presenter = true;
    LT_CHECK(acrylic::DecideFallback(c) == acrylic::FallbackReason::None);
    c.videoShowing = false;
    LT_CHECK(acrylic::DecideFallback(c) == acrylic::FallbackReason::NoVideo);
    c.videoShowing = true;
    c.highContrast = true;
    LT_CHECK(acrylic::DecideFallback(c) == acrylic::FallbackReason::HighContrast);
    c.highContrast = false;
    c.modalDialog = true;
    LT_CHECK(acrylic::DecideFallback(c) == acrylic::FallbackReason::ModalDialog);
    c.modalDialog = false;
    c.d3d11Presenter = false;
    LT_CHECK(acrylic::DecideFallback(c) == acrylic::FallbackReason::PresenterUnavailable);
    c.d3d11Presenter = true;
    c.transparencyEffects = false;
    LT_CHECK(acrylic::DecideFallback(c) == acrylic::FallbackReason::TransparencyEffectsOff);
    c.transparencyEffects = true;
    c.batterySaver = true;
    LT_CHECK(acrylic::DecideFallback(c) == acrylic::FallbackReason::BatterySaver);
    c.batterySaver = false;
    c.remoteSession = true;
    LT_CHECK(acrylic::DecideFallback(c) == acrylic::FallbackReason::RemoteSession);
    c.remoteSession = false;
    c.presenterFailure = true;
    LT_CHECK(acrylic::DecideFallback(c) == acrylic::FallbackReason::PresenterFailure);
}

LOGIC_TEST(acrylic_regions_map_client_dips_to_video_pixels_at_each_dpi) {
    const pui::Theme theme = pui::Theme::Make(false);
    const pui::RectF surface{120, 96, 280, 160};
    for (double scale : {1.0, 1.25, 1.5, 2.0}) {
        ShellLayoutIn in;
        in.cwPx = (int)std::lround(800 * scale);
        in.chPx = (int)std::lround(600 * scale);
        in.scale = (float)scale;
        in.mode = ShellMode::NowPlaying;
        const ShellLayout layout = ComputeShellLayout(in);
        auto region = acrylic::RegionFromDip(surface, layout.video.l, layout.video.t, layout.video.w(), layout.video.h(),
                                             scale, 8.f, theme, 7);
        LT_CHECK(region.has_value());
        if (!region) continue;
        LT_CHECK_EQ(region->x, (int)std::lround(surface.l * scale) - layout.video.l);
        LT_CHECK_EQ(region->y, (int)std::lround(surface.t * scale) - layout.video.t);
        LT_CHECK_EQ(region->width, (int)std::lround(surface.r * scale) - (int)std::lround(surface.l * scale));
        LT_CHECK_EQ(region->height, (int)std::lround(surface.b * scale) - (int)std::lround(surface.t * scale));
        LT_CHECK(std::abs(region->cornerRadius - 8.f * (float)scale) < 0.01f);
        LT_CHECK_EQ(region->tintColor, theme.acrylicTintColor.v);
        LT_CHECK_EQ(region->luminosityColor, theme.acrylicLuminosityColor.v);
        LT_CHECK_EQ(region->z, 7);
    }
    auto clipped = acrylic::RegionFromDip({-8, 40, 24, 72}, 0, 32, 800, 568, 1.0, 4.f, theme, 0);
    LT_CHECK(clipped.has_value() && clipped->x == -8 && clipped->y == 8);
    LT_CHECK(!acrylic::RegionFromDip({-40, 40, -8, 72}, 0, 32, 800, 568, 1.0, 4.f, theme, 0).has_value());
}

LOGIC_TEST(bar_responsive_widgets_follow_the_layout_tiers) {
    for (float scale : {1.f, 1.25f, 1.5f, 2.f}) {
        for (int width : {500, 640, 800, 1000}) {
            BarFixture f(ShellMode::NowPlaying, scale, width, 374);
            BarModel m;
            m.loaded = m.prepared = m.seekable = true; m.dur = 100; m.hasPrev = m.hasNext = true;
            m.chips = {{badge::ChipKind::HwSw, L"H/W"}, {badge::ChipKind::VideoCodec, L"HEVC"}, {badge::ChipKind::AudioCodec, L"AAC"}};
            f.shell->Bar()->SetModel(m);
            const BarLayout& l = f.shell->Bar()->Layout();
            LT_CHECK(l.Sane());
            auto focus = f.host.FocusOrder();
            for (int b = 0; b < B_Count; b++) {
                bool hidden = !l.buttonVisible[b];
                pui::Widget* w = f.shell->Bar()->ButtonWidget(b);
                LT_CHECK(w->Visible() != hidden);
                if (hidden) {
                    LT_CHECK(!w->CanFocus());
                    LT_CHECK(std::find(focus.begin(), focus.end(), w) == focus.end());
                }
            }
            for (int i = 0; i < 3; i++) LT_CHECK(f.shell->Bar()->ChipWidget(i)->Visible() == (i < l.visibleChips));
            for (int i = 0; i < 3; i++) LT_CHECK(!f.shell->Bar()->ChipWidget(i)->CanFocus());
            LT_CHECK(f.shell->Bar()->Volume()->Visible() == l.volumeVisible);
        }
    }
}

LOGIC_TEST(fullscreen_title_caption_buttons_use_client_hit_testing_and_actions) {
    BarFixture f(ShellMode::FullScreen);
    TitleBar* title = f.shell->Title();
    std::vector<int> actions;
    title->onMinimize = [&] { actions.push_back(1); };
    title->onExitFullscreen = [&] { actions.push_back(2); };
    title->onClose = [&] { actions.push_back(3); };

    GlyphButton* buttons[] = {title->MinimizeButton(), title->ExitFullscreenButton(), title->CloseButton()};
    const wchar_t* names[] = {L"Minimize", L"Exit full screen", L"Close"};
    const wchar_t* glyphs[] = {L"\xE921", L"\xE73F", L"\xE8BB"};
    for (size_t i = 0; i < 3; ++i) {
        GlyphButton* button = buttons[i];
        LT_CHECK(button->Visible());
        LT_CHECK(button->GetRole() == pui::Role::Button && button->SupportsInvoke());
        LT_CHECK_EQ(button->Name(), std::wstring(names[i]));
        LT_CHECK_EQ(button->ToolTip(), std::wstring(names[i]));
        LT_CHECK_EQ(button->Glyph(), std::wstring(glyphs[i]));
        LT_CHECK_EQ(button->Bounds().w(), 46.f);
        LT_CHECK_EQ(button->Bounds().h(), kStripDip);
        pui::PointF center{button->Bounds().cx(), button->Bounds().cy()};
        LT_CHECK(f.host.HitTest(center) == button);
        pui::PointerEvent e; e.pt = center; e.button = pui::PointerButton::Left;
        f.host.PointerDown(e);
        f.host.PointerUp(e);
        LT_CHECK_EQ(actions.size(), i + 1);
        if (actions.size() == i + 1) LT_CHECK_EQ(actions.back(), (int)i + 1);
    }

    pui::PointF closeCenter{title->CloseButton()->Bounds().cx(), title->CloseButton()->Bounds().cy()};
    f.host.PointerMove(pui::PointerEvent{closeCenter});
    pui::RecordingRenderer r;
    title->PaintTree(r, pui::Theme::Make(false));
    bool redCloseHover = false, whiteCloseGlyph = false, hasBackdrop = false;
    for (const auto& call : r.calls) {
        if (call.op == pui::RecordingRenderer::Op::FillRect && call.rect.l == title->CloseButton()->Bounds().l &&
            call.rect.r == title->CloseButton()->Bounds().r && call.color.v == 0xFFC42B1Cu) redCloseHover = true;
        if (call.op == pui::RecordingRenderer::Op::Text && call.text == L"\xE8BB" && call.color.v == 0xFFFFFFFFu) whiteCloseGlyph = true;
        if (call.op == pui::RecordingRenderer::Op::FillRect && call.rect.l == title->Bounds().l &&
            call.rect.t == title->Bounds().t && call.rect.r == title->Bounds().r && call.color == f.host.GetTheme().barFill) hasBackdrop = true;
    }
    LT_CHECK(redCloseHover && whiteCloseGlyph && hasBackdrop);

    // windowed: Max replaces "exit full screen" (the Max button itself is HTMAXBUTTON for the window, see App::NcHitTest)
    BarFixture windowed(ShellMode::NowPlaying);
    LT_CHECK(!windowed.shell->Title()->FullscreenMode());
    LT_CHECK(!windowed.shell->Title()->ExitFullscreenButton()->Visible() && windowed.shell->Title()->MaximizeButton()->Visible());
    LT_CHECK(windowed.host.HitTest({1280.f - 23.f, 16.f}) == windowed.shell->Title()->CloseButton());
    LT_CHECK(windowed.host.HitTest({1280.f - 69.f, 16.f}) == windowed.shell->Title()->MaximizeButton());
}

LOGIC_TEST(minimum_width_title_strip_keeps_name_clear_of_the_buttons) {
    for (float scale : {1.f, 1.25f, 1.5f, 2.f}) {
        BarFixture f(ShellMode::NowPlaying, scale, 500, 374);
        TitleBar* title = f.shell->Title();
        title->SetTitle(L"Playlists");
        LT_CHECK_EQ(title->Bounds().w(), 500.f);
        const TitleLayout l = title->Layout();
        LT_CHECK(title->MainMenuButton()->Visible());
        LT_CHECK(title->MainMenuButton()->Bounds().r <= title->PinButton()->Bounds().l);
        LT_CHECK(title->Zone({500.f - 46.f * 1.5f, 16}) == TitleZone::Max);
        pui::RecordingRenderer r;
        title->Paint(r, f.host.GetTheme());
        bool sawName = false;
        for (const auto& c : r.calls) {
            if (c.op != pui::RecordingRenderer::Op::Text || c.text != L"Playlists") continue;
            sawName = true;
            LT_CHECK(c.rect.l >= title->MainMenuButton()->Bounds().r && c.rect.r <= l.pin.l);
        }
        LT_CHECK(sawName);
    }
}

LOGIC_TEST(fullscreen_title_layout_keeps_controls_in_bounds_at_small_widths) {
    for (float scale : {1.f, 1.25f, 1.5f, 2.f}) {
        for (int width : {186, 200, 320}) {
            BarFixture f(ShellMode::FullScreen, scale, width, 240);
            TitleBar* title = f.shell->Title();
            title->SetTitle(L"A long media title");
            GlyphButton* buttons[] = {title->MinimizeButton(), title->ExitFullscreenButton(), title->CloseButton()};
            LT_CHECK(buttons[0]->Bounds().l >= title->Bounds().l);
            LT_CHECK(buttons[0]->Bounds().r <= buttons[1]->Bounds().l);
            LT_CHECK(buttons[1]->Bounds().r <= buttons[2]->Bounds().l);
            LT_CHECK(buttons[2]->Bounds().r <= title->Bounds().r);
            LT_CHECK(title->MainMenuButton()->Bounds().r <= buttons[0]->Bounds().l);
            for (GlyphButton* button : buttons) {
                LT_CHECK(std::fabs(button->Bounds().w() - 46.f) < 0.01f);
                LT_CHECK(std::fabs(button->Bounds().h() - kStripDip) < 0.01f);
            }
            pui::RecordingRenderer r;
            title->PaintTree(r, f.host.GetTheme());
            bool sawTitle = false;
            for (const auto& call : r.calls) if (call.op == pui::RecordingRenderer::Op::Text && call.text == L"A long media title") {
                sawTitle = true;
                LT_CHECK(call.rect.l >= title->MainMenuButton()->Bounds().r);
                LT_CHECK(call.rect.r <= buttons[0]->Bounds().l);
            }
            if (width >= 320) LT_CHECK(sawTitle);
        }
    }
}
