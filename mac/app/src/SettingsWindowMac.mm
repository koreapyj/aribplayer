#import "SettingsWindowMac.h"

#import <AppKit/AppKit.h>
#import <CoreText/CoreText.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <optional>

#include "CaptionStyleMac.h"
#include "Localize.h"
#include "LoggingMac.h"
#include "MainView.h"
#include "PopupDismissMac.h"
#include "PuiHostView.h"
#include "features/FiltersPanel.h"
#include "features/SettingsLogic.h"
#include "features/SettingsService.h"
#include "pages/SettingsPage.h"
#include "pui/Buttons.h"
#include "pui/ComboBox.h"
#include "pui/Panels.h"
#include "pui/ScrollViewer.h"
#include "pui/TextBox.h"
#include "pui/Toggles.h"
#include "pui/ToolTipStyle.h"
#include "pui/Widget.h"
#include "puimac/CGRenderer.h"
#include "wvp/win32/Utf.h"

@interface SettingsWindowDelegate : NSObject <NSWindowDelegate>
@property(nonatomic, assign) mainview::SettingsWindow* owner;
@end

@implementation SettingsWindowDelegate
- (BOOL)windowShouldClose:(NSWindow*)w { (void)w; if (_owner) _owner->Close(); return NO; }
@end

namespace mainview {

namespace {
constexpr CGFloat kDefaultW = 900, kDefaultH = 700, kMinW = 480, kMinH = 360;   // SettingsWindow::kDefault* / kMin* (DIP)
constexpr float kTitleBarDip = 28;   // the transparent title bar (traffic lights) above the page

/// The window's root: the page below the title bar area.
class Root final : public pui::Widget {
public:
    Root() : pui::Widget(pui::Role::Pane) {}
    pui::Widget* page = nullptr;
    void OnLayout() override {
        if (page) page->SetBounds({Bounds().l, Bounds().t + kTitleBarDip, Bounds().r, Bounds().b});
    }
};

pui::Widget* FindById(pui::Widget* w, const std::wstring& id) {
    if (!w) return nullptr;
    if (w->AutomationId() == id) return w;
    for (auto& c : w->Children())
        if (pui::Widget* f = FindById(c.get(), id)) return f;
    return nullptr;
}

/// A settings card with the page's row geometry (sl::ComputeRowLayout) and painting: the Mac counterpart of
/// SettingsPage.cpp's SettingRow (file-local there) for the rows whose description names Windows places.
class MacSettingRow final : public pui::Widget, public pui::IHeightForWidth {
public:
    MacSettingRow(wchar_t glyph, std::wstring title, std::wstring desc)
        : pui::Widget(pui::Role::Group), glyph_(glyph), title_(std::move(title)), desc_(std::move(desc)) {
        SetName(title_);
        SetHelpText(desc_);
        SetClipChildren(false);
    }
    void SetControl(std::unique_ptr<pui::Widget> c) {
        control_ = c.get();
        AddChild(std::move(c));
    }
    const std::wstring& Description() const { return desc_; }
    pui::SizeF DesiredSize(pui::IRenderer* m) override { return {0, Compute(Bounds().w(), m).height}; }
    float HeightForWidth(pui::IRenderer* m, float width) override { return Compute(width, m).height; }
    void OnLayout() override {
        layout_ = Compute(Bounds().w(), nullptr);
        if (control_) control_->SetBounds(layout_.control.Offset(Bounds().l, Bounds().t));
    }
    void Paint(pui::IRenderer& r, const pui::Theme& t) override {
        const pui::RectF b = Bounds();
        r.FillRoundRect(b, t.overlayRadius, t.cardFill);
        r.StrokeRoundRect(b.Deflate(0.5f, 0.5f, 0.5f, 0.5f), t.overlayRadius, t.cardStroke, 1);
        if (glyph_) r.Text(std::wstring(1, glyph_), layout_.glyph.Offset(b.l, b.t), pui::IconTextStyle(16), t.textPrimary);
        r.Text(title_, layout_.title.Offset(b.l, b.t), pui::BodyTextStyle(14), t.textPrimary);
        if (!desc_.empty()) r.Text(desc_, layout_.desc.Offset(b.l, b.t), DescStyle(), t.textSecondary);
    }

private:
    static pui::TextStyle DescStyle() {
        pui::TextStyle s = pui::BodyTextStyle(12);
        s.wrap = true;
        s.ellipsis = false;
        s.valign = pui::ParaAlign::Top;
        return s;
    }
    app::sl::RowLayoutResult Compute(float width, pui::IRenderer* m) {
        pui::IRenderer& meas = pui::MeasurerOr(m);
        app::sl::RowInput in;
        in.width = width;
        in.hasGlyph = glyph_ != 0;
        const float textLeft = app::sl::kRowPad + (in.hasGlyph ? app::sl::kGlyphCol : 0.f);
        const pui::SizeF cd = control_ ? control_->DesiredSize(&meas) : pui::SizeF{};
        in.controlW = cd.w;
        in.controlH = cd.h;
        const float textW = std::max(0.f, width - app::sl::kRowPad - cd.w - 16.f - textLeft);
        if (!desc_.empty()) in.descH = std::max(16.f, meas.MeasureText(desc_, DescStyle(), textW).h);
        return app::sl::ComputeRowLayout(in);
    }
    wchar_t glyph_;
    std::wstring title_, desc_;
    pui::Widget* control_ = nullptr;
    app::sl::RowLayoutResult layout_{};
};

/// Replaces the page's row `row` (same place, same automation id) by a MacSettingRow with the Mac wording; the row's
/// control (and its handlers into the page) moves over unchanged.
void ReplaceRow(pui::Widget* row, wchar_t glyph, const wchar_t* title, const std::wstring& desc) {
    pui::Widget* parent = row ? row->Parent() : nullptr;
    if (!parent || row->Children().empty()) return;
    int index = 0;
    for (const auto& c : parent->Children()) { if (c.get() == row) break; index++; }
    auto mac = std::make_unique<MacSettingRow>(glyph, title, desc);
    mac->SetAutomationId(row->AutomationId());
    mac->SetControl(row->RemoveChild(row->Children().front().get()));
    parent->RemoveChild(row);
    parent->AddChild(std::move(mac), index);
}

std::vector<std::wstring> FontFamilies(const std::wstring& current) {
    std::vector<std::wstring> names;
    if (CFArrayRef families = CTFontManagerCopyAvailableFontFamilyNames()) {
        for (CFIndex i = 0; i < CFArrayGetCount(families); ++i) {
            NSString* family = (__bridge NSString*)CFArrayGetValueAtIndex(families, i);
            if (family.length && ![family hasPrefix:@"."])
                names.push_back(wvp::ToWide(family.UTF8String));
        }
        CFRelease(families);
    }
    // A saved family stays selectable even when its font is not installed on this Mac.
    if (!current.empty()) names.push_back(current);
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    return names;
}

std::optional<std::uint32_t> ParseRgb(const std::wstring& text) {
    const size_t start = !text.empty() && text[0] == L'#' ? 1 : 0;
    if (text.size() != start + 6) return std::nullopt;
    std::uint32_t rgb = 0;
    for (size_t i = start; i < text.size(); ++i) {
        const wchar_t c = text[i];
        int digit = c >= L'0' && c <= L'9' ? c - L'0' : c >= L'A' && c <= L'F' ? c - L'A' + 10 :
                    c >= L'a' && c <= L'f' ? c - L'a' + 10 : -1;
        if (digit < 0) return std::nullopt;
        rgb = (rgb << 4) | static_cast<unsigned>(digit);
    }
    return rgb;
}

std::optional<double> ParsePoints(const std::wstring& text) {
    if (text.empty()) return std::nullopt;
    double points = 0;
    bool decimal = false;
    int digits = 0, fractionDigits = 0;
    for (wchar_t c : text) {
        if (c == L'.' && !decimal && digits > 0) { decimal = true; continue; }
        if (c < L'0' || c > L'9') return std::nullopt;
        if (decimal) {
            if (++fractionDigits > 1) return std::nullopt;
            points += (c - L'0') / 10.0;
        } else {
            points = points * 10 + (c - L'0');
            ++digits;
        }
        if (points > 400) return std::nullopt;
    }
    if (points < 4 || (decimal && fractionDigits == 0)) return std::nullopt;
    return points;
}

class PickerSwatch final : public pui::Button {
public:
    explicit PickerSwatch(std::uint32_t rgb) : rgb_(rgb) {
        SetName(app::sl::RgbaHex(rgb << 8));
        SetToolTip(Name());
    }
    pui::SizeF DesiredSize(pui::IRenderer*) override { return {28, 28}; }
    void Paint(pui::IRenderer& r, const pui::Theme& t) override {
        auto b = Bounds().Deflate(3, 3, 3, 3);
        r.FillRoundRect(b, 4, pui::Color(0xFF000000u | rgb_));
        r.StrokeRoundRect(b, 4, t.textSecondary, 1);
        PaintFocusVisual(r, t, Bounds(), 4);
    }
private:
    std::uint32_t rgb_;
};

class SubtitleStyleDialog final : public pui::Widget {
public:
    SubtitleStyleDialog(bool font, const wvp::SubtitleStyle& current,
                        std::function<void(const wvp::SubtitleStyle&)> apply)
        : pui::Widget(pui::Role::Pane), font_(font), original_(current), apply_(std::move(apply)) {
        SetName(font ? app::Tr(L"Subtitle font", L"자막 글꼴") : app::Tr(L"Outline colour", L"외곽선 색"));
        SetAutomationId(font ? L"SubtitleFontDialog" : L"SubtitleColorDialog");
        title_ = Add<pui::TextBlock>(Name(), pui::TextKind::Subtitle);
        scroll_ = Add<pui::ScrollViewer>();
        scroll_->SetAutomationId(L"SubtitlePickerScroll");
        fields_ = scroll_->EmplaceContent<pui::StackPanel>(pui::Orientation::Vertical, 8.f);
        fields_->SetPadding({0, 0, 12, 8});
        if (font) {
            Label(app::Tr(L"Font family", L"글꼴 모음"));
            family_ = fields_->Add<pui::ComboBox>();
            family_->SetName(app::Tr(L"Font family", L"글꼴 모음"));
            family_->SetAutomationId(L"SubtitleFontFamilyCombo");
            const auto currentFamily = wvp::ToWide(current.font);
            auto names = FontFamilies(currentFamily);
            auto found = std::find(names.begin(), names.end(), currentFamily);
            const int index = found == names.end() ? -1 : static_cast<int>(found - names.begin());
            family_->SetItems(std::move(names));
            family_->SetSelectedIndex(index);
            family_->onSelectionChanged = [this](int) { Validate(); };

            Label(app::Tr(L"Font weight", L"글꼴 굵기"));
            weight_ = fields_->Add<pui::ComboBox>();
            weight_->SetName(app::Tr(L"Font weight", L"글꼴 굵기"));
            weight_->SetAutomationId(L"SubtitleFontWeightCombo");
            const wchar_t* labels[] = {
                app::Tr(L"Thin", L"매우 가늘게"), app::Tr(L"Extra light", L"아주 가늘게"), app::Tr(L"Light", L"가늘게"),
                app::Tr(L"Regular", L"보통"), app::Tr(L"Medium", L"중간"), app::Tr(L"Semibold", L"약간 굵게"),
                app::Tr(L"Bold", L"굵게"), app::Tr(L"Extra bold", L"아주 굵게"), app::Tr(L"Black", L"매우 굵게")};
            for (int i = 0; i < 9; ++i) {
                weights_.push_back((i + 1) * 100);
                weight_->AddItem(std::wstring(labels[i]) + L" (" + std::to_wstring(weights_.back()) + L")");
            }
            auto weightIt = std::find(weights_.begin(), weights_.end(), current.fontWeight);
            if (weightIt == weights_.end()) {
                weights_.push_back(current.fontWeight);
                weight_->AddItem(std::to_wstring(current.fontWeight));
                weight_->SetSelectedIndex(static_cast<int>(weights_.size()) - 1);
            } else weight_->SetSelectedIndex(static_cast<int>(weightIt - weights_.begin()));

            Label(app::Tr(L"Italic", L"기울임꼴"));
            italic_ = fields_->Add<pui::ToggleSwitch>();
            italic_->SetName(app::Tr(L"Italic", L"기울임꼴"));
            italic_->SetAutomationId(L"SubtitleFontItalicToggle");
            italic_->SetOn(current.italic);
            Label(app::Tr(L"Size in points (4–400, e.g. 36.5)", L"크기: 포인트 (4–400, 예: 36.5)"));
            size_ = fields_->Add<pui::TextBox>();
            size_->SetBounds(pui::RectF::XYWH(0, 0, 240, pui::TextBox::kHeight));
            size_->SetName(app::Tr(L"Font size in points", L"글꼴 크기: 포인트"));
            size_->SetAutomationId(L"SubtitleFontSizeTextBox");
            wchar_t points[32];
            swprintf_s(points, current.fontSizePt == std::floor(current.fontSizePt) ? L"%.0f" : L"%.1f", current.fontSizePt);
            size_->SetText(points);
            size_->onChanged = [this](const std::wstring&) { Validate(); };
            size_->onSubmit = [this](const std::wstring&) { Apply(); };
        }
        Label(font ? app::Tr(L"Text colour (#RRGGBB)", L"글자 색 (#RRGGBB)") : app::Tr(L"Outline colour (#RRGGBB)", L"외곽선 색 (#RRGGBB)"));
        color_ = fields_->Add<pui::TextBox>();
        color_->SetBounds(pui::RectF::XYWH(0, 0, 240, pui::TextBox::kHeight));
        color_->SetName(font ? app::Tr(L"Text colour", L"글자 색") : app::Tr(L"Outline colour", L"외곽선 색"));
        color_->SetAutomationId(L"SubtitleColorHexTextBox");
        color_->SetText(app::sl::RgbaHex(font ? current.primaryRgba : current.outlineRgba));
        color_->SetHelpText(app::Tr(L"Enter six hexadecimal digits. Existing opacity is kept.", L"16진수 여섯 자리를 입력합니다. 기존 불투명도를 유지합니다."));
        color_->onChanged = [this](const std::wstring&) { Validate(); };
        color_->onSubmit = [this](const std::wstring&) { Apply(); };
        auto* palette = fields_->Add<pui::StackPanel>(pui::Orientation::Horizontal, 6.f);
        palette->SetName(app::Tr(L"Colour presets", L"색 프리셋"));
        for (std::uint32_t rgb : {0xFFFFFFu, 0x000000u, 0xFF0000u, 0xFFFF00u, 0x00FF00u, 0x00FFFFu, 0x0000FFu, 0xFF00FFu}) {
            auto* swatch = palette->Add<PickerSwatch>(rgb);
            swatch->SetAutomationId(L"SubtitleColorPreset" + app::sl::RgbaHex(rgb << 8).substr(1));
            swatch->onClick = [this, rgb] { color_->SetText(app::sl::RgbaHex(rgb << 8)); Validate(); };
        }
        error_ = fields_->Add<pui::TextBlock>(L"", pui::TextKind::Caption);
        error_->SetAutomationId(L"SubtitlePickerValidation");
        error_->SetWrap(true);
        cancel_ = Add<pui::Button>(app::Tr(L"Cancel", L"취소"));
        cancel_->SetAutomationId(L"SubtitlePickerCancelButton");
        cancel_->onClick = [this] { Close(); };
        accept_ = Add<pui::Button>(app::Tr(L"Apply", L"적용"), pui::ButtonStyle::Accent);
        accept_->SetAutomationId(L"SubtitlePickerApplyButton");
        accept_->onClick = [this] { Apply(); };
        Validate();
    }
    void ScrollToFocus(pui::Widget* w) { if (fields_->IsAncestorOf(w)) scroll_->ScrollIntoView(w); }
    void OnLayout() override {
        // Child setters request ancestor layout while the dialog is still assembling its controls.
        if (!accept_ || Bounds().Empty()) return;
        const auto b = Bounds();
        constexpr float pad = 20;
        title_->SetBounds(pui::RectF::XYWH(b.l + pad, b.t + 16, b.w() - pad * 2, 32));
        const float bw = std::max({88.f, accept_->DesiredSize(nullptr).w, cancel_->DesiredSize(nullptr).w});
        accept_->SetBounds(pui::RectF::XYWH(b.r - pad - bw, b.b - 48, bw, 32));
        cancel_->SetBounds(pui::RectF::XYWH(b.r - pad - bw * 2 - 8, b.b - 48, bw, 32));
        const auto content = pui::RectF{b.l + pad, b.t + 60, b.r - pad, b.b - 60};
        scroll_->SetBounds(content);
        scroll_->SetExtentHeight(fields_->HeightForWidth(nullptr, content.w()));
        scroll_->UpdateLayout();
    }
    void Paint(pui::IRenderer& r, const pui::Theme& t) override {
        r.FillRoundRect(Bounds(), t.overlayRadius, t.flyoutSurface);
        r.StrokeRoundRect(Bounds().Deflate(.5f, .5f, .5f, .5f), t.overlayRadius, t.flyoutStroke, 1);
        if (auto rgb = ParseRgb(color_->Text())) {
            const auto b = Bounds();
            const auto swatch = pui::RectF::XYWH(b.l + 20, b.b - 44, 24, 24);
            r.FillRoundRect(swatch, 4, pui::Color(0xFF000000u | *rgb));
            r.StrokeRoundRect(swatch, 4, t.textSecondary, 1);
        }
    }
    bool OnPointerDown(const pui::PointerEvent&) override { return true; }
    bool OnPointerUp(const pui::PointerEvent&) override { return true; }
    bool OnKeyDown(int vk, unsigned mods) override {
        if (vk == pui::key::Escape && mods == 0) { Close(); return true; }
        return false;
    }
private:
    void Label(const wchar_t* text) { fields_->Add<pui::TextBlock>(text, pui::TextKind::Caption); }
    std::optional<wvp::SubtitleStyle> Value() const {
        auto rgb = ParseRgb(color_->Text());
        if (!rgb) return std::nullopt;
        auto value = original_;
        if (font_) {
            auto points = ParsePoints(size_->Text());
            if (!points || family_->SelectedText().empty() || weight_->SelectedIndex() < 0) return std::nullopt;
            value.font = wvp::ToUtf8(family_->SelectedText());
            value.fontWeight = weights_[static_cast<size_t>(weight_->SelectedIndex())];
            value.italic = italic_->IsOn();
            value.fontSizePt = *points;
            value.primaryRgba = (*rgb << 8) | (original_.primaryRgba & 0xFFu);
        } else value.outlineRgba = (*rgb << 8) | (original_.outlineRgba & 0xFFu);
        return value;
    }
    void Validate() {
        if (!accept_) return;
        const bool valid = Value().has_value();
        accept_->SetEnabled(valid);
        error_->SetText(valid ? L"" : font_
            ? app::Tr(L"Choose a font, a size from 4 to 400 (one decimal place), and a six-digit RGB colour.",
                      L"글꼴, 4~400 범위의 크기(소수점 한 자리), 6자리 RGB 색을 지정합니다.")
            : app::Tr(L"Enter a six-digit RGB colour, e.g. #FFFF00.", L"6자리 RGB 색을 입력합니다. 예: #FFFF00"));
        Invalidate();
    }
    void Apply() {
        const auto value = Value();
        if (!value) { Validate(); return; }
        const auto apply = apply_;
        Close();
        apply(*value);
    }
    void Close() { if (Host()) Host()->ClosePopup(this); }
    bool font_;
    wvp::SubtitleStyle original_;
    std::function<void(const wvp::SubtitleStyle&)> apply_;
    std::vector<int> weights_;
    pui::TextBlock* title_ = nullptr;
    pui::ScrollViewer* scroll_ = nullptr;
    pui::StackPanel* fields_ = nullptr;
    pui::ComboBox* family_ = nullptr;
    pui::ComboBox* weight_ = nullptr;
    pui::ToggleSwitch* italic_ = nullptr;
    pui::TextBox* size_ = nullptr;
    pui::TextBox* color_ = nullptr;
    pui::TextBlock* error_ = nullptr;
    pui::Button* cancel_ = nullptr;
    pui::Button* accept_ = nullptr;
};

void ShowSubtitlePicker(pui::UiHost& host, pui::Widget* anchor, bool font, const wvp::SubtitleStyle& style,
                        std::function<void(const wvp::SubtitleStyle&)> apply) {
    if (host.HasModalPopup()) return;
    auto dialog = std::make_unique<SubtitleStyleDialog>(font, style, std::move(apply));
    auto* raw = dialog.get();
    const auto size = host.Size();
    const float w = std::min(520.f, size.w - 32), h = std::min(font ? 600.f : 340.f, size.h - 64);
    raw->SetBounds(pui::RectF::XYWH(std::round((size.w - w) / 2), std::round((size.h - h) / 2), w, h));
    const int listener = host.AddListener([raw](pui::Widget* w, pui::UiEvent event) {
        if (w && event == pui::UiEvent::FocusChanged) raw->ScrollToFocus(w);
    });
    pui::PopupOptions options;
    options.modal = true;
    options.lightDismiss = false;
    options.anchor = anchor;
    options.onClosed = [&host, listener] { host.RemoveListener(listener); };
    host.OpenPopup(std::move(dialog), std::move(options));
}

/// Mac-specific adjustments to the Settings page (see the header).
void AdaptPageForMac(pui::Widget* page, MainView& mv, pui::UiHost& host, const std::string& logsDir) {
    // Caption style: the macOS caption settings (CaptionStyleMac.h), the link opens System Settings > Accessibility >
    // Captions (launchUri maps the page's ms-settings URI).
    if (auto* link = dynamic_cast<pui::HyperlinkButton*>(FindById(page, L"CaptionStyleSettingsLink"))) {
        link->SetText(app::Tr(L"Open caption settings", L"자막 설정 열기"));
        link->SetName(app::Tr(L"Open macOS caption settings", L"macOS 자막 설정 열기"));
        link->SetToolTip(app::Tr(L"System Settings > Accessibility > Captions", L"시스템 설정 > 손쉬운 사용 > 자막"));
    }
    ReplaceRow(FindById(page, L"CaptionStyleRow"), L'\xE8D2', app::Tr(L"Caption style", L"자막 스타일"),
               app::Tr(L"Caption background and outline follow the macOS caption style (System Settings > Accessibility > Captions).",
                       L"자막 배경과 윤곽선은 macOS 자막 스타일(시스템 설정 > 손쉬운 사용 > 자막)을 따릅니다."));
    if (auto* source = dynamic_cast<pui::ToggleSwitch*>(FindById(page, L"SubtitleUseWindowsToggle"))) {
        source->SetName(app::Tr(L"Use macOS caption settings", L"macOS 자막 설정 사용"));
        ReplaceRow(source->Parent(), L'\xE8D2', app::Tr(L"Use macOS caption settings", L"macOS 자막 설정 사용"),
                   app::Tr(L"Text subtitles use the macOS caption font, size, text colour and opacity, edge effect and background where the subtitle does not set them. The app's font, colour and outline settings are hidden while this is on.",
                           L"텍스트 자막이 지정하지 않은 글꼴, 크기, 글자 색과 불투명도, 가장자리 효과, 배경을 macOS 자막 설정에서 가져옵니다. 켜져 있는 동안 앱의 글꼴, 색, 외곽선 설정은 숨겨집니다."));
        // This shared callback commits the current style and refreshes the page's summaries, swatches and visibility.
        const auto commitAndRefresh = source->onToggled;
        for (bool font : {true, false}) {
            auto* button = dynamic_cast<pui::Button*>(FindById(page, font ? L"SubtitleFontButton" : L"SubtitleOutlineColorButton"));
            if (!button) continue;
            button->onClick = [&mv, &host, button, font, commitAndRefresh] {
                if (mv.Svc().settings.subtitle.useSystemCaptionStyle) return;
                ShowSubtitlePicker(host, button, font, mv.Svc().settings.subtitle,
                                   [&mv, font, commitAndRefresh](const wvp::SubtitleStyle& picked) {
                    auto& sub = mv.Svc().settings.subtitle;
                    if (font) {
                        sub.font = picked.font;
                        sub.fontWeight = picked.fontWeight;
                        sub.italic = picked.italic;
                        sub.fontSizePt = picked.fontSizePt;
                        sub.primaryRgba = picked.primaryRgba;
                    } else sub.outlineRgba = picked.outlineRgba;
                    commitAndRefresh(sub.useSystemCaptionStyle);
                });
            };
        }
    }
    // Debug logging: the Mac log folder.
    if (pui::Widget* toggle = FindById(page, L"DebugLoggingToggle")) {
        if (pui::Widget* row = toggle->Parent()) {
            if (row->AutomationId().empty()) row->SetAutomationId(L"DebugLoggingRow");
            std::string shown = logsDir;   // ~/Library/Logs/aribplayer, as Finder's Go to Folder takes it
            const std::string home = NSHomeDirectory().UTF8String;
            if (!home.empty() && shown.rfind(home + "/", 0) == 0) shown = "~" + shown.substr(home.size());
            const std::wstring dir = wvp::ToWide(shown);
            ReplaceRow(row, L'\xE9D9', app::Tr(L"Debug logging", L"디버그 로그"),
                       app::TrW(L"Write libffplay and app events to " + dir + L"/aribplayer-YYYYMMDD.log. Press Tab in Now playing for the debug overlay",
                                L"libffplay 및 앱 이벤트를 " + dir + L"/aribplayer-YYYYMMDD.log에 기록합니다. 지금 재생 중 화면에서 Tab 키를 누르면 디버그 오버레이가 표시됩니다"));
        }
    }
    // Apple silicon has one GPU. Hiding the whole row excludes it from stack layout and focus traversal.
    if (pui::Widget* gpu = FindById(page, L"VideoGpuCombo")) {
        if (pui::Widget* row = gpu->Parent()) { row->SetVisible(false); row->SetEnabled(false); }
    }
}
}

struct SettingsWindow::Impl {
    MainView& mv;
    pui::UiHost host;
    puimac::CGRenderer renderer;
    NSWindow* window = nil;
    PuiHostView* view = nil;
    SettingsWindowDelegate* delegate = nil;
    Root* root = nullptr;
    pui::Widget* page = nullptr;
    bool inFront = false;            // a press on the player put it in front of this window (SettingsWindow::inFront_)
    bool hiddenForPlayer = false;    // hidden while the player is minimized
    explicit Impl(MainView& m) : mv(m) {}
    NSWindow* Player() const { return (__bridge NSWindow*)(void*)mv.MainHwnd(); }
    bool Shown() const { return window.visible && !hiddenForPlayer; }
    /// Directly above the player, without activation, in the player's band.
    void RaiseAbove() {
        NSWindow* player = Player();
        if (!player) return;
        if (window.level != player.level) window.level = player.level;
        [window orderWindow:NSWindowAbove relativeTo:player.windowNumber];
    }
};

SettingsWindow::SettingsWindow(MainView& mv) : impl_(std::make_unique<Impl>(mv)) {
    Impl& d = *impl_;
    pui::InstallToolTipStyle(d.host);
    NSWindowStyleMask style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable |
                              NSWindowStyleMaskResizable | NSWindowStyleMaskFullSizeContentView;
    d.window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, kDefaultW, kDefaultH) styleMask:style backing:NSBackingStoreBuffered defer:YES];
    d.window.title = [NSString stringWithUTF8String:wvp::ToUtf8(app::Tr(L"Settings", L"설정")).c_str()];
    d.window.titlebarAppearsTransparent = YES;
    d.window.titleVisibility = NSWindowTitleHidden;
    d.window.minSize = NSMakeSize(kMinW, kMinH);
    d.window.releasedWhenClosed = NO;
    d.window.tabbingMode = NSWindowTabbingModeDisallowed;
    // Managed: like a normal-level window in Spaces and Mission Control at the player's floating level too.
    d.window.collectionBehavior |= NSWindowCollectionBehaviorFullScreenAuxiliary | NSWindowCollectionBehaviorManaged;
    d.delegate = [[SettingsWindowDelegate alloc] init];
    d.delegate.owner = this;
    d.window.delegate = d.delegate;

    NSVisualEffectView* fx = [[NSVisualEffectView alloc] initWithFrame:NSMakeRect(0, 0, kDefaultW, kDefaultH)];   // Mica's counterpart
    fx.material = NSVisualEffectMaterialWindowBackground;
    fx.blendingMode = NSVisualEffectBlendingModeBehindWindow;
    fx.state = NSVisualEffectStateFollowsWindowActiveState;
    d.window.contentView = fx;
    d.view = [[PuiHostView alloc] initWithFrame:fx.bounds host:&d.host renderer:&d.renderer];
    d.view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    [fx addSubview:d.view];
    MainView* m = &mv;
    [d.view setThemeMaker:[m](float scale) { return m->MakeTheme(scale); }];
    pui::UiHost* host = &d.host;
    [d.view setOnResize:[host](NSSize s) { host->SetSize({(float)s.width, (float)s.height}); }];
    SettingsWindow* self = this;
    [d.view setOnEscape:[self] { self->Close(); }];   // Esc closes the window once no flyout / dialog took it
    // Deactivation / miniaturise / close close the flyouts; the caption style editors stand in for the font and colour
    // dialogs Windows owns by this window and stay (their own flyouts close).
    PuiHostView* view = d.view;
    popupdismiss::Watch(d.host, (__bridge void*)d.window, [](const pui::Widget* w) { return dynamic_cast<const SubtitleStyleDialog*>(w) != nullptr; },
                        [view] { [view afterEvent]; });
}

SettingsWindow::~SettingsWindow() {
    Impl& d = *impl_;
    popupdismiss::Unwatch(d.host);
    d.host.onInvalidate = nullptr;
    d.delegate.owner = nullptr;
    [d.window orderOut:nil];
    [d.view removeFromSuperview];
}

void SettingsWindow::Open() {
    Impl& d = *impl_;
    if (!IsOpen()) {
        // A fresh page over the current settings (the page reads them when it is built), like the Windows window's
        // create-on-open.
        d.host.CloseAllPopups();
        auto root = std::make_unique<Root>();
        d.root = root.get();
        app::SettingsPageEnv env;
        env.host = &d.host;
        env.owner = (HWND)(__bridge void*)d.window;
        SettingsWindow* self = this;
        // SettingsWindow::EditFilters: the filters panel is a popup of the player window, which Settings could cover, so
        // Settings closes first; posted, since the click handler runs inside the page that closing tears down.
        env.editFilters = [self] {
            MainView& mv = self->impl_->mv;
            mv.PostToUi([self, &mv] {
                self->Close();
                app::ShowFiltersPanel(mv, app::FiltersTab::Video);
            });
        };
        env.launchUri = [self](const wchar_t* uri) {
            std::string u = wvp::ToUtf8(uri);
            if (u.rfind("ms-settings:easeofaccess-closedcaptioning", 0) == 0) u = kCaptionSettingsUrl;   // Windows Settings > Captions
            self->launched.push_back(u);
            if (self->suppressLaunch) return;
            NSString* s = [NSString stringWithUTF8String:u.c_str()];
            if (NSURL* url = s ? [NSURL URLWithString:s] : nil) [NSWorkspace.sharedWorkspace openURL:url];
        };
        auto page = app::CreateSettingsPage(d.mv, std::move(env));
        d.page = page.get();
        AdaptPageForMac(d.page, d.mv, d.host, logmac::LogsDir(d.mv.Svc().settingsStore.Path()));
        d.root->page = d.page;
        d.root->AddChild(std::move(page));
        d.host.SetRoot(std::move(root));
        d.host.SetSize({(float)NSWidth(d.view.bounds), (float)NSHeight(d.view.bounds)});
        [d.view applyTheme];

        // Remembered placement (points; SettingsServiceMac), else centred over the player window.
        const wvp::WindowPlacement& saved = d.mv.Svc().settings.settingsWindow;
        bool placed = false;
        if (auto v = app::RestorablePlacement(saved)) {
            const CGFloat top = NSMaxY(NSScreen.screens.firstObject.frame);
            [d.window setFrame:NSMakeRect(v->x, top - v->y - v->h, std::max<CGFloat>(v->w, kMinW), std::max<CGFloat>(v->h, kMinH)) display:NO];
            placed = true;
        }
        if (!placed) {
            NSWindow* main = (__bridge NSWindow*)(void*)d.mv.MainHwnd();
            NSRect f = d.window.frame;
            if (main) {
                const NSRect m = main.frame;
                f.origin = NSMakePoint(NSMidX(m) - NSWidth(f) / 2, NSMidY(m) - NSHeight(f) / 2);
                if (NSScreen* s = main.screen) {
                    const NSRect vis = s.visibleFrame;
                    f.origin.x = std::clamp(f.origin.x, NSMinX(vis), std::max(NSMinX(vis), NSMaxX(vis) - NSWidth(f)));
                    f.origin.y = std::clamp(f.origin.y, NSMinY(vis), std::max(NSMinY(vis), NSMaxY(vis) - NSHeight(f)));
                }
                [d.window setFrame:f display:NO];
            } else {
                [d.window center];
            }
        }
    }
    d.hiddenForPlayer = false;
    [d.window makeKeyAndOrderFront:nil];
    [d.window makeFirstResponder:d.view];
    StartFocusCycle();
    [d.view afterEvent];
}

void SettingsWindow::Close() {
    Impl& d = *impl_;
    if (!IsOpen()) return;
    if (auto p = app::CaptureWindowPlacement((HWND)(__bridge void*)d.window)) {
        d.mv.Svc().settings.settingsWindow = *p;
        d.mv.Svc().settingsStore.Save(d.mv.Svc().settings);
    }
    d.host.CloseAllPopups();
    d.hiddenForPlayer = false;
    [d.window orderOut:nil];
}

bool SettingsWindow::IsOpen() const { return impl_->window.visible || impl_->hiddenForPlayer; }

void SettingsWindow::StartFocusCycle() {
    Impl& d = *impl_;
    if (!d.Shown()) return;
    d.inFront = false;
    d.RaiseAbove();
}

void SettingsWindow::OnPlayerPressed() {
    Impl& d = *impl_;
    if (!d.Shown() || d.inFront) return;
    d.inFront = true;
    // Out of the topmost band; below a player that is not in it.
    if (d.window.level != NSNormalWindowLevel) d.window.level = NSNormalWindowLevel;
    NSWindow* player = d.Player();
    if (player && player.level == NSNormalWindowLevel) [d.window orderWindow:NSWindowBelow relativeTo:player.windowNumber];
}

void SettingsWindow::KeepAbove() {
    Impl& d = *impl_;
    if (d.Shown() && !d.inFront) d.RaiseAbove();
}

void SettingsWindow::OnPlayerMinimized(bool minimized) {
    Impl& d = *impl_;
    if (minimized) {
        if (!d.window.visible) return;
        d.hiddenForPlayer = true;
        d.host.CloseAllPopups();
        [d.window orderOut:nil];
    } else if (d.hiddenForPlayer) {
        d.hiddenForPlayer = false;
        d.RaiseAbove();   // orders it in without making it key
    }
}

bool SettingsWindow::PlayerInFront() const { return impl_->inFront; }

void SettingsWindow::ApplyTheme() { [impl_->view applyTheme]; }

pui::UiHost& SettingsWindow::Ui() { return impl_->host; }
pui::Widget* SettingsWindow::Page() const { return impl_->page; }
void* SettingsWindow::Window() const { return (__bridge void*)impl_->window; }

}
