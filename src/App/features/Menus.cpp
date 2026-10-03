#include "features/Menus.h"

#include <cmath>

#include "AppContext.h"
#include "Localize.h"
#include "features/FiltersPanel.h"
#include "features/FxModel.h"
#include "features/NativeMenu.h"
#include "features/PlaybackExtras.h"
#include "features/SettingsLogic.h"
#include "features/SettingsService.h"

namespace app {
namespace menus {

using pui::MenuItemDesc;
namespace G = pui::glyph;

std::vector<MenuItemDesc> BuildSubtitleItems(const MenuState& s, std::function<void(const MenuCmd&)> run) {
    auto cmd = [run](Cmd c, double v = 0, int i = 0, bool f = false) {
        return [run, c, v, i, f] { if (run) run(MenuCmd{c, v, i, f}); };
    };
    std::vector<MenuItemDesc> sub;
    if (s.subs.empty()) return sub;
    // superimpose is listed for visibility but cannot be selected through ffp_cycle_stream.
    const auto cycleable = fx::CycleableSubtitles(s.subs);
    bool any = false;
    for (const auto& x : cycleable) any = any || x.Selected;
    MenuItemDesc off = MenuItemDesc::Radio(Tr(L"Off", L"끄기"), !any, cmd(Cmd::SelectSubtitle, 0, -1));
    if (cycleable.empty()) off.Disabled();
    sub.push_back(std::move(off));
    int cycleIndex = 0;
    for (size_t i = 0; i < s.subs.size(); i++) {
        const auto& stream = s.subs[i];
        if (!fx::IsCycleableSubtitle(stream)) {
            sub.push_back(MenuItemDesc::Item(fx::TrackName(stream, (int)i + 1) + Tr(L" (superimpose)", L" (슈퍼임포즈)"), {}).Disabled());
            continue;
        }
        sub.push_back(MenuItemDesc::Radio(fx::TrackName(stream, (int)i + 1), stream.Selected, cmd(Cmd::SelectSubtitle, 0, cycleIndex++)));
    }
    return sub;
}

std::vector<MenuItemDesc> BuildMenu(const MenuState& s, std::function<void(const MenuCmd&)> run) {
    auto cmd = [run](Cmd c, double v = 0, int i = 0, bool f = false) {
        return [run, c, v, i, f] { if (run) run(MenuCmd{c, v, i, f}); };
    };
    auto exec = [&](CommandId c) { return cmd(Cmd::Execute, 0, (int)c); };
    auto acc = [](CommandId c) { return ShortcutText(c); };
    auto item = [&](const wchar_t* text, CommandId c, wchar_t glyph = 0, bool enabled = true) {
        MenuItemDesc d = MenuItemDesc::Item(text, exec(c), acc(c), glyph);
        if (!enabled) d.Disabled();
        return d;
    };
    std::vector<MenuItemDesc> m;

    m.push_back(item(Tr(L"Open file(s)…", L"파일 열기…"), CommandId::OpenFiles, G::OpenFile));
    m.push_back(item(Tr(L"Open folder…", L"폴더 열기…"), CommandId::OpenFolder, G::OpenFolder));
    m.push_back(item(Tr(L"Open URL…", L"주소 열기…"), CommandId::OpenUrl, G::OpenUrl));
    m.push_back(item(Tr(L"Close", L"닫기"), CommandId::Stop, 0, s.hasMedia));
    m.push_back(MenuItemDesc::Separator());

    {
        std::vector<MenuItemDesc> pb;
        pb.push_back(item(s.bar.playing ? Tr(L"Pause", L"일시 정지") : Tr(L"Play", L"재생"), CommandId::PlayPause,
                          s.bar.playing ? G::Pause : G::Play, s.hasMedia || s.bar.canPlaySelection));
        pb.push_back(item(Tr(L"Previous file", L"이전 파일"), CommandId::Previous, G::PreviousLegacy, s.bar.hasPrev));
        pb.push_back(item(Tr(L"Next file", L"다음 파일"), CommandId::Next, G::NextLegacy, s.bar.hasNext));
        pb.push_back(MenuItemDesc::Separator());
        pb.push_back(item(Tr(L"Back 5 seconds", L"5초 뒤로"), CommandId::SeekBack5, 0, s.hasMedia));
        pb.push_back(item(Tr(L"Forward 5 seconds", L"5초 앞으로"), CommandId::SeekForward5, 0, s.hasMedia));
        pb.push_back(MenuItemDesc::Separator());
        std::vector<MenuItemDesc> speed;
        for (double sp : wvp::filters::kSpeedPresets)
            speed.push_back(MenuItemDesc::Radio(fx::SpeedLabel(sp), fx::SpeedIndex(s.speed) >= 0 && std::abs(sp - s.speed) < 1e-6,
                                                cmd(Cmd::SetSpeed, sp)));
        pb.push_back(MenuItemDesc::Sub(Tr(L"Speed", L"재생 속도"), std::move(speed), G::PlaybackRate));
        std::vector<MenuItemDesc> repeat;
        const wchar_t* labels[] = {Tr(L"Off", L"사용 안 함"), Tr(L"All", L"전체"), Tr(L"One", L"한 항목")};
        for (int value = 0; value < 3; value++)
            repeat.push_back(MenuItemDesc::Radio(labels[value], s.bar.repeat == value, cmd(Cmd::SetRepeat, 0, value)));
        pb.push_back(MenuItemDesc::Sub(Tr(L"Repeat", L"반복"), std::move(repeat)));
        pb.push_back(MenuItemDesc::Check(Tr(L"Shuffle", L"무작위 재생"), s.bar.shuffle, [run](bool v) {
            if (run) run(MenuCmd{Cmd::SetShuffle, 0, 0, v});
        }));
        pb.push_back(MenuItemDesc::Separator());
        pb.push_back(item(s.mini ? Tr(L"Exit mini player", L"미니 플레이어 종료") : Tr(L"Mini player", L"미니 플레이어"),
                          CommandId::ToggleMiniPlayer, s.mini ? G::ExitMiniPlayer : G::MiniPlayer, s.hasMedia));
        m.push_back(MenuItemDesc::Sub(Tr(L"Playback", L"재생"), std::move(pb), G::Play));
    }

    {
        std::vector<MenuItemDesc> subs = BuildSubtitleItems(s, run);
        if (subs.empty()) m.push_back(MenuItemDesc::Item(Tr(L"Subtitles", L"자막"), {}, {}, G::Subtitles).Disabled());
        else m.push_back(MenuItemDesc::Sub(Tr(L"Subtitles", L"자막"), std::move(subs), G::Subtitles));
    }
    {
        std::vector<MenuItemDesc> rot;
        const int degs[] = {0, 90, 180, 270};
        for (int d : degs)
            rot.push_back(MenuItemDesc::Radio(std::to_wstring(d) + L"°", (int)s.rotation == d, cmd(Cmd::SetRotation, d)));
        std::vector<MenuItemDesc> vs;
        vs.push_back(MenuItemDesc::Sub(Tr(L"Rotate", L"회전"), std::move(rot), G::Rotate));
        vs.push_back(MenuItemDesc::Check(Tr(L"Flip horizontally", L"좌우 반전"), s.flipH, [run](bool v) { if (run) run(MenuCmd{Cmd::FlipH, 0, 0, v}); }));
        vs.push_back(MenuItemDesc::Check(Tr(L"Flip vertically", L"상하 반전"), s.flipV, [run](bool v) { if (run) run(MenuCmd{Cmd::FlipV, 0, 0, v}); }));
        vs.push_back(MenuItemDesc::Check(Tr(L"Zoom to fill", L"화면 채우기"), s.zoom, [run](bool v) { if (run) run(MenuCmd{Cmd::Zoom, 0, 0, v}); },
                                         acc(CommandId::ZoomToFill)));
        vs.push_back(item(Tr(L"Enhancements…", L"영상 향상…"), CommandId::VideoEnhancements, G::VideoEnhancer));
        m.push_back(MenuItemDesc::Sub(Tr(L"Video", L"영상"), std::move(vs), G::VideoSettings));
    }
    if (s.aribAudio) {
        std::wstring n1, n2;
        fx::AribAudioNames(s.arib1, s.arib1Present, s.arib2, s.arib2Present, n1, n2);
        std::vector<MenuItemDesc> sub;
        sub.push_back(MenuItemDesc::Radio(n1, s.aribPref == 0, cmd(Cmd::SelectAribAudio, 0, 0)));
        sub.push_back(MenuItemDesc::Radio(n2, s.aribPref == 1, cmd(Cmd::SelectAribAudio, 0, 1)));
        m.push_back(MenuItemDesc::Sub(Tr(L"Audio track", L"소리"), std::move(sub), G::Volume));
    } else if (s.audio.empty()) {
        m.push_back(MenuItemDesc::Item(Tr(L"Audio track", L"소리"), {}, {}, G::Volume).Disabled());
    } else {
        std::vector<MenuItemDesc> sub;
        for (size_t i = 0; i < s.audio.size(); i++)
            sub.push_back(MenuItemDesc::Radio(fx::TrackName(s.audio[i], (int)i + 1), s.audio[i].Selected, cmd(Cmd::SelectAudio, 0, (int)i)));
        m.push_back(MenuItemDesc::Sub(Tr(L"Audio track", L"소리"), std::move(sub), G::Volume));
    }

    {
        const wchar_t* names[sl::kAlwaysOnTopCount] = {Tr(L"Off", L"사용 안 함"), Tr(L"Always on top", L"항상 위에"),
                                                       Tr(L"On top when playing", L"재생시 항상 위에"), Tr(L"On top when full screen", L"전체 화면시 항상 위에")};
        std::vector<MenuItemDesc> aot;
        for (int i = 0; i < sl::kAlwaysOnTopCount; i++)
            aot.push_back(MenuItemDesc::Radio(names[i], sl::AlwaysOnTopIndex(s.alwaysOnTop) == i, cmd(Cmd::SetAlwaysOnTop, 0, i)));
        aot.push_back(MenuItemDesc::Separator());
        aot.push_back(item(Tr(L"Cycle always on top", L"항상 위에 전환"), CommandId::CycleAlwaysOnTop));
        m.push_back(MenuItemDesc::Sub(Tr(L"Always on top", L"항상 위에"), std::move(aot), G::Pin));
    }
    m.push_back(MenuItemDesc::Separator());

    m.push_back(item(Tr(L"Equalizer…", L"이퀄라이저…"), CommandId::ShowEqualizer, G::Equalizer));
    m.push_back(item(Tr(L"Copy frame", L"프레임 복사"), CommandId::CaptureFrame, G::Copy, s.hasMedia));
    m.push_back(item(Tr(L"Properties…", L"재생 정보…"), CommandId::ShowProperties, G::Properties, s.hasMedia));
    m.push_back(MenuItemDesc::Separator());

    {
        MenuItemDesc panel = MenuItemDesc::Check(Tr(L"Playlist panel", L"재생목록 창"), s.bar.panelOpen, [run](bool) {
            if (run) run(MenuCmd{Cmd::Execute, 0, (int)CommandId::TogglePlaylistPanel});
        }, acc(CommandId::TogglePlaylistPanel));
        if (s.fullscreen || s.mini) panel.Disabled();
        m.push_back(std::move(panel));
    }
    m.push_back(MenuItemDesc::Check(Tr(L"Control panel", L"제어창"), s.bar.controlPanelOpen, [run](bool) {
        if (run) run(MenuCmd{Cmd::Execute, 0, (int)CommandId::ToggleControlPanel});
    }, acc(CommandId::ToggleControlPanel)));
    m.push_back(item(Tr(L"Settings", L"설정"), CommandId::OpenSettings, G::Settings));
    m.push_back(MenuItemDesc::Separator());
    m.push_back(item(Tr(L"Exit", L"종료"), CommandId::Exit));
    return m;
}

MenuState CaptureMenuState(AppContext& ctx) {
    MenuState s;
    s.fullscreen = ctx.IsFullscreen();
    s.mini = ctx.IsMiniPlayer();
    s.alwaysOnTop = ctx.Svc().settings.alwaysOnTop;
    s.bar.mini = s.mini;
    s.bar.panelOpen = ctx.Svc().settings.playlistPanel;
    s.bar.controlPanelOpen = IsFiltersPanelOpen(ctx);
    s.bar.canPlaySelection = ctx.CanPlaySelection();
    if (auto* c = ctx.Controller()) {
        s.hasMedia = c->Current() != nullptr;
        s.bar.shuffle = c->Shuffle();
        s.bar.repeat = (int)c->Repeat();
        s.bar.hasPrev = c->HasPrevious();
        s.bar.hasNext = c->HasNext();
    }
    if (auto* p = ctx.Player()) {
        s.bar.playing = p->State() == wvp::PlayerState::Playing;
        if (auto mi = p->GetMediaInfo()) {
            s.hasMedia = true;
            s.audio = mi->Of(wvp::StreamType::Audio);
            s.subs = mi->Of(wvp::StreamType::Subtitle);
            if (mi->AribAudio.Present) {
                s.aribAudio = true;
                s.aribPref = p->AudioPreference();
                for (const auto& a : s.audio) {
                    if (a.Index == mi->AribAudio.Audio1) { s.arib1 = a; s.arib1Present = true; }
                    if (a.Index == mi->AribAudio.Audio2) { s.arib2 = a; s.arib2Present = true; }
                }
            }
        }
    }
    const auto& f = ctx.Svc().filters.Settings();
    s.speed = f.speed;
    s.rotation = f.rotation;
    s.flipH = f.flipHorizontal;
    s.flipV = f.flipVertical;
    s.zoom = IsZoomToFill();
    return s;
}

}

namespace {
void Run(AppContext& ctx, const menus::MenuCmd& c) {
    using menus::Cmd;
    switch (c.cmd) {
    case Cmd::Execute: ctx.Execute(c.Command()); break;
    case Cmd::SetShuffle: if (auto* ctl = ctx.Controller()) ctl->SetShuffle(c.flag); break;
    case Cmd::SetRepeat:
        if (auto* ctl = ctx.Controller()) ctl->SetRepeat((wvp::RepeatMode)c.index);
        break;
    case Cmd::SetSpeed: SetPlaybackSpeed(ctx, c.value); break;
    case Cmd::SetRotation:
        ApplyFilterEdit(ctx, [&](wvp::FilterSettings& s) { s.rotation = (wvp::Rotation)(int)c.value; });
        break;
    case Cmd::FlipH: ApplyFilterEdit(ctx, [&](wvp::FilterSettings& s) { s.flipHorizontal = c.flag; }); break;
    case Cmd::FlipV: ApplyFilterEdit(ctx, [&](wvp::FilterSettings& s) { s.flipVertical = c.flag; }); break;
    case Cmd::Zoom: SetZoomToFill(ctx, c.flag); break;
    case Cmd::SetAlwaysOnTop:
        ctx.Svc().settings.alwaysOnTop = sl::AlwaysOnTopFromIndex(c.index);
        OnSettingsChanged(ctx);   // saves and re-applies HWND_TOPMOST
        break;
    case Cmd::SelectAudio: SelectTrack(ctx, wvp::StreamType::Audio, c.index); break;
    case Cmd::SelectAribAudio:
        if (auto* p = ctx.Player()) p->SetAudioPreference(c.index);   // no toast: audio 2 falls back to audio 1 by itself
        break;
    case Cmd::SelectSubtitle: SelectTrack(ctx, wvp::StreamType::Subtitle, c.index); break;
    }
}

std::vector<pui::MenuItemDesc> LiveMenu(AppContext& ctx) {
    // Actions run after the menu closed (and its popup is gone), on the UI thread.
    return menus::BuildMenu(menus::CaptureMenuState(ctx), [&ctx](const menus::MenuCmd& c) {
        ctx.PostToUi([&ctx, c] { Run(ctx, c); });
    });
}
}

void ShowMainMenu(AppContext& ctx, pui::PointF pt) {
    nativemenu::ShowAtPoint(LiveMenu(ctx), pt);
}

void ShowMainMenu(AppContext& ctx, pui::Widget* anchor, nativemenu::Callbacks cb) {
    if (!anchor) { nativemenu::ShowAtPoint(LiveMenu(ctx), {0, 0}, std::move(cb)); return; }
    nativemenu::ShowBelow(LiveMenu(ctx), anchor->Bounds(), false, std::move(cb));
}

void ShowSubtitleMenu(AppContext& ctx, pui::Widget* anchor) {
    std::vector<pui::MenuItemDesc> items = menus::BuildSubtitleItems(menus::CaptureMenuState(ctx), [&ctx](const menus::MenuCmd& c) {
        ctx.PostToUi([&ctx, c] { Run(ctx, c); });
    });
    if (items.empty()) return;
    if (anchor) nativemenu::ShowBelow(std::move(items), anchor->Bounds(), true);
    else nativemenu::ShowAtPoint(std::move(items), {ctx.Ui().Size().w - 24, ctx.Ui().Size().h - 120});
}

}
