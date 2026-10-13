#include "Commands.h"

#include <algorithm>
#include <cmath>

namespace app {

namespace {
// Virtual-key codes (numeric so this file needs no windows.h).
constexpr int VK_TAB_ = 0x09, VK_RETURN_ = 0x0D, VK_ESCAPE_ = 0x1B, VK_SPACE_ = 0x20, VK_PRIOR_ = 0x21, VK_NEXT_ = 0x22,
              VK_LEFT_ = 0x25, VK_UP_ = 0x26, VK_RIGHT_ = 0x27, VK_DOWN_ = 0x28,
              VK_OEM_2_ = 0xBF, VK_OEM_COMMA_ = 0xBC, VK_OEM_PERIOD_ = 0xBE,
              VK_F1_ = 0x70, VK_F2_ = 0x71, VK_F3_ = 0x72, VK_F4_ = 0x73, VK_F5_ = 0x74, VK_F6_ = 0x75, VK_F7_ = 0x76;
constexpr unsigned N = 0, S = kModShift, C = kModCtrl, A = kModAlt, CS = kModCtrl | kModShift, CA = kModCtrl | kModAlt;

std::vector<KeyBinding> Make() {
    using K = CommandId;
    return {
        {'1', N, K::WindowSize50},
        {'2', N, K::WindowSize100},
        {'3', N, K::WindowSize150},
        {'4', N, K::WindowSize200},
        {'5', N, K::WindowSizeMax},
        {VK_RETURN_, N, K::ToggleFullscreen},
        {VK_SPACE_, N, K::PlayPause},
        {VK_ESCAPE_, N, K::Escape},
        {'X', N, K::SpeedDown},
        {VK_NEXT_, N, K::SpeedDown},
        {'C', N, K::SpeedUp},
        {VK_PRIOR_, N, K::SpeedUp},
        {'Z', N, K::SpeedReset},
        {VK_LEFT_, N, K::SeekBack5},
        {VK_RIGHT_, N, K::SeekForward5},
        {VK_LEFT_, CS, K::KeyframePrevious},
        {VK_RIGHT_, CS, K::KeyframeNext},
        {VK_LEFT_, C, K::SeekBack30},
        {VK_RIGHT_, C, K::SkipForward30},
        {VK_LEFT_, S, K::SeekBack60},
        {VK_RIGHT_, S, K::SeekForward60},
        {VK_LEFT_, CA, K::SeekBack300},
        {VK_RIGHT_, CA, K::SeekForward300},
        {'F', N, K::FrameNext},
        {'D', N, K::FramePrevious},
        {'M', N, K::ToggleMute},
        {VK_UP_, N, K::VolumeUp},
        {VK_DOWN_, N, K::VolumeDown},
        {'O', C, K::OpenFiles},
        {VK_F3_, N, K::OpenFiles},
        {'U', C, K::OpenUrl},
        {VK_F2_, N, K::OpenFolder},
        {VK_F4_, N, K::Stop},
        {VK_F5_, N, K::OpenSettings},
        {VK_F6_, N, K::TogglePlaylistPanel},
        {VK_F7_, N, K::ToggleControlPanel},
        {'T', C, K::CycleAlwaysOnTop},
        {'S', C, K::FocusPlaylistSearch},
        {'C', C, K::CaptureFrameWithCaptions},
        {'C', CS, K::CaptureFrame},
        {VK_F1_, C, K::ShowProperties},
        {'E', S, K::ShowEqualizer},
        {'V', CS, K::VideoEnhancements},
        {VK_TAB_, N, K::ToggleDebugOverlay},
        {'W', N, K::CycleSubtitleTrack},
        {VK_OEM_COMMA_, N, K::SubtitleDelayEarlier},
        {VK_OEM_PERIOD_, N, K::SubtitleDelayLater},
        {VK_OEM_2_, N, K::SubtitleDelayReset},
    };
}
}

const std::vector<KeyBinding>& KeyTable() {
    static const std::vector<KeyBinding> t = Make();
    return t;
}

CommandId LookupKey(int vk, unsigned mods) {
    mods &= (kModShift | kModCtrl | kModAlt);
    // vk are Win32 virtual-key codes: letters are the UPPER-case ASCII codes (0x61..0x7A are numpad/F-keys, never folded).
    for (const KeyBinding& b : KeyTable())
        if (b.vk == vk && b.mods == mods) return b.command;
    return CommandId::None;
}

std::wstring ShortcutText(CommandId c) {
    for (const KeyBinding& b : KeyTable()) {
        if (b.command != c) continue;
        std::wstring s;
        if (b.mods & kModCtrl) s += L"Ctrl+";
        if (b.mods & kModShift) s += L"Shift+";
        if (b.mods & kModAlt) s += L"Alt+";
        switch (b.vk) {
        case VK_SPACE_: s += L"Space"; break;
        case VK_TAB_: s += L"Tab"; break;
        case VK_LEFT_: s += L"Left"; break;
        case VK_RIGHT_: s += L"Right"; break;
        case VK_UP_: s += L"Up"; break;
        case VK_DOWN_: s += L"Down"; break;
        case VK_PRIOR_: s += L"PageUp"; break;
        case VK_NEXT_: s += L"PageDown"; break;
        case VK_RETURN_: s += L"Enter"; break;
        case VK_ESCAPE_: s += L"Esc"; break;
        case VK_OEM_2_: s += L"/"; break;
        case VK_OEM_COMMA_: s += L","; break;
        case VK_OEM_PERIOD_: s += L"."; break;
        case VK_F1_: s += L"F1"; break;
        case VK_F2_: s += L"F2"; break;
        case VK_F3_: s += L"F3"; break;
        case VK_F4_: s += L"F4"; break;
        case VK_F5_: s += L"F5"; break;
        case VK_F6_: s += L"F6"; break;
        case VK_F7_: s += L"F7"; break;
        default: s += (wchar_t)b.vk; break;
        }
        return s;
    }
    return {};
}

int SeekSeconds(CommandId c) {
    switch (c) {
    case CommandId::SeekBack5: return -5;
    case CommandId::SeekForward5: return 5;
    case CommandId::SeekBack30: return -30;
    case CommandId::SkipForward30: return 30;
    case CommandId::SeekBack60: return -60;
    case CommandId::SeekForward60: return 60;
    case CommandId::SeekBack300: return -300;
    case CommandId::SeekForward300: return 300;
    default: return 0;
    }
}

double WindowScale(CommandId c) {
    switch (c) {
    case CommandId::WindowSize50: return 0.5;
    case CommandId::WindowSize100: return 1.0;
    case CommandId::WindowSize150: return 1.5;
    case CommandId::WindowSize200: return 2.0;
    case CommandId::WindowSizeMax: return -1.0;
    default: return 0.0;
    }
}

double SteppedSpeed(double current, int tenths, double maxSpeed) {
    long long t = std::llround(current * 10.0) + tenths;   // integer tenths: 0.1 steps never drift
    long long maxT = std::max(1LL, (long long)std::floor(maxSpeed * 10.0 + 1e-9));
    return (double)std::clamp<long long>(t, 1, maxT) / 10.0;
}

}
