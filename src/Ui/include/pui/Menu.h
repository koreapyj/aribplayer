#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "Buttons.h"

namespace pui {

enum class Placement { Auto, Top, Bottom, Left, Right };

/// Rectangle for a flyout of `size` around `anchor`. The preferred side is tried first; when it does not fit the
/// opposite side is used (Auto tries Bottom, Top, Right, Left). The cross axis is centred on the anchor. The result is
/// always clamped inside the client with `margin`. `actual` receives the side that was used.
RectF PlaceFlyout(SizeF size, const RectF& anchor, Placement preferred, SizeF client, float gap = 8, float margin = 4,
                  Placement* actual = nullptr);


/// Menu item descriptors: the data the app's native Win32 menus are built from (app/features/NativeMenu).
enum class MenuItemKind { Item, Check, Radio, Separator, SubMenu };

struct MenuItemDesc {
    MenuItemKind kind = MenuItemKind::Item;
    std::wstring text, accelerator;
    wchar_t glyph = 0;
    bool enabled = true, checked = false;
    std::function<void()> onInvoke;            // Item, Radio (and Check, after toggling)
    std::function<void(bool)> onToggled;
    std::vector<MenuItemDesc> sub;

    static MenuItemDesc Item(std::wstring text, std::function<void()> cb = {}, std::wstring accel = {}, wchar_t glyph = 0) {
        MenuItemDesc d; d.text = std::move(text); d.onInvoke = std::move(cb); d.accelerator = std::move(accel); d.glyph = glyph; return d;
    }
    static MenuItemDesc Check(std::wstring text, bool checked, std::function<void(bool)> cb = {}, std::wstring accel = {}) {
        MenuItemDesc d; d.kind = MenuItemKind::Check; d.text = std::move(text); d.checked = checked; d.onToggled = std::move(cb);
        d.accelerator = std::move(accel); return d;
    }
    static MenuItemDesc Radio(std::wstring text, bool checked, std::function<void()> cb = {}) {
        MenuItemDesc d; d.kind = MenuItemKind::Radio; d.text = std::move(text); d.checked = checked; d.onInvoke = std::move(cb); return d;
    }
    static MenuItemDesc Separator() { MenuItemDesc d; d.kind = MenuItemKind::Separator; return d; }
    static MenuItemDesc Sub(std::wstring text, std::vector<MenuItemDesc> items, wchar_t glyph = 0) {
        MenuItemDesc d; d.kind = MenuItemKind::SubMenu; d.text = std::move(text); d.sub = std::move(items); d.glyph = glyph; return d;
    }
    MenuItemDesc& Disabled() { enabled = false; return *this; }
};

/// Generic container popup (volume flyout etc.): content + padding on a flyout surface, 8 DIP radius.
class Flyout : public Widget {
public:
    Flyout();
    template <class T, class... A> T* SetContentNew(A&&... args) {
        auto p = std::make_unique<T>(std::forward<A>(args)...);
        T* raw = p.get();
        SetContent(std::move(p));
        return raw;
    }
    Widget* SetContent(std::unique_ptr<Widget> content);
    Widget* Content() const { return content_; }
    void SetPadding(float p) { padding_ = p; RequestLayout(); }
    float Padding() const { return padding_; }
    Placement ActualPlacement() const { return actual_; }
    void Hide();
    std::function<void()> onClosed;

    SizeF DesiredSize(IRenderer* m) override;
    void OnLayout() override;
    void Paint(IRenderer& r, const Theme& t) override;

    static Flyout* ShowAt(UiHost& host, std::unique_ptr<Flyout> f, Widget* anchor, Placement p = Placement::Auto,
                          IRenderer* measurer = nullptr);
    static Flyout* ShowAt(UiHost& host, std::unique_ptr<Flyout> f, const RectF& anchorRect, Placement p = Placement::Auto,
                          IRenderer* measurer = nullptr, Widget* focusReturn = nullptr);
private:
    Widget* content_ = nullptr;
    float padding_ = 0;
    Placement actual_ = Placement::Auto;
};

}
