#include "features/NativeMenu.h"

#include <cmath>

#include "features/DialogGuard.h"

namespace app::nativemenu {
namespace {

HWND g_owner = nullptr;
bool g_open = false;
std::vector<pui::MenuItemDesc> g_last;

// uxtheme ordinals (undocumented, used by every dark-mode-aware Win32 app): 135 SetPreferredAppMode, 136 FlushMenuThemes.
enum PreferredAppMode { kDefault = 0, kAllowDark = 1, kForceDark = 2, kForceLight = 3 };
using SetPreferredAppModeFn = int(WINAPI*)(int);
using FlushMenuThemesFn = void(WINAPI*)();
struct UxApi {
    SetPreferredAppModeFn setMode = nullptr;
    FlushMenuThemesFn flush = nullptr;
};
const UxApi& Ux() {
    static const UxApi api = [] {
        UxApi a;
        if (HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
            a.setMode = reinterpret_cast<SetPreferredAppModeFn>(GetProcAddress(ux, MAKEINTRESOURCEA(135)));
            a.flush = reinterpret_cast<FlushMenuThemesFn>(GetProcAddress(ux, MAKEINTRESOURCEA(136)));
        }
        return a;
    }();
    return api;
}

std::wstring Label(const pui::MenuItemDesc& d) {
    std::wstring s;
    for (wchar_t c : d.text) { s += c; if (c == L'&') s += L'&'; }   // '&' is the mnemonic marker
    if (!d.accelerator.empty()) { s += L'\t'; s += d.accelerator; }
    return s;
}

/// Builds the HMENU of `items`; item i of the flattened table gets id = index + 1 (sub-menus have no id).
HMENU Build(std::vector<pui::MenuItemDesc>& items, std::vector<pui::MenuItemDesc*>& table) {
    HMENU menu = CreatePopupMenu();
    if (!menu) return nullptr;
    for (auto& d : items) {
        MENUITEMINFOW mi{sizeof mi};
        std::wstring text;
        if (d.kind == pui::MenuItemKind::Separator) {
            mi.fMask = MIIM_FTYPE;
            mi.fType = MFT_SEPARATOR;
        } else {
            text = Label(d);
            mi.fMask = MIIM_FTYPE | MIIM_STATE | MIIM_STRING;
            mi.fType = MFT_STRING | (d.kind == pui::MenuItemKind::Radio ? MFT_RADIOCHECK : 0);
            mi.fState = (d.enabled ? MFS_ENABLED : MFS_GRAYED) | (d.checked && (d.kind == pui::MenuItemKind::Check || d.kind == pui::MenuItemKind::Radio) ? MFS_CHECKED : 0);
            mi.dwTypeData = text.data();
            if (d.kind == pui::MenuItemKind::SubMenu) {
                mi.fMask |= MIIM_SUBMENU;
                mi.hSubMenu = Build(d.sub, table);
                if (!d.enabled || d.sub.empty()) mi.fState = MFS_GRAYED;
            } else {
                mi.fMask |= MIIM_ID;
                table.push_back(&d);
                mi.wID = (UINT)table.size();
            }
        }
        InsertMenuItemW(menu, GetMenuItemCount(menu), TRUE, &mi);
    }
    return menu;
}

void RunItem(pui::MenuItemDesc& d) {
    if (!d.enabled) return;
    if (d.kind == pui::MenuItemKind::Check) {
        d.checked = !d.checked;
        if (d.onToggled) d.onToggled(d.checked);
    } else if (d.onInvoke) {
        d.onInvoke();
    }
}

POINT ClientToScreenPx(const pui::PointF& dip) {
    const float scale = (float)GetDpiForWindow(g_owner) / 96.f;
    POINT p{(LONG)std::lround(dip.x * scale), (LONG)std::lround(dip.y * scale)};
    ClientToScreen(g_owner, &p);
    return p;
}

bool Track(std::vector<pui::MenuItemDesc> items, const pui::PointF* pt, const pui::RectF* anchor, bool above, Callbacks cb) {
    if (items.empty()) return false;
    if (OsDialogsBlocked() || !g_owner || !IsWindow(g_owner)) {   // tests: record, never show
        g_last = std::move(items);
        return false;
    }
    if (g_open) return false;
    std::vector<pui::MenuItemDesc> tree = std::move(items);
    std::vector<pui::MenuItemDesc*> table;
    HMENU menu = Build(tree, table);
    if (!menu) return false;

    UINT flags = TPM_RETURNCMD | TPM_RIGHTBUTTON;
    TPMPARAMS params{sizeof params};
    TPMPARAMS* pParams = nullptr;
    POINT at{};
    if (anchor) {
        const POINT tl = ClientToScreenPx({anchor->l, anchor->t}), br = ClientToScreenPx({anchor->r, anchor->b});
        params.rcExclude = RECT{tl.x, tl.y, br.x, br.y};
        pParams = &params;
        flags |= TPM_VERTICAL | TPM_LEFTALIGN | (above ? TPM_BOTTOMALIGN : TPM_TOPALIGN);
        at = {tl.x, above ? tl.y : br.y};
    } else {
        flags |= TPM_LEFTALIGN | TPM_TOPALIGN;
        at = ClientToScreenPx(*pt);
    }

    g_open = true;
    if (cb.onOpen) cb.onOpen();
    SetForegroundWindow(g_owner);   // MSDN: otherwise the menu does not close when the user clicks elsewhere
    const UINT id = (UINT)TrackPopupMenuEx(menu, flags, at.x, at.y, g_owner, pParams);
    PostMessageW(g_owner, WM_NULL, 0, 0);
    DestroyMenu(menu);
    g_open = false;
    if (cb.onClose) cb.onClose();
    if (id == 0 || id > table.size()) return false;
    RunItem(*table[id - 1]);   // after the menu closed: the action may open dialogs / other menus
    return true;
}

pui::MenuItemDesc* Find(std::vector<pui::MenuItemDesc>& items, const std::wstring& text) {
    for (auto& d : items) {
        if (d.kind == pui::MenuItemKind::Separator) continue;
        if (d.text == text) return &d;
        if (d.kind == pui::MenuItemKind::SubMenu)
            if (auto* r = Find(d.sub, text)) return r;
    }
    return nullptr;
}

}

void SetOwner(HWND owner) { g_owner = owner; }
HWND Owner() { return g_owner; }

void ApplyTheme(pui::ThemeMode mode) {
    const UxApi& ux = Ux();
    if (!ux.setMode) return;   // unavailable: menus keep the light system style
    ux.setMode(mode == pui::ThemeMode::Dark ? kForceDark : mode == pui::ThemeMode::Light ? kForceLight : kAllowDark);
    if (ux.flush) ux.flush();
}

bool IsOpen() { return g_open; }

bool ShowAtPoint(std::vector<pui::MenuItemDesc> items, pui::PointF clientDip, Callbacks cb) {
    return Track(std::move(items), &clientDip, nullptr, false, std::move(cb));
}

bool ShowBelow(std::vector<pui::MenuItemDesc> items, const pui::RectF& anchorDip, bool preferAbove, Callbacks cb) {
    return Track(std::move(items), nullptr, &anchorDip, preferAbove, std::move(cb));
}

const std::vector<pui::MenuItemDesc>& LastShown() { return g_last; }
void ClearLastShown() { g_last.clear(); }

const pui::MenuItemDesc* FindItem(const std::vector<pui::MenuItemDesc>& items, const std::wstring& text) {
    return Find(const_cast<std::vector<pui::MenuItemDesc>&>(items), text);
}

bool InvokeItem(std::vector<pui::MenuItemDesc>& items, const std::wstring& text) {
    pui::MenuItemDesc* d = Find(items, text);
    if (!d || !d->enabled || d->kind == pui::MenuItemKind::SubMenu) return false;
    RunItem(*d);
    return true;
}

}
