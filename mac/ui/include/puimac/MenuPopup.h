#pragma once
// pui-drawn menu (context menus stay pui on macOS; only the menu BAR is native). Built from the same pui::MenuItemDesc
// descriptors the Windows app feeds to its native menus. Look: 10 pt flyout surface, 22 pt rows, 5 pt inset, SF Symbol
// glyph column, check / radio marks, right-aligned accelerators, submenus open to the right on hover or Right/Enter.
// A menu chain behaves like the native menus: while a submenu is open the pointer still reaches every menu of the chain
// (the host's light-dismiss hit test stops at the top popup, so the submenu answers for its parents), hovering another
// parent item moves the highlight there and switches / closes the submenu after the hover delay, clicking a parent
// item works, Left / Escape close one level (Escape on the root menu closes it).
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
};

/// Opens the menu with its top-left at `at` (clamped to the client). `focusReturn` gets the focus back on close.
MenuPopup* ShowMenu(pui::UiHost& host, std::vector<pui::MenuItemDesc> items, pui::PointF at,
                    pui::IRenderer* measurer = nullptr, pui::Widget* focusReturn = nullptr);
/// Opens the menu below `anchor` (left-aligned), flipping above when it does not fit.
MenuPopup* ShowMenu(pui::UiHost& host, std::vector<pui::MenuItemDesc> items, pui::Widget* anchor,
                    pui::IRenderer* measurer = nullptr);

}
