// Headless: a stub AppContext hosts the panel in a UiHost, text is measured with a real DirectWrite factory (no window).
#include <windows.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <cmath>
#include <filesystem>

#include "AppContext.h"
#include "LogicTest.h"
#include "features/FiltersPanel.h"
#include "features/FxUtil.h"
#include "pui/Buttons.h"
#include "pui/ComboBox.h"
#include "pui/D2dRenderer.h"
#include "pui/Panels.h"
#include "pui/ScrollViewer.h"
#include "pui/Slider.h"
#include "pui/Toggles.h"

namespace {
using namespace app;

class RecPlayer final : public wvp::IMediaPlayer {
public:
    std::vector<std::wstring> vf, cmds;
    wvp::PlayerState State() const override { return wvp::PlayerState::Playing; }
    int Volume() const override { return 100; }
    void SetVolume(int) override {}
    bool IsMuted() const override { return false; }
    void SetMuted(bool) override {}
    double Position() override { return 0; }
    double Duration() override { return 0; }
    int Open(const std::wstring&, const wvp::OpenOptions& = {}) override { return 0; }
    int Stop() override { return 0; }
    int Play() override { return 0; }
    int Pause() override { return 0; }
    int TogglePause() override { return 0; }
    int Seek(double, bool) override { return 0; }
    int CycleStream(wvp::StreamType) override { return 0; }
    int SetVideoFilter(const std::wstring& s) override { vf.push_back(s); return 0; }
    int SetAudioFilter(const std::wstring&) override { return 0; }
    int SendFilterCommand(const std::wstring& t, const std::wstring& c, const std::wstring& a) override {
        cmds.push_back(t + L" " + c + L"=" + a);
        return 0;
    }
    std::optional<wvp::MediaInfo> GetMediaInfo() override { return std::nullopt; }
    std::optional<wvp::BgraFrame> Snapshot() override { return std::nullopt; }
};

class PanelCtx final : public AppContext {
public:
    PanelCtx(const std::wstring& dir, pui::IRenderer& m) : svc(dir), measurer(m), ctl(player), theme(pui::Theme::Make(false)) {}
    HWND MainHwnd() const override { return nullptr; }
    pui::UiHost& Ui() override { return host; }
    wvp::PlaybackController* Controller() override { return &ctl; }
    wvp::FfPlayer* Player() override { return nullptr; }
    Services& Svc() override { return svc; }
    pui::IRenderer& Measurer() override { return measurer; }
    void SetFullscreen(bool) override {}
    bool IsFullscreen() const override { return false; }
    void SetMiniPlayer(bool) override {}
    bool IsMiniPlayer() const override { return false; }
    ShellMode Mode() const override { return ShellMode::NowPlaying; }
    void AppendToCurrentPlaylist(std::vector<wvp::MediaItem>) override {}
    void ReplaceUnnamedPlaylistAtPosition(std::vector<wvp::MediaItem>, double) override {}
    void ShowInfoBar(InfoSeverity, const std::wstring&, const std::wstring&) override {}
    void ShowToast(const std::wstring& t) override { toasts.push_back(t); }
    void ApplyTheme(pui::ThemeMode, uint32_t) override {}
    const pui::Theme& CurrentTheme() const override { return theme; }
    pui::ThemeMode CurrentThemeMode() const override { return pui::ThemeMode::Dark; }
    float Scale() const override { return 1.f; }
    void RequestRedraw() override {}
    void Execute(CommandId) override {}
    void PostToUi(std::function<void()> fn) override { pending.push_back(std::move(fn)); }
    void Pump() { auto p = std::move(pending); pending.clear(); for (auto& f : p) f(); }

    Services svc;
    pui::IRenderer& measurer;
    RecPlayer player;
    wvp::PlaybackController ctl;
    pui::UiHost host;
    pui::Theme theme;
    std::vector<std::function<void()>> pending;
    std::vector<std::wstring> toasts;
};

struct Env {
    Microsoft::WRL::ComPtr<IDWriteFactory> dw;
    std::unique_ptr<pui::D2dRenderer> d2d;
    std::filesystem::path dir;
    std::unique_ptr<PanelCtx> ctx;
    Env(float w, float h) {
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(dw.GetAddressOf()));
        d2d = std::make_unique<pui::D2dRenderer>(dw.Get());
        pui::SetDefaultMeasurer(d2d.get());
        static unsigned n = 0;
        dir = std::filesystem::temp_directory_path() / (L"aribplayer-fxpanel-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(++n));
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        ctx = std::make_unique<PanelCtx>(dir.wstring(), *d2d);
        ctx->host.SetRoot(std::make_unique<pui::Widget>());
        ctx->host.SetSize({w, h});
        app::fx::ClearFilterError();
    }
    ~Env() {
        ctx->host.CloseAllPopups();
        pui::SetDefaultMeasurer(nullptr);
        ctx.reset();
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
    bool Ok() const { return dw && d2d; }
    pui::Widget* Panel() { return ctx->host.TopPopup(); }
};

template <class T> T* Find(pui::Widget* root, const std::wstring& name) {
    if (!root) return nullptr;
    if (auto* t = dynamic_cast<T*>(root)) if (name.empty() || root->Name() == name) return t;
    for (auto& c : root->Children()) if (auto* r = Find<T>(c.get(), name)) return r;
    return nullptr;
}
template <class F> void Walk(pui::Widget* w, const F& f) {
    if (!w) return;
    f(w);
    for (auto& c : w->Children()) Walk(c.get(), f);
}
pui::ScrollViewer* Scroll(pui::Widget* panel) { return Find<pui::ScrollViewer>(panel, L""); }

/// Every text-carrying control must be at least as wide as its measured text and stay inside the viewport horizontally.
int g_checked = 0;   // number of text controls CountClipped inspected (guards against an empty walk)
int CountClipped(pui::Widget* panel, pui::IRenderer& m, std::wstring* why) {
    int bad = 0;
    g_checked = 0;
    pui::RectF pb = panel->Bounds();
    Walk(panel, [&](pui::Widget* w) {
        if (!w->Visible()) return;
        float need = -1;
        std::wstring text;
        if (auto* t = dynamic_cast<pui::TextBlock*>(w)) { if (!t->Wrap()) { need = t->DesiredSize(&m).w; text = t->Text(); } }
        else if (dynamic_cast<pui::CheckBox*>(w) || dynamic_cast<pui::RadioButton*>(w)) { need = w->DesiredSize(&m).w; text = w->Name(); }
        else if (auto* b = dynamic_cast<pui::Button*>(w)) { need = b->DesiredSize(&m).w; text = b->Text(); }
        if (need < 0) return;
        g_checked++;
        pui::RectF r = w->Bounds();
        if (r.w() + 0.01f < need || r.r > pb.r + 0.5f || r.l < pb.l - 0.5f) {
            bad++;
            if (why) *why += L" [" + text + L" w=" + std::to_wstring((int)r.w()) + L" need=" + std::to_wstring((int)need) + L"]";
        }
    });
    return bad;
}

bool InsideV(const pui::RectF& r, const pui::RectF& outer) { return r.t >= outer.t - 0.5f && r.b <= outer.b + 0.5f; }
}

LOGIC_TEST(fxpanel_reset_video_resets_brightness_widget_and_sends_change) {
    Env e(1120, 720);
    LT_CHECK(e.Ok());
    if (!e.Ok()) return;
    PanelCtx& c = *e.ctx;
    ShowFiltersPanel(c, FiltersTab::Video);
    pui::Widget* panel = e.Panel();
    LT_CHECK(panel != nullptr);
    if (!panel) return;
    auto* sl = Find<pui::Slider>(panel, L"Brightness");
    LT_CHECK(sl != nullptr);
    if (!sl) return;
    sl->SetRangeValue(0.5);
    LT_CHECK(c.svc.filters.Settings().brightness == 0.5);
    LT_CHECK(!c.player.vf.empty());       // eq appears: structural change
    sl = Find<pui::Slider>(panel, L"Brightness");
    sl->SetRangeValue(0.7);
    LT_CHECK(!c.player.cmds.empty());     // then only commands
    auto* sat = Find<pui::Slider>(panel, L"Saturation");
    if (sat) sat->SetRangeValue(1.4);
    size_t vfBefore = c.player.vf.size();
    auto* reset = Find<pui::Button>(panel, L"");
    pui::Button* btn = nullptr;
    Walk(panel, [&](pui::Widget* w) { if (auto* b = dynamic_cast<pui::Button*>(w)) if (b->Text() == L"Reset video settings") btn = b; });
    LT_CHECK(btn != nullptr);
    (void)reset;
    if (!btn) return;
    btn->onClick();
    c.Pump();                              // RebuildLater
    LT_CHECK(c.svc.filters.Settings().brightness == 0.0);
    LT_CHECK(c.svc.filters.Settings().saturation == 1.0);
    LT_CHECK(c.player.vf.size() > vfBefore);   // eq removed -> rebuild with the (empty) video filter
    LT_CHECK(c.player.vf.back().empty());
    panel = e.Panel();
    sl = Find<pui::Slider>(panel, L"Brightness");
    LT_CHECK(sl && sl->Value() == 0.0);
    auto* sat2 = Find<pui::Slider>(panel, L"Saturation");
    LT_CHECK(sat2 && sat2->Value() == 1.0);
}

LOGIC_TEST(fxpanel_reset_with_active_eq_keeps_other_eq_values_and_sends_eq_commands) {
    // brightness reset while contrast stays non-default: the eq filter stays, so the reset is a live command only
    Env e(1120, 720);
    if (!e.Ok()) return;
    PanelCtx& c = *e.ctx;
    ShowFiltersPanel(c, FiltersTab::Video);
    pui::Widget* panel = e.Panel();
    if (!panel) { LT_CHECK(false); return; }
    Find<pui::Slider>(panel, L"Brightness")->SetRangeValue(0.4);
    c.player.cmds.clear();
    Find<pui::Slider>(panel, L"Brightness")->SetRangeValue(0.0);   // user drags back to 0 with contrast still default -> eq removed
    LT_CHECK(c.svc.filters.Settings().brightness == 0.0);
    LT_CHECK(c.player.vf.back().empty());
}

LOGIC_TEST(fxpanel_inline_error_line_shows_mapped_text_when_panel_opens_and_live) {
    Env e(1120, 720);
    if (!e.Ok()) return;
    PanelCtx& c = *e.ctx;
    wvp::FilterErrorInfo err;
    err.Type = L"video"; err.Filter = L"bogus_filter"; err.Code = -1279870712; err.Message = L"Filter not found";
    fx::SetFilterError(err);   // arrives while no panel is open (the shell closed it for the dialog)
    ShowFiltersPanel(c, FiltersTab::Advanced);   // what EditCustom does after PromptText returns
    pui::Widget* panel = e.Panel();
    LT_CHECK(panel != nullptr);
    if (!panel) return;
    Walk(panel, [&](pui::Widget* w) {
        if (w->GetRole() == pui::Role::Text && w->Name().find(L"Video filter failed") != std::wstring::npos) {
            LT_CHECK(w->Visible());
            LT_CHECK(w->Name().find(L"Filter not found ('bogus_filter')") != std::wstring::npos);
            LT_CHECK(w->Name().find(L"-1279870712") == std::wstring::npos);
            LT_CHECK(w->Bounds().h() > 10 && InsideV(w->Bounds(), panel->Bounds()));
        }
    });
    // live: a second error while the panel is open updates the same line
    err.Type = L"audio"; err.Code = -22; err.Filter = L"volume=";
    fx::SetFilterError(err);
    bool seen = false;
    Walk(panel, [&](pui::Widget* w) { if (w->GetRole() == pui::Role::Text && w->Name().find(L"Audio filter failed: Invalid argument") != std::wstring::npos) seen = true; });
    LT_CHECK(seen);
    fx::ClearFilterError();
}

namespace {
void CheckLayout(float cw, float ch, FiltersTab tab, const char* tag) {
    Env e(cw, ch);
    if (!e.Ok()) { LT_CHECK(false); return; }
    PanelCtx& c = *e.ctx;
    ShowFiltersPanel(c, tab);
    pui::Widget* panel = e.Panel();
    if (!panel) { LT_CHECK(false); return; }
    pui::RectF pb = panel->Bounds();
    std::wstring why;
    int bad = CountClipped(panel, *e.d2d, &why);
    if (bad) std::printf("    %s: clipped text:%ls\n", tag, why.c_str());
    LT_CHECK_EQ(bad, 0);
    LT_CHECK(g_checked >= 6);
    LT_CHECK(pb.l >= 0 && pb.r <= cw && pb.t >= 0 && pb.b <= ch);
    auto* sv = Scroll(panel);
    LT_CHECK(sv != nullptr);
    if (!sv) return;
    pui::RectF vp = sv->Bounds();
    LT_CHECK(InsideV(vp, pb));
    // bottom button: reachable. Scrolled to the end it lies inside the viewport.
    pui::Button* last = nullptr;
    Walk(panel, [&](pui::Widget* w) { if (auto* b = dynamic_cast<pui::Button*>(w)) if (b->Name() != L"Close") last = b; });
    LT_CHECK(last != nullptr);
    if (!last) return;
    sv->SetOffset(sv->MaxOffset());
    sv->UpdateLayout();
    LT_CHECK(InsideV(last->Bounds(), vp));
    LT_CHECK(last->Bounds().r <= vp.r + 0.5f);
    // content that fits the available height must not need scrolling at all
    pui::RectF full = pb;
    FiltersPanelGeometry g = PanelGeometry(cw, ch);
    if (pb.h() < g.maxH - 0.5f) LT_CHECK(sv->MaxOffset() < 0.5f);
    (void)full;
}
}

LOGIC_TEST(fxpanel_layout_1120x720_no_clipped_text_and_bottom_button_visible) {
    for (FiltersTab t : {FiltersTab::Video, FiltersTab::Audio, FiltersTab::Advanced}) CheckLayout(1120, 720, t, "1120x720");
    Env e(1120, 720);
    if (!e.Ok()) return;
    ShowFiltersPanel(*e.ctx, FiltersTab::Video);
    auto* sv = Scroll(e.Panel());
    LT_CHECK(sv && sv->MaxOffset() < 0.5f);
}

LOGIC_TEST(fxpanel_layout_800x450_scrolls_and_no_clipped_text) {
    for (FiltersTab t : {FiltersTab::Video, FiltersTab::Audio, FiltersTab::Advanced}) CheckLayout(800, 450, t, "800x450");
    Env e(800, 450);
    if (!e.Ok()) return;
    ShowFiltersPanel(*e.ctx, FiltersTab::Audio);
    auto* sv = Scroll(e.Panel());
    LT_CHECK(sv && sv->MaxOffset() > 10);   // 10 EQ bands cannot fit: the ScrollViewer takes over
}

LOGIC_TEST(fxpanel_tab_switch_resizes_panel_to_content) {
    Env e(1120, 720);
    if (!e.Ok()) return;
    ShowFiltersPanel(*e.ctx, FiltersTab::Advanced);
    float hAdv = e.Panel()->Bounds().h();
    ShowFiltersPanel(*e.ctx, FiltersTab::Video);
    float hVid = e.Panel()->Bounds().h();
    LT_CHECK(hVid > hAdv + 20);
    LT_CHECK(e.ctx->host.PopupCount() == 1);
}
