#pragma once
#include <functional>
#include <string>

#include "Buttons.h"

namespace pui {

enum class InfoBarSeverity { Informational, Success, Warning, Error };

class InfoBar : public Widget, public IHeightForWidth {
public:
    static constexpr float kMinHeight = 48, kPadV = 14, kTextLeft = 44, kIconX = 16, kCloseSize = 32;
    InfoBar();

    void SetSeverity(InfoBarSeverity s) { severity_ = s; Invalidate(); }
    InfoBarSeverity Severity() const { return severity_; }
    void SetTitle(std::wstring t);
    const std::wstring& Title() const { return title_; }
    void SetMessage(std::wstring m);
    const std::wstring& Message() const { return message_; }
    /// Shows an action button with this text (empty hides it).
    void SetActionText(std::wstring text);
    void SetClosable(bool c);
    void SetOpen(bool open);                 // IsOpen=false hides the bar (VisibilityChanged)
    bool IsOpen() const { return open_; }
    Button* ActionButton() const { return action_; }
    IconButton* CloseButton() const { return close_; }
    static wchar_t SeverityGlyph(InfoBarSeverity s);
    Color SeverityColor(const Theme& t) const;
    Color BackgroundColor(const Theme& t) const;

    std::function<void()> onClosed;          // user closed the bar (close button)
    std::function<void()> onAction;

    float HeightForWidth(IRenderer* m, float width) override;
    SizeF DesiredSize(IRenderer* m) override;
    void OnLayout() override;
    void Paint(IRenderer& r, const Theme& t) override;

private:
    float TextWidth(float width) const;
    void Changed() { RelayoutAncestors(*this); RequestLayout(); }
    InfoBarSeverity severity_ = InfoBarSeverity::Informational;
    std::wstring title_, message_;
    bool closable_ = true, open_ = true;
    Button* action_ = nullptr;
    IconButton* close_ = nullptr;
};

}
