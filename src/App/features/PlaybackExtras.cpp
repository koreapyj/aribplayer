#include "features/PlaybackExtras.h"
#include "Localize.h"

#include <algorithm>

#include "AppContext.h"
#include "BarLogic.h"
#include "features/FiltersPanel.h"
#include "features/FxModel.h"
#include "features/FxUtil.h"
#include "wvp/win32/Utf.h"
#include "pui/Glyphs.h"
#include "pui/InfoBar.h"

namespace app {

namespace {
bool g_audioOnly = false;
pui::Widget* g_errBar = nullptr;
UINT_PTR g_errTimer = 0;

void AnnounceTrack(AppContext& ctx, wvp::StreamType type) {
    fx::SetUiTimer(ctx.MainHwnd(), 150, [&ctx, type] {
        auto* p = ctx.Player();
        if (!p) return;
        auto mi = p->GetMediaInfo();
        if (!mi) return;
        auto streams = mi->Of(type);
        if (type == wvp::StreamType::Subtitle) streams = fx::CycleableSubtitles(streams);
        std::wstring t = fx::TrackToast(type, streams);
        if (!t.empty()) ctx.ShowToast(t);
    }, false);
}

}

void CycleTrack(AppContext& ctx, wvp::StreamType type) {
    auto* p = ctx.Player();
    if (!p) return;
    if (type == wvp::StreamType::Subtitle) {
        auto mi = p->GetMediaInfo();
        if (!mi || fx::CycleableSubtitles(mi->Of(wvp::StreamType::Subtitle)).empty()) {
            ctx.ShowToast(Tr(L"No subtitles", L"자막 없음"));
            return;
        }
    }
    if (type == wvp::StreamType::Audio) {
        auto mi = p->GetMediaInfo();
        if (mi && mi->AribAudio.Present) {      // ARIB: the cycle key toggles audio 1 / audio 2 (the preference)
            int pref = p->AudioPreference() ? 0 : 1;
            p->SetAudioPreference(pref);
            ctx.ShowToast(pref ? Tr(L"Audio 2", L"소리 2") : Tr(L"Audio 1", L"소리 1"));
            return;
        }
    }
    p->CycleStream(type);
    AnnounceTrack(ctx, type);
}

void SelectTrack(AppContext& ctx, wvp::StreamType type, int index) {
    auto* p = ctx.Player();
    if (!p) return;
    auto mi = p->GetMediaInfo();
    if (!mi) return;
    if (type == wvp::StreamType::Audio && mi->AribAudio.Present) {
        p->SetAudioPreference(index > 0 ? 1 : 0);   // ARIB: no stream cycling, no announcement
        return;
    }
    auto list = mi->Of(type);
    if (type == wvp::StreamType::Subtitle) list = fx::CycleableSubtitles(list);
    if (list.empty()) return;
    int n = fx::CyclesNeeded(list, index, type == wvp::StreamType::Subtitle);
    for (int i = 0; i < n; i++) p->CycleStream(type);
    AnnounceTrack(ctx, type);
}

void ShowPlaybackError(AppContext& ctx, const wvp::PlayerErrorInfo& e) {
    pui::UiHost& ui = ctx.Ui();
    if (g_errBar && ui.IsPopupOpen(g_errBar)) ui.ClosePopup(g_errBar);
    g_errBar = nullptr;
    fx::KillUiTimer(ctx.MainHwnd(), g_errTimer);
    g_errTimer = 0;

    auto bar = std::make_unique<pui::InfoBar>();
    pui::InfoBar* raw = bar.get();
    bar->SetSeverity(pui::InfoBarSeverity::Error);
    bar->SetTitle(Tr(L"Playback error", L"재생 오류"));
    std::wstring msg = e.Message.empty() ? Tr(L"The media could not be played.", L"미디어를 재생할 수 없습니다.") : wvp::ToWide(e.Message);
    if (!e.Url.empty()) {
        const std::wstring url = wvp::ToWide(e.Url);
        size_t i = url.find_last_of(L"\\/");
        msg += L" (" + (i == std::wstring::npos ? url : url.substr(i + 1)) + L")";
    }
    bar->SetMessage(msg);
    bar->SetClosable(true);
    bar->onClosed = [&ctx, raw] { if (ctx.Ui().IsPopupOpen(raw)) ctx.Ui().ClosePopup(raw); };

    pui::SizeF cs = ui.Size();
    float w = std::min(560.f, std::max(0.f, cs.w - 16.f));
    float y = ctx.IsFullscreen() ? 8.f : kStripDip + 8.f;
    float h = std::min(raw->HeightForWidth(&ctx.Measurer(), w), std::max(0.f, cs.h - y - 8.f));
    float x = std::clamp(std::round((cs.w - w) / 2), 0.f, std::max(0.f, cs.w - w));
    y = std::clamp(y, 0.f, std::max(0.f, cs.h - h));
    raw->SetBounds(pui::RectF::XYWH(x, y, w, h));
    pui::PopupOptions po;
    po.lightDismiss = false;
    po.takeFocus = false;
    g_errBar = ui.OpenPopup(std::move(bar), po);
    g_errTimer = fx::SetUiTimer(ctx.MainHwnd(), 12000, [&ctx, w = g_errBar] {
        if (g_errBar == w && ctx.Ui().IsPopupOpen(w)) ctx.Ui().ClosePopup(w);
        if (g_errBar == w) g_errBar = nullptr;
    }, false);
}

bool AudioOnlyMedia() { return g_audioOnly; }

void InitPlaybackExtras(AppContext& ctx) {
    auto hookPlayer = [&ctx] {
        if (auto* p = ctx.Player()) {
            p->FilterError.add([&ctx](const wvp::FilterErrorInfo& e) {
                fx::SetFilterError(e);
                ctx.ShowToast(fx::FormatFilterError(e));
            });
        }
    };
    if (ctx.Player()) hookPlayer();
    else ctx.OnPlayerReady.add(hookPlayer);

    ctx.OnMediaOpened.add([&ctx](const wvp::MediaItem*) {
        g_audioOnly = false;
        ctx.RequestRedraw();
    });
    ctx.OnMediaPrepared.add([&ctx](const wvp::MediaPreparedInfo& p) {
        g_audioOnly = !p.HasVideo && p.HasAudio;
        RefreshZoomAspect(ctx);
        ctx.RequestRedraw();
    });
    ctx.OnModeChanged.add([&ctx](ShellMode) {
        RefreshZoomAspect(ctx);
        ctx.RequestRedraw();
    });
}

}
