#pragma once
// pui-drawn menu (context menus stay pui on macOS; only the menu BAR is native). Built from the same pui::MenuItemDesc
// descriptors the Windows app feeds to its native menus. Look: 10 pt flyout surface, 22 pt rows, 5 pt inset, SF Symbol
// glyph column, check / radio marks, right-aligned accelerators, submenus open to the right on hover or Right/Enter.
// A menu chain behaves like the native menus: while a submenu is open the pointer still reaches every menu of the chain
// (the host's light-dismiss hit test stops at the top popup, so the submenu answers for its parents), hovering another
// parent item moves the highlight there and switches / closes the submenu after the hover delay, clicking a parent
// item works, Left / Escape close one level (Escape on the root menu closes it).
// Placement keeps every menu inside the host's menu area (MenuArea): by default the client area; the Mac app shows its
// menus in their own windows (mac/app MenuWindowsMac) and sets the visible frame of the screen there, so a menu leaves a
// small window like a native one and flips left / above at the screen edges.
#include <functional>
#include <vector>

#include "pui/Menu.h"
#include "pui/Widget.h"

namespace puimac {

class MenuPopup : public pui::Widget {
public:
    static constexpr float kRowH = 22, kSepH = 11, kPadV = 5, kPadH = 5, kTextL = 28, kMinW = 160;
    explicit MenuPopup(std::vector<pui::MenuItemDesc> items, MenuPopup* parentMenu = nullptr);

    pui::SizeF DesiredSize(pui::IRenderer* m) override;
    void Paint(pui::IRenderer& r, const pui::Theme& t) override;
    bool OnPointerDown(const pui::PointerEvent& e) override;
    bool OnPointerMove(const pui::PointerEvent& e) override;
    bool OnPointerUp(const pui::PointerEvent& e) override;
    void OnPointerLeave() override;
    bool OnKeyDown(int vk, unsigned mods) override;
    bool OnTick(uint64_t nowMs) override;
    pui::Widget* HitTest(pui::PointF p) override;

    int Highlight() const { return highlight_; }
    int ItemCount() const { return (int)items_.size(); }
    const std::vector<pui::MenuItemDesc>& Items() const { return items_; }
    pui::RectF RowRect(int i) const;
    MenuPopup* SubMenu() const;                         // the open submenu (nullptr: none)
    int SubMenuIndex() const { return sub_ ? subIndex_ : -1; }

    /// Shown in a window of its own (mac/app MenuWindowsMac): the host's Paint skips it and the window paints it with
    /// PaintInWindow. Input still goes through the host (the window forwards it in host coordinates).
    void SetOwnWindow(bool on) { ownWindow_ = on; }
    bool OwnWindow() const { return ownWindow_; }
    void PaintInWindow(pui::IRenderer& r, const pui::Theme& t);

private:
    int IndexAt(pui::PointF p) const;
    void SetHighlight(int i);            // keyboard: immediate, closes a submenu of another item
    void HoverItem(int i);               // pointer: highlight now, open / switch / close the submenu after the delay
    void KeepSubMenu();                  // the pointer reached the open submenu: its item stays highlighted
    bool IsSubMenuItem(int i) const;
    void FocusSubMenuFirst();
    void Activate(int i);
    void OpenSubMenu(int i);
    void CloseSubMenu();
    void CloseChain();
    void MoveHighlight(int dir);

    std::vector<pui::MenuItemDesc> items_;
    MenuPopup* parent_;
    pui::Widget* sub_ = nullptr;   // open submenu popup (owned by the host)
    int subIndex_ = -1, highlight_ = -1, pendingSub_ = -1;
    bool pending_ = false;               // a hover change waits for the delay (pendingSub_ = the hovered item, may be -1)
    uint64_t pendingSince_ = 0;
    bool ownWindow_ = false, paintingInWindow_ = false;
};

/// The rectangle (host client DIPs) the menus of `host` are kept inside, for a menu placed at `near`: the function set
/// with SetMenuArea, else the client area.
pui::RectF MenuArea(pui::UiHost& host, pui::PointF near);
/// Sets (or clears, with an empty function) the menu area of `host`.
void SetMenuArea(pui::UiHost* host, std::function<pui::RectF(pui::PointF near)> area);
/// A menu of `size` with its top-left at `p`; where it would leave `area` it opens to the left of / above `p` (a macOS
/// context menu), then it is kept inside. Whole-DIP origin.
pui::RectF PlaceMenuAtPoint(const pui::RectF& area, pui::PointF p, pui::SizeF size);
/// A menu of `size` under `anchor` (above it when `preferAbove`), left-aligned; it takes the other side when its side
/// does not fit, right-aligns with the anchor when it would leave the area on the right, then it is kept inside.
pui::RectF PlaceMenuBeside(const pui::RectF& area, const pui::RectF& anchor, pui::SizeF size, bool preferAbove);

/// Opens the menu with its top-left at `at` (PlaceMenuAtPoint in the menu area). `focusReturn` gets the focus back on close.
MenuPopup* ShowMenu(pui::UiHost& host, std::vector<pui::MenuItemDesc> items, pui::PointF at,
                    pui::IRenderer* measurer = nullptr, pui::Widget* focusReturn = nullptr);
/// Opens the menu below `anchor` (PlaceMenuBeside in the menu area).
MenuPopup* ShowMenu(pui::UiHost& host, std::vector<pui::MenuItemDesc> items, pui::Widget* anchor,
                    pui::IRenderer* measurer = nullptr);

}
