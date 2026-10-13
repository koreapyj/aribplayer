#include "NativeMenuMac.h"

#include <algorithm>
#include <cwctype>
#include <memory>

#include "Localize.h"
#include "features/DialogGuard.h"
#include "pui/Widget.h"
#include "puimac/MenuPopup.h"

namespace app::nativemenu {

namespace {
pui::UiHost* g_host = nullptr;
pui::IRenderer* g_measurer = nullptr;
HWND g_owner = nullptr;
int g_open = 0;
std::vector<pui::MenuItemDesc> g_last;

}

std::wstring MacAccelerator(const std::wstring& win) {
    // "Ctrl+Shift+Left" -> "⇧⌘←": the key table's Ctrl is the Command key on macOS (puimac::ModsFromFlags maps Command
    // to ModCtrl); modifiers in the Mac order (⌃⌥⇧⌘), named keys as their symbols.
    if (win.empty()) return win;
    bool ctrl = false, shift = false, alt = false;
    std::wstring rest = win;
    for (;;) {
        if (rest.rfind(L"Ctrl+", 0) == 0) { ctrl = true; rest.erase(0, 5); }
        else if (rest.rfind(L"Shift+", 0) == 0) { shift = true; rest.erase(0, 6); }
        else if (rest.rfind(L"Alt+", 0) == 0) { alt = true; rest.erase(0, 4); }
        else break;
    }
    static const std::pair<const wchar_t*, const wchar_t*> keys[] = {
        {L"Left", L"\u2190"}, {L"Right", L"\u2192"}, {L"Up", L"\u2191"}, {L"Down", L"\u2193"}, {L"Enter", L"\u21A9"},
        {L"Esc", L"\u238B"}, {L"Space", L"Space"}, {L"PageUp", L"\u21DE"}, {L"PageDown", L"\u21DF"}, {L"Tab", L"\u21E5"}};
    for (const auto& [from, to] : keys)
        if (rest == from) { rest = to; break; }
    std::wstring out;
    if (alt) out += L"\u2325";
    if (shift) out += L"\u21E7";
    if (ctrl) out += L"\u2318";
    return out + rest;
}

namespace {
constexpr wchar_t kLeft = 0xF702, kRight = 0xF703, kF1 = 0xF704;   // NSLeftArrowFunctionKey, NSRightArrowFunctionKey, NSF1FunctionKey
}

const std::vector<MacShortcut>& MacShortcuts() {
    using C = CommandId;
    static const std::vector<MacShortcut> t = {
        {C::OpenFiles, L'o', kMacCmd},           {C::OpenFolder, L'o', kMacCmd | kMacShift},
        {C::OpenUrl, L'u', kMacCmd},             {C::Stop, (wchar_t)(kF1 + 3), 0},          // F4, as Commands.cpp
        {C::ShowProperties, L'i', kMacCmd},      {C::OpenSettings, L',', kMacCmd},
        {C::Exit, L'q', kMacCmd},                {C::Previous, L'[', kMacCmd},
        {C::Next, L']', kMacCmd},                {C::SeekBack30, kLeft, kMacCmd},
        {C::SkipForward30, kRight, kMacCmd},     {C::SeekBack300, kLeft, kMacCmd | kMacOpt},
        {C::SeekForward300, kRight, kMacCmd | kMacOpt},
        {C::CaptureFrame, L'c', kMacCmd | kMacShift},
        {C::FocusPlaylistSearch, L'f', kMacCmd}, {C::TogglePlaylistPanel, L'l', kMacCmd | kMacShift},
        {C::ToggleControlPanel, L'e', kMacCmd | kMacOpt},
        {C::ToggleMiniPlayer, L'm', kMacCmd | kMacShift},
        {C::ToggleFullscreen, L'f', kMacCmd | kMacCtrl},
        {C::CycleAlwaysOnTop, L't', kMacCmd},
        {C::CycleSubtitleTrack, L'w', 0},
        {C::SubtitleDelayEarlier, L',', 0},     {C::SubtitleDelayLater, L'.', 0},
        {C::SubtitleDelayReset, L'/', 0},
    };
    return t;
}

const MacShortcut* MacShortcutFor(CommandId c) {
    for (const MacShortcut& s : MacShortcuts())
        if (s.command == c) return &s;
    return nullptr;
}

std::wstring MacShortcutText(const MacShortcut& s) {
    std::wstring out;
    if (s.mods & kMacCtrl) out += L"\u2303";
    if (s.mods & kMacOpt) out += L"\u2325";
    if (s.mods & kMacShift) out += L"\u21E7";
    if (s.mods & kMacCmd) out += L"\u2318";
    if (s.key == kLeft) out += L"\u2190";
    else if (s.key == kRight) out += L"\u2192";
    else if (s.key >= kF1 && s.key < kF1 + 12) out += L"F" + std::to_wstring(s.key - kF1 + 1);
    else out += (wchar_t)std::towupper(s.key);
    return out;
}

std::wstring MacMenuAccelerator(const std::wstring& label, const std::wstring& windowsText) {
    using C = CommandId;
    if (!windowsText.empty()) {
        for (const MacShortcut& s : MacShortcuts())
            if (ShortcutText(s.command) == windowsText) return MacShortcutText(s);
        return MacAccelerator(windowsText);
    }
    // Menus.cpp items without a Windows key (Alt+F4 / the native window menu there), by their label
    const std::pair<const wchar_t*, C> byLabel[] = {
        {Tr(L"Exit", L"종료"), C::Exit}, {Tr(L"Previous file", L"이전 파일"), C::Previous}, {Tr(L"Next file", L"다음 파일"), C::Next},
        {Tr(L"Mini player", L"미니 플레이어"), C::ToggleMiniPlayer}, {Tr(L"Exit mini player", L"미니 플레이어 종료"), C::ToggleMiniPlayer}};
    for (const auto& [text, c] : byLabel)
        if (label == text) if (const MacShortcut* s = MacShortcutFor(c)) return MacShortcutText(*s);
    return {};
}

namespace {
void MacAccelerators(std::vector<pui::MenuItemDesc>& items) {
    for (auto& i : items) {
        if (i.kind != pui::MenuItemKind::Separator && i.kind != pui::MenuItemKind::SubMenu)
            i.accelerator = MacMenuAccelerator(i.text, i.accelerator);
        MacAccelerators(i.sub);
    }
}

bool Show(std::vector<pui::MenuItemDesc> items, pui::RectF anchor, bool point, bool preferAbove, Callbacks cb) {
    g_last = items;
    MacAccelerators(items);
    if (OsDialogsBlocked() || !g_host) return false;
    auto m = std::make_unique<puimac::MenuPopup>(std::move(items));
    puimac::MenuPopup* raw = m.get();
    pui::SizeF sz = m->DesiredSize(g_measurer);
    pui::SizeF client = g_host->Size();
    float x, y;
    if (point) {
        x = anchor.l; y = anchor.t;
    } else {
        x = anchor.l;
        y = preferAbove ? anchor.t - 4 - sz.h : anchor.b + 4;
        if (preferAbove && y < 4 && anchor.b + 4 + sz.h <= client.h - 4) y = anchor.b + 4;
        if (!preferAbove && y + sz.h > client.h - 4 && anchor.t - 4 - sz.h >= 4) y = anchor.t - 4 - sz.h;
    }
    x = std::clamp(x, 4.f, std::max(4.f, client.w - sz.w - 4));
    y = std::clamp(y, 4.f, std::max(4.f, client.h - sz.h - 4));
    m->SetBounds(pui::RectF::XYWH(x, y, sz.w, sz.h));
    pui::PopupOptions o;
    o.lightDismiss = true;
    o.takeFocus = true;
    auto onClose = std::move(cb.onClose);
    o.onClosed = [onClose] { g_open = std::max(0, g_open - 1); if (onClose) onClose(); };
    if (cb.onOpen) cb.onOpen();
    g_open++;
    g_host->OpenPopup(std::move(m), std::move(o));
    raw->Raise(pui::UiEvent::MenuOpened);
    return true;   // the item action runs from the popup; "chosen" is not known synchronously on macOS
}
}

void SetMacHost(pui::UiHost* host, pui::IRenderer* measurer) { g_host = host; g_measurer = measurer; }

void SetOwner(HWND owner) { g_owner = owner; }
HWND Owner() { return g_owner; }
void ApplyTheme(pui::ThemeMode) {}
bool IsOpen() { return g_open > 0; }

bool ShowAtPoint(std::vector<pui::MenuItemDesc> items, pui::PointF clientDip, Callbacks cb) {
    return Show(std::move(items), {clientDip.x, clientDip.y, clientDip.x, clientDip.y}, true, false, std::move(cb));
}

bool ShowBelow(std::vector<pui::MenuItemDesc> items, const pui::RectF& anchorDip, bool preferAbove, Callbacks cb) {
    return Show(std::move(items), anchorDip, false, preferAbove, std::move(cb));
}

const std::vector<pui::MenuItemDesc>& LastShown() { return g_last; }
void ClearLastShown() { g_last.clear(); }

const pui::MenuItemDesc* FindItem(const std::vector<pui::MenuItemDesc>& items, const std::wstring& text) {
    for (const auto& i : items) {
        if (i.text == text) return &i;
        if (const auto* s = FindItem(i.sub, text)) return s;
    }
    return nullptr;
}

bool InvokeItem(std::vector<pui::MenuItemDesc>& items, const std::wstring& text) {
    for (auto& i : items) {
        if (i.text == text) {
            if (!i.enabled) return false;
            if (i.kind == pui::MenuItemKind::Check) { i.checked = !i.checked; if (i.onToggled) i.onToggled(i.checked); }
            if (i.onInvoke) i.onInvoke();
            return true;
        }
        if (InvokeItem(i.sub, text)) return true;
    }
    return false;
}

}
