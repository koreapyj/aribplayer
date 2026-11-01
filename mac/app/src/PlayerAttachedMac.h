#pragma once
// Z-order state shared by the windows attached to the player (SettingsWindowMac, PropertiesWindowMac), the Mac counterpart of
// src/App/shell/HostWindow: the window sits directly above the player, in its band (floating while the player is always on top
// or the mini player), until a mouse button goes down on the player; the player then stays in front until the next focus cycle
// (the window opened, or the app activated from another app). None of these activate the window, and all do nothing while it is
// closed or hidden for a minimized player.
#import <AppKit/AppKit.h>

#include "MainView.h"
#include "pui/Widget.h"

namespace mainview {

struct PlayerAttached {
    MainView& mv;
    NSWindow* window = nil;
    bool inFront = false;            // a press on the player put it in front of this window
    bool hiddenForPlayer = false;    // hidden while the player is minimized
    explicit PlayerAttached(MainView& m) : mv(m) {}

    NSWindow* Player() const { return (__bridge NSWindow*)(void*)mv.MainHwnd(); }
    bool Shown() const { return window.visible && !hiddenForPlayer; }
    /// Directly above the player, without activation, in the player's band.
    void RaiseAbove() {
        NSWindow* player = Player();
        if (!player) return;
        if (window.level != player.level) window.level = player.level;
        [window orderWindow:NSWindowAbove relativeTo:player.windowNumber];
    }
    void StartFocusCycle() {
        if (!Shown()) return;
        inFront = false;
        RaiseAbove();
    }
    void OnPlayerPressed() {
        if (!Shown() || inFront) return;
        inFront = true;
        // Out of the topmost band; below a player that is not in it.
        if (window.level != NSNormalWindowLevel) window.level = NSNormalWindowLevel;
        NSWindow* player = Player();
        if (player && player.level == NSNormalWindowLevel) [window orderWindow:NSWindowBelow relativeTo:player.windowNumber];
    }
    /// The player re-applied its level: stay directly above it unless it was put in front.
    void KeepAbove() {
        if (Shown() && !inFront) RaiseAbove();
    }
    void OnPlayerMinimized(pui::UiHost& host, bool minimized) {
        if (minimized) {
            if (!window.visible) return;
            hiddenForPlayer = true;
            host.CloseAllPopups();
            [window orderOut:nil];
        } else if (hiddenForPlayer) {
            hiddenForPlayer = false;
            RaiseAbove();   // orders it in without making it key
        }
    }
};

}
