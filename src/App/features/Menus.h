#pragma once
// The item list is built by a pure function (BuildMenu) from a MenuState snapshot so it can be tested headless.
// There is ONE main menu: the "Player" button in the title strip and a right click on the video both open it.
#include <functional>
#include <vector>

#include "BarLogic.h"
#include "Commands.h"
#include "features/NativeMenu.h"
#include "pui/Menu.h"
#include "pui/Widget.h"
#include "wvp/AppSettings.h"
#include "wvp/FilterManager.h"
#include "wvp/PlayerTypes.h"

namespace app {
class AppContext;
/// The main menu at a client DIP position (right click on the video).
void ShowMainMenu(AppContext& ctx, pui::PointF ptDip);
/// The main menu below `anchor` (the title strip's Player button); cb.onOpen / cb.onClose drive its pressed look.
void ShowMainMenu(AppContext& ctx, pui::Widget* anchor, nativemenu::Callbacks cb = {});
/// The subtitle track list (the "Subtitles" sub-menu of the main menu) as its own menu; anchor = the bar's Subtitle button.
void ShowSubtitleMenu(AppContext& ctx, pui::Widget* anchor);

namespace menus {
enum class Cmd {
    Execute,         // index = CommandId (everything that is just a command with a shortcut)
    SetShuffle, SetRepeat,
    SetSpeed,        // value = speed
    SetRotation,     // value = degrees
    FlipH, FlipV, Zoom,   // flag = new state
    SetAlwaysOnTop,  // index = sl::AlwaysOnTopIndex
    SelectAudio,     // index = position among the audio streams
    SelectAribAudio, // ARIB TS: index = 0 audio 1, 1 audio 2 (the audio preference)
    SelectSubtitle,  // index = position among cycleable subtitle streams (superimpose excluded), -1 = off
};
struct MenuCmd {
    Cmd cmd = Cmd::Execute;
    double value = 0;
    int index = 0;
    bool flag = false;
    CommandId Command() const { return cmd == Cmd::Execute ? (CommandId)index : CommandId::None; }
};
struct MenuState {
    bool hasMedia = false, fullscreen = false, mini = false;
    BarModel bar;                               // playing / hasPrev / hasNext / shuffle / repeat / panelOpen / controlPanelOpen
    wvp::AlwaysOnTop alwaysOnTop = wvp::AlwaysOnTop::Disable;
    double speed = 1.0;
    wvp::Rotation rotation = wvp::Rotation::None;
    bool flipH = false, flipV = false, zoom = false;
    std::vector<wvp::StreamInfo> audio, subs;   // same-type streams of the current media
    // ARIB TS: the audio item is a fixed pair Audio 1 / Audio 2 (the preference); audio 2 stays selectable while no
    // stream holds it, because it may appear later. The check mark is the user's selection, never the source playing.
    bool aribAudio = false;
    int aribPref = 0;                           // 0 audio 1, 1 audio 2
    wvp::StreamInfo arib1, arib2;               // the streams of the two roles (empty when absent)
    bool arib1Present = false, arib2Present = false;
};
std::vector<pui::MenuItemDesc> BuildMenu(const MenuState& s, std::function<void(const MenuCmd&)> run);
/// The subtitle track items (Off + the tracks); empty when the media has no subtitle stream.
std::vector<pui::MenuItemDesc> BuildSubtitleItems(const MenuState& s, std::function<void(const MenuCmd&)> run);
MenuState CaptureMenuState(AppContext& ctx);
}
}
