// --headless-check: hidden window, real libffplay with the dummy audio driver, every composition layer drawn, mode/page
// transitions, resize and full-screen state changes on the hidden window, clean destruction. Writes a JSON result.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>

#include "App.h"
#include "Localize.h"
#include "features/DialogGuard.h"
#include "features/FiltersPanel.h"
#include "features/Menus.h"
#include "features/NativeMenu.h"
#include "features/OpenFlows.h"
#include "features/TextInputDialog.h"
#include "features/ListsCommon.h"
#include "pui/Menu.h"
#include "pui/Panels.h"
#include "wvp/DebugInfo.h"
#include "wvp/JsonLite.h"
#include "wvp/JsonWriter.h"
#include "features/Logging.h"
#include "features/MiniLogic.h"
#include "features/MiniBar.h"
#include "features/MiniPlayer.h"
#include "features/SettingsLogic.h"
#include "features/SettingsService.h"
#include "pui/VirtualList.h"
#include "shell/SettingsWindow.h"
#include <shellapi.h>
#include <shlobj.h>

namespace app {
namespace {
/// A small acrylic popup (stands in for "some popup is open over the video"; native menus are never shown by the checks).
void OpenProbePopup(pui::UiHost& host, pui::IRenderer* measurer, pui::PointF at) {
    auto f = std::make_unique<pui::Flyout>();
    f->SetPadding(12);
    f->SetContentNew<pui::TextBlock>(L"popup probe");
    pui::Flyout::ShowAt(host, std::move(f), pui::RectF{at.x, at.y, at.x, at.y}, pui::Placement::Bottom, measurer);
}

bool IsWindowAbove(HWND upper, HWND lower) {
    for (HWND w = GetWindow(lower, GW_HWNDPREV); w; w = GetWindow(w, GW_HWNDPREV))
        if (w == upper) return true;
    return false;
}

void ShowTestWindowInvisible(HWND hwnd) {
    if (!hwnd) return;
    LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex | WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW);
    SetLayeredWindowAttributes(hwnd, 0, 0, LWA_ALPHA);
    SetWindowPos(hwnd, HWND_TOP, -32000, -32000, 420, 240, SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

HWND CreateOwnedWindowStandIn(HWND owner, const wchar_t* title) {
    HWND h = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC", title, WS_POPUP,
                             -32000, -32000, 420, 240, owner, nullptr, GetModuleHandleW(nullptr), nullptr);
    ShowTestWindowInvisible(h);
    return h;
}
}

int App::RunHeadlessCheck() {
    SetUiLanguage(UiLanguage::English);   // the steps compare English texts, whatever the Windows UI language is
    OsDialogsBlocked() = true;            // the steps never show an OS dialog on the desktop
    // Synthetic messages must not inherit the physical Shift/Ctrl/Alt state from CurrentMods().
    // Record the physical state for diagnosis, then pin the modifiers to "none".
    Note("physical_mods_at_start", std::to_string(CurrentMods()));
    modsOverride_ = 0;
    bool created = Create(true);
    Step("windows_never_visible", (hwnd_ && !IsWindowVisible(hwnd_)) ? S_OK : E_FAIL);

    if (created) {
        const auto keepAot = svc_->settings.alwaysOnTop;
        svc_->settings.alwaysOnTop = wvp::AlwaysOnTop::Enable;
        ApplyWindowTopmost();

        TextPromptOptions promptOptions;
        promptOptions.title = L"Headless dialog z-order check";
        promptOptions.label = L"Invisible owned-dialog stand-in";
        HWND prompt = CreateTextDialogWindowHiddenForTest(promptOptions, CurrentTheme(), 96, hwnd_);
        ShowTestWindowInvisible(prompt);
        Step("text_prompt_owned_by_main", prompt && GetWindow(prompt, GW_OWNER) == hwnd_ ? S_OK : E_FAIL);
        SetWindowPos(hwnd_, prompt, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        Step("text_prompt_above_main_after_owner_zorder_request", prompt && IsWindowAbove(prompt, hwnd_) ? S_OK : E_FAIL);
        SendMessageW(hwnd_, WM_ACTIVATEAPP, FALSE, 0);
        Step("text_prompt_above_main_after_activateapp_reassert", prompt && IsWindowAbove(prompt, hwnd_) ? S_OK : E_FAIL);
        ApplyWindowTopmost();
        Step("text_prompt_above_main_after_apply_reassert", prompt && IsWindowAbove(prompt, hwnd_) ? S_OK : E_FAIL);

        HWND fileDialogStandIn = CreateOwnedWindowStandIn(hwnd_, L"Headless IFileOpenDialog stand-in");
        Step("file_dialog_standin_owned_by_main", fileDialogStandIn && GetWindow(fileDialogStandIn, GW_OWNER) == hwnd_ ? S_OK : E_FAIL);
        SetWindowPos(hwnd_, fileDialogStandIn, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        Step("file_dialog_standin_above_main", fileDialogStandIn && IsWindowAbove(fileDialogStandIn, hwnd_) ? S_OK : E_FAIL);
        if (fileDialogStandIn) DestroyWindow(fileDialogStandIn);
        if (prompt) DestroyTextDialogWindowHiddenForTest(prompt);

        svc_->settings.alwaysOnTop = keepAot;
        ApplyWindowTopmost();
    }

    if (created && gfxOk_) {
        ForceRender();
        Step("start_view_is_player", (Mode() == ShellMode::NowPlaying && Shell()->Title()->MainMenuButton()->Visible()) ? S_OK : E_FAIL);
        ForceRender();
        {
            const TitleLayout tl = Shell()->Title()->Layout();
            auto probe = [&](const pui::RectF& r) {
                const RectPx px = ToPx(r, scale_);
                POINT pt{(px.l + px.r) / 2, (px.t + px.b) / 2};
                ClientToScreen(hwnd_, &pt);
                return (int)SendMessageW(hwnd_, WM_NCHITTEST, 0, MAKELPARAM((short)pt.x, (short)pt.y));
            };
            Step("title_buttons_are_46x44_flush_right", (std::fabs(tl.close.r - layout_.widthDip) < 0.51f && tl.close.h() == kStripDip &&
                                                          tl.close.w() == 46.f && tl.max.r == tl.close.l && tl.min.r == tl.max.l) ? S_OK : E_FAIL);
            Step("hit_max_button_is_htmaxbutton", probe(tl.max) == HTMAXBUTTON ? S_OK : E_FAIL);
            Step("hit_other_title_buttons_are_client", (probe(tl.close) == HTCLIENT && probe(tl.min) == HTCLIENT && probe(tl.pin) == HTCLIENT &&
                                                         probe(tl.mainMenu) == HTCLIENT) ? S_OK : E_FAIL);
            Step("hit_title_drag_area_is_caption", probe({tl.name.l, 14.f, tl.name.r, 30.f}) == HTCAPTION ? S_OK : E_FAIL);
            POINT top{cw_ / 2, 0};
            ClientToScreen(hwnd_, &top);
            Step("hit_top_edge_resizes", (int)SendMessageW(hwnd_, WM_NCHITTEST, 0, MAKELPARAM((short)top.x, (short)top.y)) == HTTOP ? S_OK : E_FAIL);
            POINT body{cw_ / 2, S(kStripDip) + S(60)};
            ClientToScreen(hwnd_, &body);
            Step("hit_below_the_strip_is_client", (int)SendMessageW(hwnd_, WM_NCHITTEST, 0, MAKELPARAM((short)body.x, (short)body.y)) == HTCLIENT ? S_OK : E_FAIL);
            Step("title_surface_spans_the_full_width", (sTitle_.surface && sTitle_.w == cw_ && sTitle_.h == S(kStripDip)) ? S_OK : E_FAIL);
            Step("window_has_no_caption_style", ((GetWindowLongPtrW(hwnd_, GWL_STYLE) & WS_CAPTION) == 0 && (GetWindowLongPtrW(hwnd_, GWL_STYLE) & WS_THICKFRAME)) ? S_OK : E_FAIL);
        }
        {
            struct Src : pui::IListSource {
                int Count() const override { return 3; }
                std::wstring Text(int r, int) const override { return L"Row " + std::to_wstring(r); }
            } src;
            auto list = std::make_unique<pui::VirtualList>();
            list->SetColumns({{L"Title", 0, 1.f}});
            list->SetSource(&src);
            int invoked = -1;
            list->onInvoke = [&](int row) { invoked = row; };
            list->SetBounds(pui::RectF::XYWH(300, 200, 400, 240));
            pui::VirtualList* lp = list.get();
            pui::PopupOptions po; po.lightDismiss = false; po.takeFocus = false;
            host_.OpenPopup(std::move(list), po);
            constexpr float kRowH = 48.f;
            float rowCy = 200.f + 2 * kRowH + kRowH / 2;
            LPARAM at = MAKELPARAM((short)std::lround(500 * scale_), (short)std::lround(rowCy * scale_));
            Step("main_class_has_dblclks", (GetClassLongPtrW(hwnd_, GCL_STYLE) & CS_DBLCLKS) ? S_OK : E_FAIL);
            SendMessageW(hwnd_, WM_LBUTTONDOWN, MK_LBUTTON, at);
            SendMessageW(hwnd_, WM_LBUTTONUP, 0, at);
            bool singleSelectsOnly = invoked == -1 && lp->IsRowSelected(2);
            SendMessageW(hwnd_, WM_LBUTTONDBLCLK, MK_LBUTTON, at);
            SendMessageW(hwnd_, WM_LBUTTONUP, 0, at);
            Step("single_click_selects_not_invokes", singleSelectsOnly ? S_OK : E_FAIL);
            Step("double_click_invokes_row", invoked == 2 ? S_OK : E_FAIL);
            host_.CloseAllPopups();
            ForceRender();
        }
        ShowToast(L"Copied");
        ShowInfoBar(InfoSeverity::Warning, L"Headless check", L"InfoBar draw test");
        ForceRender();
        Step("popup_layer_notices", lastDrawHr_);
        host_.CloseAllPopups();
        ForceRender();
    }

    // player: FfPlayer::Create with a presenter on the video child, the topmost composition target already present
    Step("player_created_with_presenter", (player_ && player_->CurrentPresenter()) ? S_OK : E_FAIL);
    if (player_ && !opt_.inputs.empty()) {
        OpenPaths(*this, {opt_.inputs[0]});
        bool prep = PumpUntil([&] { return model_.prepared && state_ == wvp::PlayerState::Playing; }, 20000);
        Step("open_prepared_and_playing", prep ? S_OK : E_FAIL);
        Note("startup_probe_ms", std::to_string(startupProbeMs_));
        Note("startup_first_prepared_ms", std::to_string(firstPreparedMs_));
        // Steps that need running, seekable media call this: the suite runs on short clips (~3-6 s, which end while earlier
        // steps wait) and long ones (the 90 s broadcast fixture), and a drop / reopen may still be preparing. An ended or
        // paused item is restarted; one still opening is only waited for (PlayPause would toggle the pending open).
        auto ensurePlaying = [&](const char* note) {
            using wvp::PlayerState;
            const bool idle = state_ == PlayerState::Ended || state_ == PlayerState::Stopped || state_ == PlayerState::Idle ||
                              (model_.prepared && state_ == PlayerState::Paused);
            if (idle) {
                Note(note, "1");
                Execute(CommandId::PlayPause);
            }
            return PumpUntil([&] { return model_.prepared && model_.Seekable() && state_ == PlayerState::Playing; }, 20000);
        };
        auto checkInAppModal = [&](const char* stepName) {
            lists::ShowConfirmDialog(host_, L"Headless modal", L"Invisible composition smoke test", L"Confirm", [] {});
            ForceRender();
            UpdateAcrylicState(true);
            ForceRender();
            auto* dlg = dynamic_cast<lists::ConfirmDialog*>(host_.TopPopup());
            const pui::RectF bounds = dlg ? dlg->Bounds() : pui::RectF{};
            const pui::SizeF size = host_.Size();
            const bool coversVideo = dlg && bounds.l <= layout_.video.l / scale_ && bounds.t <= layout_.video.t / scale_ &&
                                     bounds.r >= layout_.video.r / scale_ && bounds.b >= layout_.video.b / scale_;
            const bool aboveChrome = LayerStackMatches() && LayerIndex(Layer::Popup) > LayerIndex(Layer::Content) &&
                                     LayerIndex(Layer::Popup) > LayerIndex(Layer::Title) && LayerIndex(Layer::Popup) > LayerIndex(Layer::Bar);
            const bool acrylicOff = host_.HasModalPopup() && acrylicFallbackReason_ == acrylic::FallbackReason::ModalDialog &&
                                    !host_.GetTheme().acrylicEnabled && acrylicAppliedRegions_.empty();
            const bool rendered = sPopup_.surface && sPopup_.w == cw_ && sPopup_.h == ch_ && SUCCEEDED(lastDrawHr_);
            const bool focused = dlg && host_.Focused() == dlg->CancelButton();
            Step(stepName, (dlg && host_.HasModalPopup() && focused && coversVideo && bounds.l == 0 && bounds.t == 0 &&
                            bounds.r == size.w && bounds.b == size.h && aboveChrome && acrylicOff && rendered) ? S_OK : E_FAIL);
            host_.CloseAllPopups();
            UpdateAcrylicState(true);
            ForceRender();
        };
        {
            const auto savedAot = svc_->settings.alwaysOnTop;
            HWND owned = CreateOwnedWindowStandIn(hwnd_, L"Always-on-top setting stand-in");
            auto setFullscreen = [&](bool fullscreen) {   // mode flag only (what ApplyWindowTopmost reads): the window itself is untouched
                modes_.fullscreen = fullscreen;
            };
            auto testSetting = [&](const char* afterOrder, const char* afterDeactivate, const char* afterApply,
                                   wvp::AlwaysOnTop mode, bool fullscreen) {
                setFullscreen(fullscreen);
                svc_->settings.alwaysOnTop = mode;
                ApplyWindowTopmost();
                SetWindowPos(hwnd_, owned, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                Step(afterOrder, owned && GetWindow(owned, GW_OWNER) == hwnd_ && IsWindowAbove(owned, hwnd_) ? S_OK : E_FAIL);
                SendMessageW(hwnd_, WM_ACTIVATEAPP, FALSE, 0);
                Step(afterDeactivate, owned && IsWindowAbove(owned, hwnd_) ? S_OK : E_FAIL);
                ApplyWindowTopmost();
                const bool expectedTopmost = sl::ShouldBeTopmost(mode, state_ == wvp::PlayerState::Playing, modes_.fullscreen);
                const bool actualTopmost = (GetWindowLongPtrW(hwnd_, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
                Step(afterApply, owned && IsWindowAbove(owned, hwnd_) && actualTopmost == expectedTopmost && !IsWindowVisible(hwnd_)
                     ? S_OK : E_FAIL);
            };
            testSetting("dialog_aot_enable_after_owner_order", "dialog_aot_enable_after_activateapp", "dialog_aot_enable_after_apply",
                        wvp::AlwaysOnTop::Enable, false);
            testSetting("dialog_aot_playing_after_owner_order", "dialog_aot_playing_after_activateapp", "dialog_aot_playing_after_apply",
                        wvp::AlwaysOnTop::WhenPlaying, false);
            Step("playing_for_whenplaying_aot_dialog_test", state_ == wvp::PlayerState::Playing ? S_OK : E_FAIL);
            testSetting("dialog_aot_fullscreen_after_owner_order", "dialog_aot_fullscreen_after_activateapp", "dialog_aot_fullscreen_after_apply",
                        wvp::AlwaysOnTop::WhenFullscreen, true);
            testSetting("dialog_aot_windowed_after_owner_order", "dialog_aot_windowed_after_activateapp", "dialog_aot_windowed_after_apply",
                        wvp::AlwaysOnTop::WhenFullscreen, false);
            testSetting("dialog_aot_disable_after_owner_order", "dialog_aot_disable_after_activateapp", "dialog_aot_disable_after_apply",
                        wvp::AlwaysOnTop::Disable, false);
            if (owned) DestroyWindow(owned);
            setFullscreen(false);
            svc_->settings.alwaysOnTop = savedAot;
            ApplyWindowTopmost();
            Relayout();
        }
        checkInAppModal("now_playing_modal_above_video_osd");
        {
            RECT restored{};
            GetWindowRect(hwnd_, &restored);
            LONG_PTR style = GetWindowLongPtrW(hwnd_, GWL_STYLE);
            MONITORINFO mi{sizeof(mi)};
            GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &mi);
            SetWindowLongPtrW(hwnd_, GWL_STYLE, style | WS_MAXIMIZE);
            SetWindowPos(hwnd_, nullptr, mi.rcWork.left, mi.rcWork.top, mi.rcWork.right - mi.rcWork.left,
                         mi.rcWork.bottom - mi.rcWork.top, SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOACTIVATE);
            SendMessageW(hwnd_, WM_SIZE, SIZE_MAXIMIZED, 0);
            Step("headless_maximized_state", IsZoomed(hwnd_) ? S_OK : E_FAIL);
            checkInAppModal("maximized_modal_above_video_osd");
            SetWindowLongPtrW(hwnd_, GWL_STYLE, style);
            SetWindowPos(hwnd_, nullptr, restored.left, restored.top, restored.right - restored.left,
                         restored.bottom - restored.top, SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOACTIVATE);
            SendMessageW(hwnd_, WM_SIZE, SIZE_RESTORED, 0);
            Relayout();
            ApplyWindowTopmost();
        }
        bool pos = PumpUntil([&] { return positionEvents_ >= 3; }, 12000);
        Step("position_events_ge_3", pos ? S_OK : E_FAIL);
        Note("duration_s", std::to_string(model_.dur));
        Note("position_events", std::to_string(positionEvents_));
        double aribCuePos = -1;   // where a caption cue was seen (the paused-reopen steps seek back there)
        {   // ARIB fixtures must deliver a live cue and compose it into the real snapshot, not just expose a subtitle stream.
            bool expectsAribCaption = false;
            if (auto media = player_->GetMediaInfo()) {
                for (const auto& stream : media->Streams) {
                    if (stream.Type == wvp::StreamType::Subtitle && stream.Codec == "arib_caption" && stream.AribType != "superimpose") {
                        expectsAribCaption = true;
                        break;
                    }
                }
            }
            if (expectsAribCaption) {
                Note("arib_caption_expected", "true");
                wvp::DebugInfo cueInfo{};
                uint64_t nextPollMs = 0;
                auto hasActiveCue = [&] {
                    const auto& layer = cueInfo.CaptionLayer;
                    return cueInfo.Valid && layer.Present && layer.RectCount.value_or(0) > 0 && layer.CuesShown.value_or(0) > 0;
                };
                bool cueObserved = PumpUntil([&] {
                    uint64_t now = Now();
                    if (now >= nextPollMs) {
                        nextPollMs = now + 100;
                        if (auto text = player_->GetDebugInfoJson()) cueInfo = wvp::ParseDebugInfo(*text);
                    }
                    return hasActiveCue();
                }, 30000);
                if (cueObserved) aribCuePos = player_->Position();
                Note("arib_caption_cue_observed", cueObserved ? "true" : "false");
                bool paused = false, cueActiveWhilePaused = false, snapshotDiff = false, resumed = false;
                if (cueObserved) {
                    int pauseRc = player_->Pause();
                    paused = pauseRc == 0 && PumpUntil([&] { return state_ == wvp::PlayerState::Paused; }, 5000);
                    if (paused) {
                        if (auto text = player_->GetDebugInfoJson()) cueInfo = wvp::ParseDebugInfo(*text);
                        cueActiveWhilePaused = hasActiveCue();
                        if (cueActiveWhilePaused) {
                            auto videoOnly = player_->Snapshot(false);
                            auto withCaption = player_->Snapshot(true);
                            bool sameShape = videoOnly && withCaption && videoOnly->Width == withCaption->Width &&
                                             videoOnly->Height == withCaption->Height && videoOnly->Stride == withCaption->Stride &&
                                             videoOnly->Data.size() == withCaption->Data.size() && !videoOnly->Data.empty();
                            snapshotDiff = sameShape && std::mismatch(videoOnly->Data.begin(), videoOnly->Data.end(),
                                                                     withCaption->Data.begin()).first != videoOnly->Data.end();
                        }
                    }
                    if (paused) {
                        int playRc = player_->Play();
                        resumed = playRc == 0 && PumpUntil([&] { return state_ == wvp::PlayerState::Playing; }, 5000);
                    }
                }
                Note("arib_caption_rects", std::to_string(cueInfo.CaptionLayer.RectCount.value_or(0)));
                Note("arib_caption_cues_shown", std::to_string(cueInfo.CaptionLayer.CuesShown.value_or(0)));
                Note("arib_caption_snapshot_diff", snapshotDiff ? "true" : "false");
                Note("arib_caption_pause_resume", (paused && resumed) ? "true" : "false");
                Step("arib_caption_cue_drawn", (cueObserved && paused && cueActiveWhilePaused && snapshotDiff && resumed) ? S_OK : E_FAIL);
            } else {
                Note("arib_caption_expected", "false");
            }
        }
        {
            // the first open waited for the plan; the planner's content reopen right after it gives an empty sample, so wait for
            // one from a decoding player
            wvp::DebugInfo di;
            PumpUntil([&] {
                auto js = player_->GetDebugInfoJson();
                di = js ? wvp::ParseDebugInfo(*js) : wvp::DebugInfo{};
                return di.Valid && model_.prepared && !di.Video.HwaccelRequested.empty() && !di.Video.FramePath.empty();
            }, 8000);
            const wvp::Plan* plan = svc_->filters.CurrentPlan();
            bool reqOk = di.Valid && plan && (plan->hwaccel.empty() || di.Video.HwaccelRequested == plan->hwaccel);
            Step("hwaccel_requested_on_first_open", reqOk ? S_OK : E_FAIL);
            bool hw = di.Video.HwaccelDevice != "none" && !di.Video.HwaccelDevice.empty() &&
                      (di.Video.FramePath == "hw_download" || di.Video.FramePath == "hw_direct");
            bool explained = di.Video.HwaccelDevice == "none" && !di.Video.HwaccelError.empty();
            Step("hwaccel_decoding_actually_hardware_or_explained", (di.Valid && (hw || explained)) ? S_OK : E_FAIL);
            Note("hwaccel_device", di.Video.HwaccelDevice);
            Note("hwaccel_frame_path", di.Video.FramePath);
        }
        {   // Play / pause from every entry point: pause, then resume, through the real input path. Per transition: input ->
            // libffplay API call (app), API -> loop thread apply, input -> state event (bar icon), last / first presented frame
            // and the decoded audio handed to the device (libffplay "pause_timing"; its clock is aligned to ours
            // through its "now_us"). Load-tolerant ceilings; the precise gate is PlayerCore.Tests PauseLatency_*.
            ensurePlaying("playback_restarted_before_pause_entry_points");
            auto qpcUs = [] {
                static const double f = [] { LARGE_INTEGER q; QueryPerformanceFrequency(&q); return (double)q.QuadPart; }();
                LARGE_INTEGER c; QueryPerformanceCounter(&c);
                return c.QuadPart * 1e6 / f;
            };
            auto clickAt = [&](pui::Widget* w) {
                StartBarAnim(autoHide_.Enter(Now()));
                StartTitleAnim(titleHide_.Enter(Now()));
                SnapBar();
                ForceRender();
                const pui::RectF b = w->Bounds();
                LPARAM at = MAKELPARAM((short)std::lround((b.l + b.r) * 0.5f * scale_), (short)std::lround((b.t + b.b) * 0.5f * scale_));
                SendMessageW(hwnd_, WM_LBUTTONDOWN, MK_LBUTTON, at);
                SendMessageW(hwnd_, WM_LBUTTONUP, 0, at);
            };
            auto rewindShortClip = [&] {   // a short clip (3 s) would end during the measurements: back to its start
                ensurePlaying("playback_restarted_during_pause_entry_points");
                if (player_ && model_.Seekable() && model_.dur - player_->Position() < 2.5) {
                    player_->Seek(0.0);
                    PumpUntil([&] { double p = player_->Position(); return std::isfinite(p) && p < 1.0; }, 3000);
                    PumpUntil([] { return false; }, 200);
                }
            };
            {   // a key press waits for whatever the UI thread is dispatching: the longest single dispatch while playing
                double maxMs = 0, sumMs = 0;
                int count = 0;
                UINT maxMsg = 0;
                rewindShortClip();   // steady playback only (not a short clip's end and restart)
                // a restart's or the planner's content reopen (just after the first open) finishes first: wait for running
                // playback with fresh position events (an open's own UI work is not what a key press during playback waits for)
                {
                    const int events0 = positionEvents_;
                    PumpUntil([&] { return state_ == wvp::PlayerState::Playing && model_.prepared && positionEvents_ >= events0 + 3; }, 5000);
                    rewindShortClip();
                }
                const double left = player_ ? model_.dur - player_->Position() - 0.5 : NAN;   // NaN right after a seek
                const double window = std::isfinite(left) ? std::clamp(left, 1.0, 4.0) : 2.0;
                const double end = qpcUs() + window * 1e6;
                while (qpcUs() < end) {
                    MSG m;
                    while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
                        const double t = qpcUs();
                        TranslateMessage(&m); DispatchMessageW(&m);
                        const double d = (qpcUs() - t) / 1000.0;
                        sumMs += d; ++count;
                        if (d > maxMs) { maxMs = d; maxMsg = m.message; }
                    }
                    MsgWaitForMultipleObjects(0, nullptr, FALSE, 20, QS_ALLINPUT);
                }
                char b[128];
                std::snprintf(b, sizeof b, "max %.1f ms (message 0x%04X), %d dispatches, mean %.2f ms", maxMs, maxMsg, count,
                              count ? sumMs / count : 0.0);
                Note("ui_thread_dispatch_while_playing", b);
                wchar_t tmpDir[MAX_PATH] = {};
                GetTempPathW(MAX_PATH, tmpDir);
                const std::wstring tmp = std::wstring(tmpDir) + L"aribplayer-history-write-probe.json";
                const std::string tmpUtf8 = wvp::ToUtf8(tmp);
                const std::string payload(16 * 1024, 'x');
                for (bool durable : {true, false}) {
                    double mx = 0, sum = 0;
                    for (int i = 0; i < 10; ++i) {
                        const double t = qpcUs();
                        wvp::platform::AtomicWriteFile(tmpUtf8, payload, durable);
                        const double d = (qpcUs() - t) / 1000.0;
                        mx = std::max(mx, d); sum += d;
                    }
                    std::snprintf(b, sizeof b, "16 KB x10: mean %.2f ms, max %.2f ms", sum / 10, mx);
                    Note(durable ? "history_write_durable" : "history_write_non_durable", b);
                }
                DeleteFileW(tmp.c_str());
                Step("ui_thread_responsive_while_playing", count > 0 && maxMs <= 50.0 ? S_OK : E_FAIL);
            }
            auto playGlyph = [&]() -> std::wstring {
                if (modes_.mini) return std::wstring(1, Shell()->Mini()->PlayButton()->Glyph());
                auto* g = dynamic_cast<GlyphButton*>(Shell()->Bar()->ButtonWidget(B_Play));
                return g ? g->Glyph() : std::wstring();
            };
            auto measure = [&](const char* step, const std::function<void()>& fire) {
                bool ok = true;
                rewindShortClip();
                for (int want : {1, 0}) {
                    PumpUntil([] { return false; }, 400);
                    if (want && state_ != wvp::PlayerState::Playing)
                        ensurePlaying((std::string(step) + "_restarted_before_pause").c_str());
                    const std::wstring glyphBefore = playGlyph();
                    const double t0 = qpcUs();
                    fire();
                    const auto target = want ? wvp::PlayerState::Paused : wvp::PlayerState::Playing;
                    // wait like the app's GetMessage loop (woken by the posted event), not PumpUntil's Sleep(5) (~15.6 ms ticks)
                    bool reached = false;
                    while (!(reached = state_ == target) && qpcUs() - t0 < 2e6) {
                        MSG m;
                        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
                        if (state_ == target) { reached = true; break; }
                        MsgWaitForMultipleObjects(0, nullptr, FALSE, 50, QS_ALLINPUT);
                    }
                    const double tState = qpcUs();
                    const bool iconChanged = playGlyph() != glyphBefore && model_.playing == !want;
                    PumpUntil([] { return false; }, 300);
                    double nowQpc = qpcUs();
                    auto js = player_->GetDebugInfoJson();
                    auto v = js ? wvp::json::Parse(*js) : std::nullopt;
                    const wvp::json::Value* pt = v ? v->Find("pause_timing") : nullptr;
                    char buf[512] = "no pause_timing";
                    bool stagesOk = false;
                    if (pt && pt->Int("want") == want) {
                        const double off = pt->Dbl("now_us") - nowQpc;   // libffplay clock - our clock
                        const double api = pt->Dbl("api_us") - off, apply = pt->Dbl("apply_us") - off;
                        const double dev = pt->Dbl("audio_device_ms");
                        const double appMs = (api - t0) / 1000.0, applyMs = (apply - api) / 1000.0, stateMs = (tState - t0) / 1000.0;
                        double videoMs, audioMs;
                        if (want) {
                            videoMs = pt->Int("frames_after") > 0 ? (pt->Dbl("last_frame_us") - off - t0) / 1000.0 : appMs + applyMs;
                            audioMs = std::max(0.0, (pt->Dbl("audio_last_end_us") - off - t0) / 1000.0) + dev;
                        } else {
                            videoMs = pt->Dbl("first_frame_us") > 0 ? (pt->Dbl("first_frame_us") - off - t0) / 1000.0 : NAN;
                            audioMs = pt->Dbl("audio_first_us") > 0 ? (pt->Dbl("audio_first_us") - off - t0) / 1000.0 + dev : NAN;
                        }
                        const double emitMs = pt->Dbl("state_us") > 0 ? (pt->Dbl("state_us") - off - t0) / 1000.0 : NAN;
                        std::snprintf(buf, sizeof buf, "input->api %.1f ms, api->apply %.1f ms, state emitted %.1f ms, input->state+icon "
                                      "%.1f ms, video %.1f ms (frames after %d), audio %.1f ms (device %.1f ms)", appMs, applyMs, emitMs,
                                      stateMs, videoMs, pt->Int("frames_after"), audioMs, dev);
                        // input->api is synchronous; the state event needs the loop thread and a posted message (Sleep(5) pumping)
                        // audio: hand-over (without the in-flight estimate) by the next device callback (half of the
                        // estimate: ~10 ms on a real device, ~43 ms with the dummy driver of the headless runs). The slack is 40 ms:
                        // the dummy driver's SDL_Delay and our own sampling are both quantised to the 15.6 ms Windows timer tick (a
                        // hidden window may lose the 1 ms timer request), so 2-3 ticks of jitter above the period are noise.
                        const double handoffMs = audioMs - dev, period = dev / 2.0;
                        stagesOk = appMs >= -1.0 && appMs <= 20.0 && applyMs <= 20.0 && stateMs <= 150.0 &&
                                   std::isfinite(videoMs) && videoMs <= (want ? 50.0 : 120.0) &&
                                   (!std::isfinite(audioMs) || handoffMs <= period + 40.0);
                    }
                    {
                        char h[160];
                        std::snprintf(h, sizeof h, " | handler entered %.1f ms, topmost %.1f, bar sync+render %.1f, history save %.1f ms",
                                      (stateTiming_.enter - t0) / 1000.0, (stateTiming_.topmost - stateTiming_.enter) / 1000.0,
                                      (stateTiming_.synced - stateTiming_.topmost) / 1000.0, (stateTiming_.saved - stateTiming_.synced) / 1000.0);
                        std::strncat(buf, h, sizeof buf - std::strlen(buf) - 1);
                    }
                    Note((std::string(step) + (want ? "_pause" : "_resume")).c_str(), buf);
                    ok = ok && reached && iconChanged && stagesOk;
                }
                Step(step, ok ? S_OK : E_FAIL);
            };
            measure("play_pause_bar_button_click", [&] { clickAt(Shell()->Bar()->ButtonWidget(B_Play)); });
            measure("play_pause_video_double_click", [&] {
                const LPARAM at = MAKELPARAM(layout_.video.w() / 2, layout_.video.h() / 2);
                SendMessageW(video_, WM_LBUTTONDBLCLK, MK_LBUTTON, at);   // the second press of a double click on the video child
                SendMessageW(video_, WM_LBUTTONUP, 0, at);
            });
            measure("play_pause_space_key", [&] { SendMessageW(hwnd_, WM_KEYDOWN, VK_SPACE, 0); SendMessageW(hwnd_, WM_KEYUP, VK_SPACE, 0); });
            SetMiniPlayer(true);
            ForceRender();
            if (modes_.mini) {
                measure("play_pause_mini_double_click", [&] {
                    RECT wr{}; GetWindowRect(hwnd_, &wr);
                    SendMessageW(hwnd_, WM_NCLBUTTONDBLCLK, HTCAPTION, MAKELPARAM((short)((wr.left + wr.right) / 2), (short)((wr.top + wr.bottom) / 2)));
                });
                measure("play_pause_mini_bar_button_click", [&] { clickAt(Shell()->Mini()->PlayButton()); });
            } else {
                Step("play_pause_mini_player_entered", E_FAIL);
            }
            SetMiniPlayer(false);
            ForceRender();
            ensurePlaying("playback_restarted_after_pause_entry_points");
            if (player_ && model_.Seekable() && model_.dur < 10.0) {   // the following steps expect a short clip near its start
                player_->Seek(0.0);
                PumpUntil([&] { double p = player_->Position(); return std::isfinite(p) && p < 1.0; }, 3000);
            }
        }
        {
            int filterErrors = 0;
            auto tok = player_->FilterError.add([&](const wvp::FilterErrorInfo&) { filterErrors++; });
            wvp::FilterSettings fs = svc_->filters.Settings();
            fs.speed = 1.25;
            SendFilterChange(*this, svc_->filters.Apply(fs));
            fs.speed = 1.5;
            SendFilterChange(*this, svc_->filters.Apply(fs));
            int before = positionEvents_;
            bool still = PumpUntil([&] { return positionEvents_ >= before + 2; }, 6000);
            Step("live_filter_change_no_error", (still && filterErrors == 0 && state_ == wvp::PlayerState::Playing) ? S_OK : E_FAIL);
            Note("filter_errors", std::to_string(filterErrors));
            // overlay / log follow the live graph, not just the open-time options
            bool liveVf = false;
            for (const auto& [k, v] : effOpts_) liveVf = liveVf || (k == "vf" && v.find("setpts=PTS/1.5") != std::string::npos);
            Step("live_filter_change_updates_effective_options", liveVf ? S_OK : E_FAIL);
            SendFilterChange(*this, svc_->filters.Reset());
            player_->FilterError.remove(tok);
        }
        Step("opening_media_switches_to_now_playing", (Mode() == ShellMode::NowPlaying && VideoVisible()) ? S_OK : E_FAIL);
        if (ctl_ && ctl_->Current()) {   // a drop on a playlist view (here: the current playlist) only appends
            const std::string cur = ctl_->Current()->url;
            const int idx = ctl_->CurrentIndex();
            const size_t before = ctl_->Items().size();
            bool done = false;
            DropPathsToPlaylist(*this, {opt_.inputs[0]}, CurrentPlaylistName(), [&done] { done = true; });
            PumpUntil([&] { return done; }, 10000);
            Step("playlist_view_drop_appends_without_changing_playback",
                 done && ctl_->Items().size() == before + 1 && ctl_->CurrentIndex() == idx && ctl_->Current() &&
                 ctl_->Current()->url == cur ? S_OK : E_FAIL);
        }
        {   // OLE uses the shared route at DragOver and Drop on the main window and the video child.
            Step("video_drop_targets_ready",
                 (DropTargetsReady() && !(GetWindowLongPtrW(VideoHwnd(), GWL_EXSTYLE) & WS_EX_ACCEPTFILES)) ? S_OK : E_FAIL);
            std::wstring first = opt_.inputs[0], dropPath = first;
            {   // A different file is required: dropping the playing file again does not replace the queue. Paths are compared
                // as normalized full paths (the input may use '/' and be relative). Deterministic choice: the known small
                // fixtures first, in the input's folder and then its parent (testmedia\arib\tbs-captions.ts has no sibling;
                // testmedia\ has the short clips), then the first other mp4/ts/mkv by name.
                auto fullPath = [](const std::wstring& p) {
                    wchar_t buf[MAX_PATH * 4];
                    DWORD n = GetFullPathNameW(p.c_str(), (DWORD)std::size(buf), buf, nullptr);
                    return n && n < std::size(buf) ? std::wstring(buf, n) : p;
                };
                auto isFile = [](const std::wstring& p) {
                    DWORD a = GetFileAttributesW(p.c_str());
                    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
                };
                const std::wstring firstFull = fullPath(first);
                size_t slash = firstFull.find_last_of(L'\\');
                std::vector<std::wstring> dirs{firstFull.substr(0, slash)};
                if (size_t p = dirs[0].find_last_of(L'\\'); p != std::wstring::npos && p + 1 < dirs[0].size()) dirs.push_back(dirs[0].substr(0, p));
                auto usable = [&](const std::wstring& p) { return isFile(p) && _wcsicmp(p.c_str(), firstFull.c_str()) != 0; };
                std::wstring found;
                for (const std::wstring& d : dirs) {
                    for (const wchar_t* name : {L"hevc-720p.mp4", L"testsrc2-360p-short.mp4", L"mpeg4-240p-short.mp4"}) {
                        if (usable(d + L"\\" + name)) { found = d + L"\\" + name; break; }
                    }
                    if (!found.empty()) break;
                }
                for (size_t i = 0; found.empty() && i < dirs.size(); i++) {
                    std::vector<std::wstring> names;
                    for (const wchar_t* pattern : {L"\\*.mp4", L"\\*.ts", L"\\*.mkv"}) {
                        WIN32_FIND_DATAW fd{};
                        HANDLE fh = FindFirstFileW((dirs[i] + pattern).c_str(), &fd);
                        if (fh == INVALID_HANDLE_VALUE) continue;
                        do { names.push_back(fd.cFileName); } while (FindNextFileW(fh, &fd));
                        FindClose(fh);
                    }
                    std::sort(names.begin(), names.end(), [](const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
                    for (const std::wstring& n : names) {
                        if (usable(dirs[i] + L"\\" + n)) { found = dirs[i] + L"\\" + n; break; }
                    }
                }
                if (!found.empty()) dropPath = found;
                else {
                    std::wstring want = dirs.back() + L"\\hevc-720p.mp4";
                    std::string w8; for (wchar_t ch : want) w8.push_back(ch < 128 ? (char)ch : (char)63);
                    Note("video_drop_missing_fixture", "no other mp4/ts/mkv next to the input or in its parent; expected e.g. " + w8);
                }
                Step("video_drop_has_a_different_file", !found.empty() ? S_OK : E_FAIL);
            }
            if (dropPath == first) {   // never drop the playing file again: report the dependent steps as failed instead
                for (const char* step : {"video_drop_forwarded_to_main", "video_drop_opens_dropped_file", "video_drop_replaces_unnamed_and_plays"})
                    Step(step, E_FAIL);
            } else {
                POINT videoPt{(layout_.video.l + layout_.video.r) / 2, (layout_.video.t + layout_.video.b) / 2};
                int before = DropEvents();
                bool routed = HandleDropAtClient(videoPt, {dropPath});
                Step("video_drop_forwarded_to_main", routed && DropEvents() == before + 1 ? S_OK : E_FAIL);
                std::wstring leaf = dropPath.substr(dropPath.find_last_of(L"\\/") + 1);
                bool opened = PumpUntil([&] {
                    auto* cur = ctl_ ? ctl_->Current() : nullptr;
                    if (!cur) return false;
                    const std::wstring url = wvp::ToWide(cur->url);
                    return url.size() >= leaf.size() && _wcsicmp(url.c_str() + url.size() - leaf.size(), leaf.c_str()) == 0;
                }, 6000);
                Step("video_drop_opens_dropped_file", opened ? S_OK : E_FAIL);
                Step("video_drop_replaces_unnamed_and_plays", opened && currentPlaylistName_.empty() && ctl_ && ctl_->Items().size() == 1 &&
                     ctl_->CurrentIndex() == 0 ? S_OK : E_FAIL);
                { std::string n8; for (wchar_t ch : leaf) n8.push_back(ch < 128 ? (char)ch : (char)63); Note("dropped", n8); }
                if (opened) aribCuePos = -1;   // the dropped clip replaced the fixture: its cue is gone (and the clip has a single keyframe)
                PumpUntil([&] { return model_.prepared && state_ == wvp::PlayerState::Playing; }, 20000);
            }

            ensurePlaying("playback_restarted_after_drops");   // the video drop's reopen may still be preparing
        }
        ForceRender();
        Step("now_playing_render_layers", lastDrawHr_);
        Step("now_playing_popup_above_bar", CheckPopupOverBar());
        {   // simulated UI GPU device loss while playing (Gfx injects DXGI_ERROR_DEVICE_REMOVED at the next draw/commit)
            const ShellMode lossMode = Mode();
            const bool lossDebug = DebugOverlayVisible();
            const int lossRebuilds = RebindCount();
            const size_t lossNotices = notices_.size();
            const bool warpBefore = Warp();
            auto closeLossNotices = [&] { for (int i = 0; i < 8 && notices_.size() > lossNotices; i++) CloseNotice(notices_.back().w); };
            auto uiIntact = [&] {
                return GfxOk() && LayerStackMatches() && sBar_.surface && sBar_.w == layout_.barPx.w() && SUCCEEDED(gfx_.Commit()) &&
                       Mode() == lossMode && DebugOverlayVisible() == lossDebug &&
                       (!lossDebug || DebugOverlayDrawn());
            };
            auto lossQpcUs = [] {
                static const double f = [] { LARGE_INTEGER q; QueryPerformanceFrequency(&q); return (double)q.QuadPart; }();
                LARGE_INTEGER c; QueryPerformanceCounter(&c);
                return c.QuadPart * 1e6 / f;
            };
            // Playback restore: the item is reopened on a fresh video device at the same position and play state. Done once the
            // reopen (a new pipeline generation) has shown a frame; returns the QPC time of that observation.
            auto presentedFrames = [&]() -> long long {
                auto js = player_->GetDebugInfoJson();
                if (!js) return 0;
                wvp::DebugInfo d = wvp::ParseDebugInfo(*js);
                return d.Presenter.FramesDirect.value_or(0) + d.Presenter.FramesUploaded.value_or(0);
            };
            auto waitRestored = [&](int gen, wvp::PlayerState want, double& firstFrameUs, bool needFrame = true) {
                firstFrameUs = 0;
                return PumpUntil([&] {
                    const double pos = player_->Position();
                    if (pipe_.Generation() == gen || state_ != want || (needFrame && presentedFrames() <= 0) || !(pos == pos)) return false;
                    firstFrameUs = lossQpcUs();
                    return true;
                }, 5000);
            };
            PumpUntil([&] { const double p = player_->Position(); return state_ == wvp::PlayerState::Playing && p == p && p > 0; }, 3000);
            PumpUntil([] { return false; }, 300);   // settled after the previous step's reopen
            Execute(CommandId::OpenSettings);   // the Settings window shares the UI device: its surfaces go through the rebind too
            const int settingsRendersBefore = Settings() ? Settings()->Renders() : 0;
            const double lossPos = player_->Position();
            const int lossGen = pipe_.Generation();
            SimulateUiDeviceLoss();
            const bool rebuilt = PumpUntil([&] { return RebindCount() > lossRebuilds && !RebindBusy(); }, 3000);
            const RebindTiming tm = LastRebindTiming();
            Step("ui_loss_rebuilt_after_simulated_device_removed", (rebuilt && tm.reason == "test") ? S_OK : E_FAIL);
            Step("ui_loss_layers_recreated_and_commit_ok", uiIntact() ? S_OK : E_FAIL);
            {
                SettingsWindow* sw = Settings();
                Step("ui_loss_settings_window_surfaces_recreated", sw && sw->IsOpen() && sw->DeviceReady() && sw->ContentDrawn() &&
                     SUCCEEDED(sw->LastDrawHr()) && sw->Renders() > settingsRendersBefore ? S_OK : E_FAIL);
                if (sw) sw->Close();
            }
            Step("ui_loss_one_notice", notices_.size() == lossNotices + 1 ? S_OK : E_FAIL);
            Note("ui_loss_where", tm.where);
            Note("ui_loss_device_create_ms", std::to_string((tm.deviceReadyUs - tm.startUs) / 1000.0));
            Note("ui_loss_hook_after_device_ms", std::to_string((tm.hookUs - tm.deviceReadyUs) / 1000.0));
            Note("ui_loss_device_to_ui_ms", std::to_string(tm.DeviceToUiMs()));
            Note("ui_loss_detect_to_ui_ms", std::to_string(tm.DetectToUiMs()));
            Step("ui_loss_ui_back_within_500ms_of_detection", (rebuilt && tm.DetectToUiMs() < 500.0) ? S_OK : E_FAIL);
            {
                double firstUs = 0;
                const bool restored = waitRestored(lossGen, wvp::PlayerState::Playing, firstUs);
                const double delta = player_->Position() - lossPos;
                Note("ui_loss_detect_to_first_video_frame_ms", restored ? std::to_string((firstUs - tm.detectedUs) / 1000.0) : "not restored");
                Note("ui_loss_resume_position_delta_s", std::to_string(delta));
                // keyframe seek: at most one GOP back, plus the playback that continues while the reopen runs
                Step("ui_loss_playback_resumed_playing_at_position", (restored && delta > -2.0 && delta < 2.0) ? S_OK : E_FAIL);
            }
            closeLossNotices();
            // A reopen while paused (device loss, a settings change) presents the landing keyframe and stays paused; captions and
            // Acrylic are drawn on it, and play resumes in sync without drops.
            auto info = [&] {
                auto js = player_->GetDebugInfoJson();
                return js ? wvp::ParseDebugInfo(*js) : wvp::DebugInfo{};
            };
            auto pausedReopen = [&](const char* prefix, const std::function<void()>& trigger, bool reopens = true) {
                const std::string pre = prefix;
                if (aribCuePos > 0) {   // a cue on screen, so the paused frame must carry it; aribCuePos is the cue's start,
                    const double target = aribCuePos + 1.5;   // and the reopen lands on a keyframe up to a GOP earlier
                    player_->Seek(target, false);
                    PumpUntil([&] {   // landed (a cue still shown from before the seek proves nothing)
                        const double p = player_->Position();
                        return p == p && std::fabs(p - target) < 1.0 && info().CaptionLayer.RectCount.value_or(0) > 0;
                    }, 3000);
                }
                OpenProbePopup(host_, &d2d_, {layout_.widthDip / 2, layout_.heightDip / 2});   // a popup over the video (an Acrylic region)
                ForceRender();
                PumpUntil([&] { return info().Presenter.Acrylic.Regions.value_or(0) > 0; }, 1000);
                // a short clip: seek to 1 s only now and pause at once, so the sample stays near the start (a clip with one keyframe lands
                // the reopen at 0 and must not be paused further in than the 2 s the landing check allows)
                if (aribCuePos <= 0 && player_->Duration() < 20) {
                    player_->Seek(1.0, false);
                    PumpUntil([&] { const double p = player_->Position(); return p == p && p < 2.5 && state_ == wvp::PlayerState::Playing; }, 3000);
                }
                if (state_ == wvp::PlayerState::Playing) player_->Pause();
                PumpUntil([&] { const double p = player_->Position(); return state_ == wvp::PlayerState::Paused && p == p; }, 2000);
                const double pausedPos = player_->Position();
                const wvp::DebugInfo before = info();
                const bool captionBefore = before.CaptionLayer.RectCount.value_or(0) > 0;
                const int acrylicRegions = before.Presenter.Acrylic.Regions.value_or(0);
                const int gen = pipe_.Generation();
                const size_t notices = notices_.size();
                trigger();
                double firstUs = 0;
                const bool restored = waitRestored(reopens ? gen : gen - 1, wvp::PlayerState::Paused, firstUs);
                PumpUntil([] { return false; }, 300);
                const wvp::DebugInfo after = info();
                const double delta = player_->Position() - pausedPos;
                Note((pre + "_position_delta_s").c_str(), std::to_string(delta));
                Note((pre + "_presented_frames").c_str(), std::to_string(presentedFrames()));
                Step((pre + "_stays_paused_at_position").c_str(),
                     (restored && state_ == wvp::PlayerState::Paused && delta > -2.0 && delta < 0.5) ? S_OK : E_FAIL);
                Note((pre + "_caption_rects_before_after").c_str(), std::to_string(before.CaptionLayer.RectCount.value_or(0)) + " / " +
                                                           std::to_string(after.CaptionLayer.RectCount.value_or(0)));
                Note((pre + "_caption_cue_start_vs_position").c_str(), std::to_string(before.CaptionLayer.CueStart.value_or(-1)) + " / " +
                                                                        std::to_string(pausedPos) + " -> " + std::to_string(player_->Position()));
                if (captionBefore) {
                    // The reopen lands on a keyframe up to a GOP earlier: the cue that was on screen belongs on the paused frame
                    // only if it had started by then (both are stream timestamps). The reopened ARIB decoder continues the
                    // caption history of the same file, so a cue that started before the landing point comes from it and
                    // is shown with the landing frame, like one starting on it.
                    const double cueStart = before.CaptionLayer.CueStart.value_or(NAN), cueEnd = before.CaptionLayer.CueEnd.value_or(INFINITY);
                    const double frameClock = after.Sync.VideoClock.value_or(NAN);
                    // The position was just played, so the history covers it (a fresh resume open or a forward seek into an
                    // unplayed part is not tested here: those have no history and keep waiting for the next cue).
                    const bool active = frameClock == frameClock && cueStart == cueStart && frameClock >= cueStart - 0.001 && frameClock < cueEnd;
                    const bool due = active && frameClock - cueStart <= 0.001;
                    if (active && !due) Note((pre + "_caption_started_before_landing").c_str(), "from the caption history");
                    const double t0 = lossQpcUs();
                    const bool shown = PumpUntil([&] { return info().CaptionLayer.RectCount.value_or(0) > 0; }, active ? 2000 : 500);
                    Note((pre + "_caption_cue_vs_frame").c_str(), std::to_string(cueStart) + " .. " + std::to_string(cueEnd) + " / frame " +
                                                                     std::to_string(frameClock) + (due ? " (due)" : active ? " (started before landing)" : " (not due)"));
                    Note((pre + "_caption_after_frame_ms").c_str(), shown ? std::to_string((lossQpcUs() - t0) / 1000.0 + 300.0) : "not shown");
                    {
                        const wvp::DebugSubtitleLayer cl = info().CaptionLayer;
                        Note((pre + "_caption_layer").c_str(), "present " + std::to_string(cl.Present) + ", rects " + std::to_string(cl.RectCount.value_or(-1)) +
                             ", shown " + std::to_string(cl.CuesShown.value_or(-1)) + ", cleared " + std::to_string(cl.CuesCleared.value_or(-1)) +
                             ", bitmaps " + std::to_string(cl.BitmapsUploaded.value_or(-1)) + ", rerenders " + std::to_string(cl.Rerenders.value_or(-1)) +
                             ", cue " + std::to_string(cl.CueStart.value_or(-1)) + ", canvas " + std::to_string(cl.CanvasWidth.value_or(-1)) + "x" +
                             std::to_string(cl.CanvasHeight.value_or(-1)) + ", history " + std::to_string(cl.HistoryEvents.value_or(-1)) + " events " +
                             std::to_string(cl.HistoryBytes.value_or(-1)) + " bytes");
                    }
                    if (active)   // must show, a cue that started before the landing point too: the history covers it
                        Step((pre + "_caption_on_paused_frame").c_str(), shown && state_ == wvp::PlayerState::Paused ? S_OK : E_FAIL);
                    else Note((pre + "_caption_shown").c_str(), shown ? "true" : "false");
                }
                Note((pre + "_acrylic_regions_blurred").c_str(), std::to_string(acrylicRegions) + " / " +
                                                        std::to_string(after.Presenter.Acrylic.FramesBlurred.value_or(0)));
                if (acrylicRegions > 0)
                    Step((pre + "_acrylic_on_paused_frame").c_str(), after.Presenter.Acrylic.FramesBlurred.value_or(0) > 0 ? S_OK : E_FAIL);
                for (int i = 0; i < 8 && notices_.size() > notices; i++) CloseNotice(notices_.back().w);
                host_.CloseAllPopups();
                // play (not within a second of the reopen, as a person would): in sync, no drops
                PumpUntil([] { return false; }, 1000);
                const wvp::DebugInfo settled = info();
                const long long drops0 = settled.Sync.FrameDropsEarly.value_or(0) + settled.Sync.FrameDropsLate.value_or(0);
                player_->Play();
                const bool playing = PumpUntil([&] { return state_ == wvp::PlayerState::Playing; }, 2000);
                std::string avTrace;   // A/V difference and clocks over the first seconds of play
                double audioPrev = NAN, avWhileAudio = NAN;   // A/V at the last point where the audio clock still advanced
                for (int ms : { 500, 500, 500 }) {
                    PumpUntil([] { return false; }, ms);
                    const wvp::DebugInfo t = info();
                    const double a = t.Sync.AudioClock.value_or(NAN);
                    if (audioPrev == audioPrev && a - audioPrev >= 0.4) avWhileAudio = t.Sync.AvDiffMs.value_or(NAN);
                    audioPrev = a;
                    avTrace += (avTrace.empty() ? "" : " | ") + std::to_string(t.Sync.AvDiffMs.value_or(NAN)) + " (a " +
                               std::to_string(t.Sync.AudioClock.value_or(NAN)) + ", v " + std::to_string(t.Sync.VideoClock.value_or(NAN)) + ")";
                }
                Note((pre + "_resume_av_trace").c_str(), avTrace);
                const wvp::DebugInfo run = info();
                Note((pre + "_caption_rects_after_play").c_str(), std::to_string(run.CaptionLayer.RectCount.value_or(0)));
                const long long drops = run.Sync.FrameDropsEarly.value_or(0) + run.Sync.FrameDropsLate.value_or(0) - drops0;
                const double av = run.Sync.AvDiffMs.value_or(0);
                Note((pre + "_resume_drops_av_ms").c_str(), std::to_string(drops) + " (early " +
                     std::to_string(run.Sync.FrameDropsEarly.value_or(0) - settled.Sync.FrameDropsEarly.value_or(0)) + ", late " +
                     std::to_string(run.Sync.FrameDropsLate.value_or(0) - settled.Sync.FrameDropsLate.value_or(0)) + ") / " + std::to_string(av));
                // A/V is only judged while audio plays: at the end of a short clip the audio track stops before the video
                const bool audioRunning = avWhileAudio == avWhileAudio;
                if (!audioRunning) Note((pre + "_resume_audio_ended").c_str(), "A/V not judged");
                Step((pre + "_resume_in_sync_without_drops").c_str(),
                     (playing && drops == 0 && (!audioRunning || (avWhileAudio > -100 && avWhileAudio < 100))) ? S_OK : E_FAIL);
            };
            const std::string configuredDecoder = svc_->settings.hwDecoder;
            pausedReopen("baseline_pause_play", [] {}, false);   // no reopen: the drop / caption numbers to compare with
            pausedReopen("ui_loss_paused", [&] { SimulateUiDeviceLoss(); });
            pausedReopen("settings_reopen_paused", [&] {
                wvp::AppSettings before = svc_->settings, after = before;   // a decoder change reopens at the position
                after.hwDecoder = before.hwDecoder == "off" ? "auto" : "off";
                svc_->settings = after;
                ApplySettingsLive(*ctl_, svc_->filters, before, after);
            });
            if (svc_->settings.hwDecoder != configuredDecoder) {   // back to the configured decoder (playing, not measured)
                wvp::AppSettings before = svc_->settings, after = before;
                after.hwDecoder = configuredDecoder;
                svc_->settings = after;
                ApplySettingsLive(*ctl_, svc_->filters, before, after);
                PumpUntil([&] { return state_ == wvp::PlayerState::Playing; }, 3000);
            }

            // repeated loss: 3 attempts per 30 s, then the WARP fallback; no further rebuilds
            for (int i = 0; i < 3; i++) {
                const int before = RebindIncidents();
                SimulateUiDeviceLoss();
                PumpUntil([&] { return RebindIncidents() > before && !RebindBusy(); }, 3000);
            }
            Step("ui_loss_repeated_takes_fallback", (RebindFallbackActive() && Warp()) ? S_OK : E_FAIL);
            Step("ui_loss_fallback_ui_intact", uiIntact() ? S_OK : E_FAIL);
            Step("ui_loss_fallback_playback_software", playAdapter_.luid.empty() ? S_OK : E_FAIL);
            const int incidents = RebindIncidents(), rebuilds = RebindCount();
            SimulateUiDeviceLoss();
            PumpUntil([] { return false; }, 300);
            Step("ui_loss_no_loop_after_fallback", (RebindIncidents() == incidents && RebindCount() == rebuilds && !RebindBusy()) ? S_OK : E_FAIL);
            closeLossNotices();

            ResetRebindFallback();
            const bool restored = PumpUntil([&] { return !RebindBusy() && !RebindFallbackActive() && RebindCount() > rebuilds; }, 3000);
            Step("ui_loss_hardware_ui_restored", (restored && Warp() == warpBefore && uiIntact()) ? S_OK : E_FAIL);
            closeLossNotices();
            {   // test-only WVP_HEADLESS_REOPEN_SAMPLES=N: N more settings-triggered paused reopens (resume stability)
                wchar_t n[16] = {};
                const int extra = GetEnvironmentVariableW(L"WVP_HEADLESS_REOPEN_SAMPLES", n, 16) ? _wtoi(n) : 0;
                for (int i = 0; i < extra; i++) {
                    const std::string prefix = "reopen_sample_" + std::to_string(i + 1);
                    pausedReopen(prefix.c_str(), [&] {
                        wvp::AppSettings before = svc_->settings, after = before;
                        after.hwDecoder = before.hwDecoder == configuredDecoder ? (configuredDecoder == "off" ? "auto" : "off") : configuredDecoder;
                        svc_->settings = after;
                        ApplySettingsLive(*ctl_, svc_->filters, before, after);
                    });
                }
                if (svc_->settings.hwDecoder != configuredDecoder) {
                    wvp::AppSettings before = svc_->settings, after = before;
                    after.hwDecoder = configuredDecoder;
                    svc_->settings = after;
                    ApplySettingsLive(*ctl_, svc_->filters, before, after);
                    PumpUntil([&] { return state_ == wvp::PlayerState::Playing; }, 3000);
                }
            }
        }
        {   // seek logging (the log-file step below looks for it): the skip key hook ignores keys while the media is not seekable
            ensurePlaying("playback_restarted_before_seek");
            Execute(CommandId::SeekBack5);
        }
        {
            host_.SetFocus(nullptr);   // as a click on the video does: Tab then toggles instead of moving focus
            Step("debug_overlay_hidden_by_default", (!DebugOverlayVisible() && !DebugOverlayDrawn()) ? S_OK : E_FAIL);
            int drawsBefore = DebugDraws();
            PumpUntil([] { return false; }, 700);
            Step("debug_overlay_no_repaint_while_hidden", (DebugDraws() == drawsBefore && !DebugOverlayDrawn()) ? S_OK : E_FAIL);
            SendMessageW(hwnd_, WM_KEYDOWN, VK_TAB, 0);
            Note("tab1", LastTabDecision());
            ForceRender();
            Step("debug_overlay_tab_toggles_on_in_now_playing", (DebugOverlayVisible() && DebugOverlayDrawn() && SUCCEEDED(lastDrawHr_)) ? S_OK : E_FAIL);
            {
                int dbgZ = LayerIndex(Layer::DebugOverlay);
                bool z = LayerStackMatches() && dbgZ > LayerIndex(Layer::Content) &&
                         dbgZ < LayerIndex(Layer::Title) && dbgZ < LayerIndex(Layer::Bar) && dbgZ < LayerIndex(Layer::Popup);
                Step("debug_overlay_layer_below_title_bar_popup", z ? S_OK : E_FAIL);
            }
            auto hasAll = [&] {
                std::string t = DebugText();
                return t.find("output    ") != std::string::npos && t.find("output    ?") == std::string::npos && t.find("decoder ") != std::string::npos &&
                       t.find("frame_path ") != std::string::npos && t.find("A-V ") != std::string::npos && t.find("codec ") != std::string::npos &&
                       t.find("hwaccel") != std::string::npos && t.find("master ") != std::string::npos && t.find("no debug info") == std::string::npos;
            };
            bool text = PumpUntil(hasAll, 6000);
            Step("debug_overlay_text_has_renderer_decoder_frame_path_av", text ? S_OK : E_FAIL);
            {   // the result JSON writer does not escape notes: keep the text on one line without quotes / backslashes
                std::string t = DebugText();
                for (char& ch : t) {
                    if (ch == '\n') ch = '|';
                    else if (ch == '"') ch = '\'';
                    else if (ch == '\\') ch = '/';
                }
                Note("overlay_text", t);
            }
            auto posLine = [&] {
                std::string t = DebugText();
                size_t a = t.find("position  ");
                return a == std::string::npos ? std::string() : t.substr(a, t.find('\n', a) - a);
            };
            std::string p0 = posLine();
            bool changed = PumpUntil([&] { return !posLine().empty() && posLine() != p0; }, 4000);
            Step("debug_overlay_refresh_updates_position", (changed && !p0.empty()) ? S_OK : E_FAIL);
            ForceRender();
            Step("debug_overlay_surface_redrawn_after_refresh", (DebugOverlayDrawn() && SUCCEEDED(lastDrawHr_) && !DebugRows().empty()) ? S_OK : E_FAIL);
            {
                OpenProbePopup(host_, &d2d_, {layout_.widthDip / 2, layout_.heightDip / 2});
                SendMessageW(hwnd_, WM_KEYDOWN, VK_TAB, 0);
                Note("tab_popup", LastTabDecision());
                Step("debug_overlay_tab_with_popup_open_keeps_focus_traversal", (DebugOverlayVisible() && host_.PopupCount() > 0) ? S_OK : E_FAIL);
                host_.CloseAllPopups();
                host_.SetFocus(nullptr);
                ForceRender();
            }
            Step("debug_overlay_popup_above_overlay", CheckPopupOverBar());
            // Tab off, then on again: left visible for the full-screen / mini-player checks below
            SendMessageW(hwnd_, WM_KEYDOWN, VK_TAB, 0);
            Note("tab_off", LastTabDecision());
            ForceRender();
            Step("debug_overlay_tab_toggles_off", (!DebugOverlayVisible() && !DebugOverlayDrawn()) ? S_OK : E_FAIL);
            SendMessageW(hwnd_, WM_KEYDOWN, VK_TAB, 0);
            Note("tab_on_again", LastTabDecision());
            ForceRender();
            Step("debug_overlay_tab_toggles_on_again", (DebugOverlayVisible() && DebugOverlayDrawn()) ? S_OK : E_FAIL);
        }
        RECT vr = VideoRect();
        bool geo = (vr.right - vr.left) == layout_.video.w() && (vr.bottom - vr.top) == layout_.video.h() && layout_.video.t == S(kStripDip);
        Step("now_playing_video_geometry", geo ? S_OK : E_FAIL);
        // the windowed player docks the bar under the video: it never auto-hides there (full screen / mini player do)
        ensurePlaying("playback_restarted_before_bar_dock_check");
        autoHide_.Touch(Now() - 4000);
        AutoHideTick(Now());
        Step("windowed_bar_is_docked_and_never_hides", (!autoHide_.Enabled() && !autoHide_.Hidden() && !layout_.barOverlaysVideo &&
             layout_.video.b <= layout_.barPx.t && layout_.barPx.h() == S(kDockDip)) ? S_OK : E_FAIL);
        {   // the playlist panel is its own layer directly above the content; its rect spans the full body height and overlaps neither the video nor the dock (which sits under the video only)
            ForceRender();
            const RectPx& p = layout_.panelPx;
            const bool panelOk = !p.Empty() && shell_->Panel()->Visible() && sPanel_.surface && LayerStackMatches() &&
                                 LayerIndex(Layer::Panel) == LayerIndex(Layer::Content) + 1 && sPanel_.w == p.w() && sPanel_.h == p.h() &&
                                 p.l >= layout_.video.r && p.l >= layout_.barPx.r && p.t >= layout_.video.t && p.b <= ch_;
            Step("playlist_panel_layer_beside_video", panelOk ? S_OK : E_FAIL);
        }
        int oldW = cw_;
        SetWindowPos(hwnd_, nullptr, 0, 0, S(1000), S(600), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        PumpUntil([&] { return cw_ != oldW; }, 3000);
        vr = VideoRect();
        bool vidOk = (vr.right - vr.left) == layout_.video.w() && (vr.bottom - vr.top) == layout_.video.h() && layout_.video.b == ch_ - S(kDockDip);
        RECT clr{}; GetClientRect(hwnd_, &clr);
        RECT wnr{}; GetWindowRect(hwnd_, &wnr);
        bool frameOk = (wnr.right - wnr.left) == S(1000) && cw_ == clr.right && cw_ <= S(1000) && cw_ >= S(1000) - S(40);   // side borders are non-client
        Step("resize_hidden_video_follows", (frameOk && vidOk) ? S_OK : E_FAIL);
        ForceRender();
        Step("resize_redraw", lastDrawHr_);
        {   // Always on top "While fullscreen": the real enter / exit re-evaluate it. The window stays hidden, and Windows
            // does not reliably keep a hidden, never-activated full-screen popup in the topmost band, so this checks the
            // decision ApplyWindowTopmost made rather than WS_EX_TOPMOST.
            const auto savedAot = svc_->settings.alwaysOnTop;
            svc_->settings.alwaysOnTop = wvp::AlwaysOnTop::WhenFullscreen;
            ApplyWindowTopmost();
            const bool windowedBefore = !topmostWanted_;
            SetFullscreen(true);
            const bool inFullscreen = Mode() == ShellMode::FullScreen && topmostWanted_;
            SetFullscreen(false);
            const bool windowedAfter = Mode() == ShellMode::NowPlaying && !topmostWanted_;
            svc_->settings.alwaysOnTop = savedAot;
            ApplyWindowTopmost();
            Step("aot_while_fullscreen_follows_fullscreen_enter_exit",
                 windowedBefore && inFullscreen && windowedAfter && !IsWindowVisible(hwnd_) ? S_OK : E_FAIL);
        }
        SetFullscreen(true);
        vr = VideoRect();
        Step("fullscreen_mode_and_video_fills", (Mode() == ShellMode::FullScreen && (vr.right - vr.left) == cw_ && (vr.bottom - vr.top) == ch_) ? S_OK : E_FAIL);
        ForceRender();
        Step("fullscreen_render", lastDrawHr_);
        const TitleBar* fullscreenTitle = Shell()->Title();
        const bool titleSurface = fullscreenTitle->Visible() && fullscreenTitle->FullscreenMode() && !Layout().title.Empty() &&
                                  sTitle_.surface && sTitle_.w == cw_ && sTitle_.h == S(kStripDip);
        Step("fullscreen_title_layer_visible_and_sized", titleSurface ? S_OK : E_FAIL);
        const bool titleButtons = fullscreenTitle->MinimizeButton()->Visible() && fullscreenTitle->ExitFullscreenButton()->Visible() &&
                                  fullscreenTitle->CloseButton()->Visible() && fullscreenTitle->MinimizeButton()->SupportsInvoke() &&
                                  fullscreenTitle->ExitFullscreenButton()->SupportsInvoke() && fullscreenTitle->CloseButton()->SupportsInvoke();
        Step("fullscreen_title_caption_buttons_exist", titleButtons ? S_OK : E_FAIL);
        fullscreenTitle->ExitFullscreenButton()->Invoke();
        ForceRender();
        Step("fullscreen_title_exit_button_returns_to_windowed", Mode() == ShellMode::NowPlaying && !Layout().titleSlides &&
             !Shell()->Title()->FullscreenMode() ? S_OK : E_FAIL);
        ensurePlaying("playback_restarted_before_fullscreen_title_hide");   // the title below only auto-hides while Playing
        SetFullscreen(true);
        ForceRender();
        {   // the main menu from the title's Player button, also in full screen
            nativemenu::ClearLastShown();
            Shell()->Title()->MainMenuButton()->Invoke();
            ForceRender();
            Step("fullscreen_main_menu_opens_from_the_player_button", nativemenu::LastShown().size() > 10 && host_.PopupCount() == 0 &&
                 Mode() == ShellMode::FullScreen && VideoVisible() ? S_OK : E_FAIL);   // native menu: recorded, never shown
            host_.CloseAllPopups();
            Execute(CommandId::Escape);
            Step("fullscreen_escape_leaves_fullscreen_only", Mode() == ShellMode::NowPlaying ? S_OK : E_FAIL);
        }
        SetFullscreen(true);
        ensurePlaying("playback_restarted_before_fullscreen_chrome");   // the OSD only hides while Playing
        ForceRender();
        Step("fullscreen_entered_for_chrome_checks", Mode() == ShellMode::FullScreen ? S_OK : E_FAIL);
        StartBarAnim(autoHide_.Hide(Now()));
        StartTitleAnim(titleHide_.Hide(Now()));
        Step("fullscreen_bar_hide_animation_commit", (autoHide_.Hidden() ? lastCommitHr_ : E_FAIL));
        PumpUntil([] { return false; }, 600);
        Step("fullscreen_title_reports_offscreen_when_hidden", IsAutoHiddenChrome(Shell()->Title()) ? S_OK : E_FAIL);
        StartBarAnim(autoHide_.Enter(Now()));
        StartTitleAnim(titleHide_.Enter(Now()));
        PumpUntil([] { return false; }, 350);
        Step("fullscreen_title_offscreen_clears_on_show", !IsAutoHiddenChrome(Shell()->Title()) ? S_OK : E_FAIL);
        Step("fullscreen_debug_overlay_drawn", (DebugOverlayVisible() && DebugOverlayDrawn() && DebugCols() >= 8) ? S_OK : E_FAIL);
        Step("fullscreen_popup_above_bar", CheckPopupOverBar());
        checkInAppModal("fullscreen_modal_above_video_osd");
        auto checkFullscreenContextMenu = [&](const char* openStep, const char* invokeStep, pui::PointF cursor) {
            host_.CloseAllPopups();
            nativemenu::ClearLastShown();
            const int x = (int)std::lround(cursor.x * scale_) - layout_.video.l, y = (int)std::lround(cursor.y * scale_) - layout_.video.t;
            SendMessageW(video_, WM_RBUTTONUP, 0, MAKELPARAM(x, y));   // a right-button release over the video child
            ForceRender();
            auto items = nativemenu::LastShown();   // the native menu was not shown (tests): its descriptors were recorded
            Step(openStep, (items.size() > 10 && host_.PopupCount() == 0) ? S_OK : E_FAIL);
            // The "Properties" item (looked up by text) runs its action directly: an item click must never open an OS dialog.
            const bool found = nativemenu::FindItem(items, L"Properties…") != nullptr;
            const bool ran = found && nativemenu::InvokeItem(items, L"Properties…");
            if (ran) {
                SendMessageW(hwnd_, WM_RUN_QUEUE, 0, 0);
                ForceRender();
            }
            Step(invokeStep, (ran && host_.HasModalPopup()) ? S_OK : E_FAIL);   // Properties opens only through the menu item.
            host_.CloseAllPopups();
            UpdateAcrylicState(true);
            ForceRender();
        };
        const pui::PointF videoMenuPoint{cw_ / (2.f * scale_), ch_ / (2.f * scale_)};
        checkFullscreenContextMenu("fullscreen_video_rightclick_opens_menu", "fullscreen_video_menu_item_invokes", videoMenuPoint);
        {   // a notice (toast) open: a right-click reaching the main window still opens the menu
            host_.CloseAllPopups();
            nativemenu::ClearLastShown();
            ShowToast(L"Speed: 1.1x");
            const POINT px{(LONG)std::lround(videoMenuPoint.x * scale_), (LONG)std::lround(videoMenuPoint.y * scale_)};
            SendMessageW(hwnd_, WM_RBUTTONDOWN, MK_RBUTTON, MAKELPARAM(px.x, px.y));
            SendMessageW(hwnd_, WM_RBUTTONUP, 0, MAKELPARAM(px.x, px.y));
            ForceRender();
            Step("fullscreen_rightclick_with_toast_opens_menu", !nativemenu::LastShown().empty() ? S_OK : E_FAIL);
            host_.CloseAllPopups();
            ForceRender();
        }
        autoHide_.Snap(1.f);
        titleHide_.Snap(1.f);
        SnapBar();
        ForceRender();
        const bool chromeWasHidden = autoHide_.Hidden() && IsAutoHiddenChrome(Shell()->Title());
        Step("fullscreen_chrome_hidden_before_rightclick", chromeWasHidden ? S_OK : E_FAIL);
        checkFullscreenContextMenu("fullscreen_hidden_chrome_rightclick_opens_menu", "fullscreen_hidden_chrome_menu_item_invokes", videoMenuPoint);
        host_.CloseAllPopups();
        ForceRender();
        Step("fullscreen_window_still_hidden", !IsWindowVisible(hwnd_) ? S_OK : E_FAIL);
        Execute(CommandId::Escape);
        Step("escape_exits_fullscreen", (!IsFullscreen() && Mode() == ShellMode::NowPlaying) ? S_OK : E_FAIL);
        {
            Step("main_menu_button_accessible", Shell()->Title()->MainMenuButton()->Name() == L"Main menu" && Shell()->Title()->MainMenuButton()->SupportsInvoke() ? S_OK : E_FAIL);
            nativemenu::ClearLastShown();
            Shell()->Title()->MainMenuButton()->Invoke();
            ForceRender();
            Step("main_menu_button_opens_native_menu", !nativemenu::LastShown().empty() && host_.PopupCount() == 0 && VideoVisible() &&
                 Mode() == ShellMode::NowPlaying && SUCCEEDED(lastDrawHr_) ? S_OK : E_FAIL);
            {   // the pin cycles the persisted always-on-top mode and shows it
                const auto aotKeep = svc_->settings.alwaysOnTop;
                svc_->settings.alwaysOnTop = wvp::AlwaysOnTop::Disable;
                SyncModel();
                const bool pinOff = Shell()->Title()->PinButton()->Glyph() == pui::glyph::Str(pui::glyph::Pin);
                Shell()->Title()->PinButton()->Invoke();
                Step("title_pin_cycles_always_on_top", (pinOff && svc_->settings.alwaysOnTop == wvp::AlwaysOnTop::Enable &&
                     Shell()->Title()->PinButton()->Glyph() == pui::glyph::Str(pui::glyph::Pinned) &&
                     Shell()->Title()->PinButton()->ToolTip() == TitleBar::PinToolTip(wvp::AlwaysOnTop::Enable)) ? S_OK : E_FAIL);
                svc_->settings.alwaysOnTop = aotKeep;
                ApplyWindowTopmost();
                SyncModel();
                host_.CloseAllPopups();
            }
        }
        {   // Always on top: an outside z-order change cannot drop the window out of the topmost band
            auto keep = svc_->settings.alwaysOnTop;
            svc_->settings.alwaysOnTop = wvp::AlwaysOnTop::Enable;
            ApplyWindowTopmost();
            SetWindowPos(hwnd_, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);   // the lowering we guard against
            Step("always_on_top_survives_notopmost_request", (GetWindowLongPtrW(hwnd_, GWL_EXSTYLE) & WS_EX_TOPMOST) ? S_OK : E_FAIL);
            SetWindowPos(hwnd_, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            Step("always_on_top_survives_bottom_request", (GetWindowLongPtrW(hwnd_, GWL_EXSTYLE) & WS_EX_TOPMOST) ? S_OK : E_FAIL);
            Step("always_on_top_survives_outside_lowering", (GetWindowLongPtrW(hwnd_, GWL_EXSTYLE) & WS_EX_TOPMOST) ? S_OK : E_FAIL);
            svc_->settings.alwaysOnTop = wvp::AlwaysOnTop::Disable;
            ApplyWindowTopmost();
            Step("always_on_top_off_leaves_the_band", !(GetWindowLongPtrW(hwnd_, GWL_EXSTYLE) & WS_EX_TOPMOST) ? S_OK : E_FAIL);
            svc_->settings.alwaysOnTop = keep;
            ApplyWindowTopmost();
        }
        {
            RECT before{}; GetWindowRect(hwnd_, &before);
            SetMiniPlayer(true);
            Step("mini_mode", Mode() == ShellMode::MiniPlayer ? S_OK : E_FAIL);
            Step("mini_topmost", (GetWindowLongPtrW(hwnd_, GWL_EXSTYLE) & WS_EX_TOPMOST) ? S_OK : E_FAIL);
            PxSize mn = MiniMinSizePx(Scale());
            Step("mini_size_ge_min", (ClientW() >= mn.w && ClientH() >= mn.h) ? S_OK : E_FAIL);
            RECT mvr = VideoRect();
            Step("mini_video_fills_client", (VideoVisible() && mvr.right - mvr.left == ClientW() && mvr.bottom - mvr.top == ClientH() &&
                                             Layout().title.Empty()) ? S_OK : E_FAIL);
            Step("mini_bar_reduced", (Layout().barPx.h() == MiniBarHeightPx(Scale()) && Layout().barPx.b == ClientH() &&
                                      Shell()->Mini()->IsEffectivelyVisible() && !Shell()->Bar()->IsEffectivelyVisible()) ? S_OK : E_FAIL);
            {   // WM_SIZING keeps the media aspect in the mini player (client == window)
                RECT want{0, 0, 900, 300};
                SendMessageW(hwnd_, WM_SIZING, WMSZ_RIGHT, reinterpret_cast<LPARAM>(&want));
                bool ratio = std::fabs((want.right - want.left) - (want.bottom - want.top) * mediaAspect_) <= 1.0 && want.right - want.left == 900;
                RECT tiny{0, 0, 10, 10};
                SendMessageW(hwnd_, WM_SIZING, WMSZ_BOTTOMRIGHT, reinterpret_cast<LPARAM>(&tiny));
                bool minOk = tiny.right - tiny.left >= mn.w && tiny.bottom - tiny.top >= mn.h;
                Step("mini_wm_sizing_locks_aspect", (ratio && minOk) ? S_OK : E_FAIL);
            }
            Step("mini_window_still_hidden", !IsWindowVisible(hwnd_) ? S_OK : E_FAIL);
            ForceRender();
            Step("mini_render", lastDrawHr_);
            Step("mini_debug_overlay_drawn_within_window", (DebugOverlayVisible() && DebugOverlayDrawn() && DebugCols() >= 8 && DebugCols() <= (int)(ClientW() / 4)) ? S_OK : E_FAIL);
            Step("mini_popup_above_bar", CheckPopupOverBar());
            checkInAppModal("mini_modal_above_video_osd");
            checkFullscreenContextMenu("mini_video_rightclick_opens_menu", "mini_video_menu_item_invokes", {cw_ / (2.f * scale_), ch_ / (2.f * scale_)});
            Execute(CommandId::Escape);   // remapped: leaves the mini player only
            RECT after{}; GetWindowRect(hwnd_, &after);
            Step("mini_exit_restores", (Mode() == ShellMode::NowPlaying &&
                                        !(GetWindowLongPtrW(hwnd_, GWL_EXSTYLE) & WS_EX_TOPMOST) && EqualRect(&before, &after)) ? S_OK : E_FAIL);
        }
        Execute(CommandId::Escape);
        Step("escape_on_video_stays_video", Mode() == ShellMode::NowPlaying && VideoVisible() ? S_OK : E_FAIL);
        {   // Settings (F5): a separate window owned by the main window (hidden in this run), single instance, normal focus, Esc closes it
            const ShellMode modeBefore = Mode();
            Execute(CommandId::OpenSettings);
            SettingsWindow* sw = Settings();
            const HWND sh = sw ? sw->Hwnd() : nullptr;
            Step("settings_key_opens_the_settings_window", sh && GetWindow(sh, GW_OWNER) && GetWindow(sh, GW_OWNER) != hwnd_ && !IsWindowVisible(sh) &&
                 Mode() == modeBefore ? S_OK : E_FAIL);
            if (sw) sw->Render(true);
            Step("settings_window_renders_the_settings_page", sw && sw->DeviceReady() && sw->ContentDrawn() && SUCCEEDED(sw->LastDrawHr()) &&
                 sw->Page() && sw->Page()->AutomationId() == L"SettingsPage" ? S_OK : E_FAIL);
            Execute(CommandId::OpenSettings);
            Step("settings_window_is_single_instance", sw && sw->IsOpen() && sw->Hwnd() == sh ? S_OK : E_FAIL);
            if (sh) SendMessageW(sh, WM_KEYDOWN, VK_TAB, 0);
            Step("settings_window_tab_moves_focus", sw && sw->Ui().Focused() && sw->Page() && sw->Page()->IsAncestorOf(sw->Ui().Focused()) ? S_OK : E_FAIL);
            std::function<pui::Widget*(pui::Widget*, const wchar_t*)> find = [&](pui::Widget* w, const wchar_t* id) -> pui::Widget* {
                if (!w) return nullptr;
                if (w->AutomationId() == id) return w;
                for (auto& c : w->Children()) if (pui::Widget* f = find(c.get(), id)) return f;
                return nullptr;
            };
            pui::Widget* combo = sw ? find(sw->Page(), L"VideoGpuCombo") : nullptr;
            if (combo) { combo->Focus(); combo->Expand(); }
            if (sw) sw->Render(true);
            const bool flyout = sw && sw->Ui().PopupCount() == 1 && SUCCEEDED(sw->LastDrawHr()) && host_.PopupCount() == 0;
            if (sh) SendMessageW(sh, WM_KEYDOWN, VK_ESCAPE, 0);
            Step("settings_window_flyout_in_its_own_host_and_escape_closes_it_first", flyout && sw->IsOpen() && sw->Ui().PopupCount() == 0 ? S_OK : E_FAIL);
            if (sh) SendMessageW(sh, WM_KEYDOWN, VK_ESCAPE, 0);
            Step("settings_window_escape_closes_it", sw && !sw->IsOpen() && !IsWindow(sh) ? S_OK : E_FAIL);
        }
        // Reopen the supplied short clip: the drop smoke test above may have switched to a longer, different codec.
        // Natural EOF avoids coupling this navigation regression to decoder-specific near-EOF seeking.
        ctl_->SetRepeat(wvp::RepeatMode::Off);
        bool queueEnded = false;
        auto endToken = ctl_->QueueEnded.add([&] { queueEnded = true; });
        OpenPaths(*this, {opt_.inputs[0]});
        // The reopen is asynchronous: wait for the queue's current-item change (it clears model_.prepared) before waiting for
        // PREPARED, or the item still playing from the steps above is taken (and seeked) instead of the reopened one.
        PumpUntil([&] { return !model_.prepared; }, 5000);
        bool endClipPrepared = PumpUntil([&] { return model_.prepared && state_ == wvp::PlayerState::Playing; }, 20000);
        double endDuration = model_.dur;
        double endPosition = player_ ? player_->Position() : 0.0;
        if (player_) {
            double reportedDuration = player_->Duration();
            if (std::isfinite(reportedDuration) && reportedDuration > 0) endDuration = reportedDuration;
        }
        if (!std::isfinite(endPosition) || endPosition < 0) endPosition = 0.0;
        // A long input (the 90 s broadcast fixture) would make this wait for the whole clip: one ordinary (keyframe) seek to about
        // 8 s before the end, then the end is still reached by playing, not by a seek past it.
        constexpr double kQueueEndLeadSeconds = 8.0;
        if (endClipPrepared && player_ && model_.Seekable() && std::isfinite(endDuration) &&
            endDuration - endPosition > kQueueEndLeadSeconds + 7.0) {
            const double target = endDuration - kQueueEndLeadSeconds;
            LogSeek("headless", target, false);
            player_->Seek(target);
            PumpUntil([&] { double p = player_->Position(); return std::isfinite(p) && p > target - 5.0; }, 5000);
            endPosition = player_->Position();
            if (!std::isfinite(endPosition) || endPosition < 0) endPosition = 0.0;
            Note("queue_end_seek_target_s", std::to_string(target));
        }
        double endSpeed = svc_ ? svc_->filters.Settings().speed : 1.0;
        if (!std::isfinite(endSpeed) || endSpeed <= 0) endSpeed = 1.0;
        double remainingSeconds = std::isfinite(endDuration) && endDuration > 0
            ? std::max(0.0, endDuration - endPosition) / endSpeed : 0.0;
        constexpr double kQueueEndGraceSeconds = 15.0;
        DWORD queueEndTimeoutMs = static_cast<DWORD>(std::min((remainingSeconds + kQueueEndGraceSeconds) * 1000.0,
                                                              static_cast<double>(MAXDWORD)));
        Note("queue_end_duration_s", std::to_string(endDuration));
        Note("queue_end_start_position_s", std::to_string(endPosition));
        Note("queue_end_wait_s", std::to_string(queueEndTimeoutMs / 1000.0));
        bool finished = endClipPrepared && PumpUntil([&] { return queueEnded && state_ == wvp::PlayerState::Ended; }, queueEndTimeoutMs);
        AutoHideTick(Now());
        bool controlsShown = PumpUntil([&] { return !autoHide_.Hidden() && autoHide_.P(Now()) < 0.01f; }, 1000);
        ForceRender();
        Note("queue_end_clip_prepared", endClipPrepared ? "true" : "false");
        Note("queue_end_event", queueEnded ? "true" : "false");
        Note("queue_end_state", std::to_string((int)state_));
        Note("queue_end_mode", std::to_string((int)Mode()));
        Note("queue_end_controls_shown", controlsShown ? "true" : "false");
        Step("queue_end_keeps_video_and_controls", finished && Mode() == ShellMode::NowPlaying && VideoVisible() &&
             ctl_->Current() && Shell()->Bar()->IsEffectivelyVisible() && controlsShown &&
             SUCCEEDED(lastDrawHr_) ? S_OK : E_FAIL);
        ctl_->QueueEnded.remove(endToken);
        if (ctl_) ctl_->Clear();
        PumpUntil([&] { return !Loaded(); }, 3000);
        Step("clear_file_stays_in_the_player_view", !Loaded() && Mode() == ShellMode::NowPlaying && VideoVisible() ? S_OK : E_FAIL);
    }
    Step("windows_still_never_visible", (hwnd_ && !IsWindowVisible(hwnd_) &&
                                         (!Settings() || !Settings()->IsOpen() || !IsWindowVisible(Settings()->Hwnd()))) ? S_OK : E_FAIL);

    // log file (only when --log-file= was given): the app's own events + libffplay's av_log lines + the status line
    if (log::Global().HasFile(false)) {
        log::Global().Flush();
        std::ifstream in(log::Global().FilePath(false), std::ios::binary);
        std::string txt((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        // The events follow docs/debugging.md; the seek is the Skip -10 key issued above.
        std::vector<const char*> want = {"] open: url: ", "] open: options: ",
                                         "] player: prepared: ", "] player: state: Playing", "] mode: now-playing",
                                         "] seek: key relative -10", "] filter: vf (", "] av: "};
        if (opt_.stats) { want.push_back(" A-V:"); want.push_back("stats=1"); }
        std::string missing;
        for (const char* n : want)
            if (txt.find(n) == std::string::npos) missing += std::string(missing.empty() ? "" : " | ") + n;
        const bool ok = missing.empty();
        Step("log_file_has_open_options_prepared_av_and_status_lines", (ok && !txt.empty()) ? S_OK : E_FAIL);
        Note("log_file_bytes", std::to_string(txt.size()));
        Note("log_status_lines", std::to_string(log::Global().StatusLinesWritten()));
        if (!ok) {
            Note("log_file_missing", missing);
            std::fprintf(stderr, "log file check failed, missing: %s; first 3000 bytes:\n%.3000s\n", missing.c_str(), txt.c_str());
        }
    }

    // close: player disposed before the video child is destroyed
    if (hwnd_) SendMessageW(hwnd_, WM_CLOSE, 0, 0);
    PumpUntil([&] { return !IsWindow(hwnd_); }, 3000);
    Step("close_disposes_player_then_destroys", (!IsWindow(hwnd_) && !player_ && !ctl_) ? S_OK : E_FAIL);
    Destroy();

    bool allOk = true;
    std::string js = "{\n  \"steps\": [\n";
    for (size_t i = 0; i < steps_.size(); i++) {
        bool ok = SUCCEEDED(steps_[i].second);
        allOk = allOk && ok;
        char hr[16]; std::snprintf(hr, sizeof hr, "0x%08X", (unsigned)steps_[i].second);
        js += "    {\"name\": \"" + steps_[i].first + "\", \"ok\": " + (ok ? "true" : "false") + ", \"hr\": \"" + hr + "\"}";
        js += (i + 1 < steps_.size()) ? ",\n" : "\n";
    }
    wvp::JsonWriter noteWriter(false);
    noteWriter.BeginObject();
    for (const auto& note : notes_) noteWriter.Prop(note.first, note.second);
    noteWriter.EndObject();
    js += "  ],\n  \"notes\": " + noteWriter.Str();
    js += std::string(",\n  \"warp\": ") + (gfx_.warp ? "true" : "false") + ",\n  \"ok\": " + (allOk ? "true" : "false") + "\n}\n";
    if (!opt_.resultPath.empty()) { std::ofstream f(opt_.resultPath, std::ios::trunc | std::ios::binary); f << js; }
    std::printf("%s", js.c_str());
    return allOk ? 0 : 2;
}

}

namespace app {

bool App::LayerStackMatches() {
    const auto& z = gfx_.ZOrder();
    if (z.size() != (size_t)Layer::Count) return false;
    for (int i = 0; i < (int)Layer::Count; i++)
        if (z[(size_t)i] != LayerSurf(kLayerStack[i]).visual.Get()) return false;
    return true;
}

HRESULT App::CheckPopupOverBar() {
    host_.CloseAllPopups();
    OpenProbePopup(host_, &d2d_, {layout_.bar.cx(), std::max(8.f, layout_.bar.t - 40.f)});
    ForceRender();
    const auto& z = gfx_.ZOrder();
    auto idx = [&](Surf& s) { for (size_t i = 0; i < z.size(); i++) if (z[i] == s.visual.Get()) return (int)i; return -1; };
    bool ok = host_.PopupCount() > 0 && sPopup_.surface && LayerStackMatches() && idx(sPopup_) == (int)z.size() - 1 &&
              idx(sPopup_) > idx(sBar_) && idx(sPopup_) > idx(sTitle_) && SUCCEEDED(lastDrawHr_);
    host_.CloseAllPopups();
    ForceRender();
    return ok ? S_OK : E_FAIL;
}

}
