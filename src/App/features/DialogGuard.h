#pragma once
// --logictest and --headless-check run on the user's desktop: they must never show an OS dialog (file pickers, the text prompt).
// Every entry point that creates one checks this and behaves as "cancelled" instead.

namespace app {

inline bool& OsDialogsBlocked() {
    static bool blocked = false;
    return blocked;
}

}
