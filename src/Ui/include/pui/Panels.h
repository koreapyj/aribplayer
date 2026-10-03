#pragma once
// After adding/removing children of a StackPanel call RequestLayout() (or SetBounds on the root).
#include <string>

#include "Buttons.h"

namespace pui {

struct Thickness {
    float l = 0, t = 0, r = 0, b = 0;
    static Thickness All(float v) { return {v, v, v, v}; }
    static Thickness HV(float h, float v) { return {h, v, h, v}; }
    float Horizontal() const { return l + r; }
    float Vertical() const { return t + b; }
};

enum class TextColorRole { Primary, Secondary, Tertiary, Accent, Disabled };
enum class TextKind { Body, BodyStrong, Caption, Subtitle, Title };
enum class Orientation { Vertical, Horizontal };
enum class CrossAlign { Stretch, Start, Center, End };

class TextBlock : public Widget, public IHeightForWidth {
public:
    explicit TextBlock(std::wstring text = {}, TextKind kind = TextKind::Body);
    void SetText(std::wstring t);
    const std::wstring& Text() const { return text_; }
    void SetKind(TextKind k);                  // sets size/weight to the Fluent type ramp entry
    void SetFontSize(float s) { size_ = s; Changed(); }
    void SetColorRole(TextColorRole c) { color_ = c; Invalidate(); }
    void SetWrap(bool w) { wrap_ = w; Changed(); }
    void SetAlign(TextAlign a) { align_ = a; Invalidate(); }
    float FontSize() const { return size_; }
    bool Wrap() const { return wrap_; }
    TextStyle Style() const;

    SizeF DesiredSize(IRenderer* m) override;
    float HeightForWidth(IRenderer* m, float width) override;
    void Paint(IRenderer& r, const Theme& t) override;
private:
    void Changed() { RelayoutAncestors(*this); Invalidate(); }
    std::wstring text_;
    float size_ = 14;
    int weight_ = 400;
    TextColorRole color_ = TextColorRole::Primary;
    bool wrap_ = false;
    TextAlign align_ = TextAlign::Leading;
};

/// Fixed or flexible gap. A flexible spacer takes an equal share of the stack's remaining main-axis space.
class Spacer : public Widget {
public:
    explicit Spacer(float w = 0, float h = 0, bool flex = false) : w_(w), h_(h), flex_(flex) { SetHitTestVisible(false); }
    void SetSize(float w, float h) { w_ = w; h_ = h; RelayoutAncestors(*this); }
    bool Flex() const { return flex_; }
    SizeF DesiredSize(IRenderer*) override { return {w_, h_}; }
private:
    float w_, h_;
    bool flex_;
};

/// 1 DIP separator line (horizontal by default; stretches across the stack's cross axis).
class Divider : public Widget {
public:
    explicit Divider(bool vertical = false) : Widget(Role::Separator), vertical_(vertical) { SetHitTestVisible(false); }
    SizeF DesiredSize(IRenderer*) override { return vertical_ ? SizeF{1, 0} : SizeF{0, 1}; }
    void Paint(IRenderer& r, const Theme& t) override;
    bool Vertical() const { return vertical_; }
private:
    bool vertical_;
};

class StackPanel : public Widget, public IHeightForWidth {
public:
    explicit StackPanel(Orientation o = Orientation::Vertical, float spacing = 0, Role role = Role::Group);
    void SetSpacing(float s) { spacing_ = s; RequestLayout(); }
    void SetPadding(Thickness p) { pad_ = p; RequestLayout(); }
    Thickness Padding() const { return pad_; }
    void SetCrossAlign(CrossAlign a) { cross_ = a; RequestLayout(); }

    void OnLayout() override;
    SizeF DesiredSize(IRenderer* m) override;
    float HeightForWidth(IRenderer* m, float width) override;
private:
    const Orientation orient_;
    float spacing_;
    Thickness pad_{};
    CrossAlign cross_ = CrossAlign::Stretch;
};

/// StackPanel with card chrome (CardFill + CardStroke, radius 8, padding 16).
class Card : public StackPanel {
public:
    explicit Card(Orientation o = Orientation::Vertical, float spacing = 8);
    void Paint(IRenderer& r, const Theme& t) override;
};

}
