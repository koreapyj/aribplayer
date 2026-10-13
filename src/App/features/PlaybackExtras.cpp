#include "features/PlaybackExtras.h"
#include "Localize.h"

#include <algorithm>
#include <cstdio>

#include "AppContext.h"
#include "BarLogic.h"
#include "features/FiltersPanel.h"
#include "features/FxModel.h"
#include "features/FxUtil.h"
#include "features/OpenLogic.h"
#include "features/SettingsService.h"
#include "wvp/MediaUri.h"
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

void SelectTrack(AppContext& ctx, wvp::StreamType type, int index) {
    auto* p = ctx.Player();
    if (!p) return;
    if (type == wvp::StreamType::Audio) {
        auto mi = p->GetMediaInfo();
        if (mi && mi->AribAudio.Present) {
            p->SetAudioPreference(index > 0 ? 1 : 0);   // ARIB: the index is the audio preference; no announcement
            return;
        }
    }
    if (p->SelectStream(type, index) == 0) AnnounceTrack(ctx, type);
}

void ShowSubtitles(AppContext& ctx) {
    wvp::AppSettings& s = ctx.Svc().settings;
    if (s.showCaptions) return;
    s.showCaptions = true;
    OnSettingsChanged(ctx);
}

void CycleSubtitleTrack(AppContext& ctx) {
    auto* p = ctx.Player();
    auto mi = p ? p->GetMediaInfo() : std::nullopt;
    auto next = mi ? fx::NextSubtitleIndex(mi->Of(wvp::StreamType::Subtitle)) : std::nullopt;
    if (!next) {
        ctx.ShowToast(Tr(L"No subtitles", L"자막 없음"));
        return;
    }
    if (*next >= 0) ShowSubtitles(ctx);
    SelectTrack(ctx, wvp::StreamType::Subtitle, *next);
}

void ToggleSubtitles(AppContext& ctx) {
    auto* p = ctx.Player();
    auto mi = p ? p->GetMediaInfo() : std::nullopt;
    wvp::AppSettings& s = ctx.Svc().settings;
    if (!mi || !fx::ToggleSubtitleVisibility(*mi, s.showCaptions)) {
        ctx.ShowToast(Tr(L"No subtitles", L"자막 없음"));
        return;
    }
    OnSettingsChanged(ctx);
    ctx.ShowToast(s.showCaptions ? Tr(L"Subtitles on", L"자막 켜짐") : Tr(L"Subtitles off", L"자막 꺼짐"));
}

void AdjustSubtitleDelay(AppContext& ctx, int deltaMs) {
    auto* p = ctx.Player();
    if (!p) return;
    const std::int64_t next = deltaMs == 0 ? 0 : std::clamp<std::int64_t>(p->SubtitleDelay() + deltaMs, -3600000, 3600000);
    if (p->SetSubtitleDelay(next) != 0) return;
    wchar_t buf[64];
    swprintf_s(buf, L"%+.1f", (double)next / 1000.0);
    ctx.ShowToast(Tr(L"Subtitle sync: ", L"자막 싱크: ") + std::wstring(buf) + Tr(L" s", L" 초"));
}

int AttachSubtitleFiles(AppContext& ctx, const std::vector<std::wstring>& paths, bool select) {
    auto* p = ctx.Player();
    if (!p) return -1;
    int first = -1;
    for (const auto& path : paths) {
        int rc = p->AddSubtitleFile(wvp::ToUtf8(path));
        if (rc >= 0 && first < 0) first = rc;
        if (rc < 0) ctx.ShowToast(Tr(L"Could not load the subtitle file", L"자막 파일을 불러올 수 없습니다"));
    }
    if (first >= 0 && select) {
        ShowSubtitles(ctx);
        SelectTrack(ctx, wvp::StreamType::Subtitle, first);
    }
    return first;
}

namespace {
void AttachSidecars(AppContext& ctx) {
    auto* p = ctx.Player();
    auto* ctl = ctx.Controller();
    const wvp::MediaItem* item = ctl ? ctl->Current() : nullptr;
    if (!p || !item) return;
    const auto uri = wvp::MediaUri::Parse(item->url);
    if (!uri.IsLocal() || !uri.localPath) return;
    const auto sidecars = open::ListSidecarSubtitles(wvp::ToWide(*uri.localPath));
    if (sidecars.empty()) return;
    bool any = false;
    for (const auto& sc : sidecars) any = p->AddSubtitleFile(wvp::ToUtf8(sc.path)) >= 0 || any;
    if (!any) return;
    auto mi = p->GetMediaInfo();
    if (!mi) return;
    std::vector<open::SidecarTrack> tracks;
    for (const auto& s : mi->Of(wvp::StreamType::Subtitle)) {
        if (!s.external) continue;
        std::string lang = s.Language == "und" ? std::string() : s.Language;
        if (lang.empty()) {
            const std::wstring wpath = wvp::ToWide(s.path);
            for (const auto& sc : sidecars) if (_wcsicmp(sc.path.c_str(), wpath.c_str()) == 0) lang = wvp::ToUtf8(sc.langTag);
        }
        tracks.push_back({s.Index, lang});
    }
    const int pick = open::PickSidecarTrack(tracks, open::PreferredSubtitleLanguages());
    if (pick >= 0) p->SelectStream(wvp::StreamType::Subtitle, pick);
}
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
        AttachSidecars(ctx);   // registered before any track restore, so a restore can resolve external tracks
        ApplySubtitleStyle(ctx);
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
