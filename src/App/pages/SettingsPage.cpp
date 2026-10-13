#include "pages/SettingsPage.h"

#include <windows.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <memory>
#include <vector>

#include "AppContext.h"
#include "features/DialogGuard.h"
#include "LogicTest.h"
#include "Localize.h"
#include "features/FiltersPanel.h"
#include "features/SettingsLogic.h"
#include "features/SettingsService.h"
#include "pages/LicensesDialog.h"
#include "pui/Buttons.h"
#include "pui/ComboBox.h"
#include "pui/Glyphs.h"
#include "pui/Panels.h"
#include "pui/ScrollViewer.h"
#include "pui/Toggles.h"
#include "wvp/FfPlayer.h"
#include "wvp/win32/Utf.h"

namespace app {

namespace {
constexpr const wchar_t* kAppName = L"ARIB Player";
constexpr const wchar_t* kAppVersion = L"1.0.0";
constexpr const wchar_t* kCaptionStyleUri = L"ms-settings:easeofaccess-closedcaptioning";

// All geometry comes from sl::ComputeRowLayout.
class SettingRow final : public pui::Widget, public pui::IHeightForWidth {
public:
    SettingRow(wchar_t glyph, std::wstring title, std::wstring desc, bool stacked = false)
        : pui::Widget(pui::Role::Group), glyph_(glyph), title_(std::move(title)), desc_(std::move(desc)), stacked_(stacked) {
        SetName(title_);
        SetHelpText(desc_);
        SetClipChildren(false);
    }
    template <class T>
    T* SetControl(std::unique_ptr<T> c) {
        T* raw = c.get();
        control_ = raw;
        AddChild(std::move(c));
        return raw;
    }
    void SetDescription(std::wstring d) {
        desc_ = std::move(d);
        SetHelpText(desc_);
        RelayoutAncestors();
    }
    const std::wstring& Description() const { return desc_; }

    pui::SizeF DesiredSize(pui::IRenderer* m) override { return {0, Compute(Bounds().w(), m).height}; }
    float HeightForWidth(pui::IRenderer* m, float width) override { return Compute(width, m).height; }
    void OnLayout() override {
        layout_ = Compute(Bounds().w(), nullptr);
        if (control_) control_->SetBounds(layout_.control.Offset(Bounds().l, Bounds().t));
    }
    void Paint(pui::IRenderer& r, const pui::Theme& t) override {
        pui::RectF b = Bounds();
        r.FillRoundRect(b, t.overlayRadius, t.cardFill);
        r.StrokeRoundRect(b.Deflate(0.5f, 0.5f, 0.5f, 0.5f), t.overlayRadius, t.cardStroke, 1);
        if (glyph_) r.Text(std::wstring(1, glyph_), layout_.glyph.Offset(b.l, b.t), pui::IconTextStyle(16), t.textPrimary);
        pui::TextStyle ts = pui::BodyTextStyle(14);
        r.Text(title_, layout_.title.Offset(b.l, b.t), ts, t.textPrimary);
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
    void RelayoutAncestors() {
        layout_ = Compute(Bounds().w(), nullptr);
        if (control_) control_->SetBounds(layout_.control.Offset(Bounds().l, Bounds().t));
        pui::RelayoutAncestors(*this);
        Invalidate();
    }
    sl::RowLayoutResult Compute(float width, pui::IRenderer* m) {
        pui::IRenderer& meas = pui::MeasurerOr(m);
        sl::RowInput in;
        in.width = width;
        in.hasGlyph = glyph_ != 0;
        in.stacked = stacked_;
        float textLeft = sl::kRowPad + (in.hasGlyph ? sl::kGlyphCol : 0.f);
        pui::SizeF cd = control_ ? control_->DesiredSize(&meas) : pui::SizeF{};
        in.controlW = cd.w;
        in.controlH = cd.h;
        float textW;
        if (stacked_) {
            textW = std::max(0.f, width - textLeft - sl::kRowPad);
            if (auto* hw = dynamic_cast<pui::IHeightForWidth*>(control_)) in.controlH = hw->HeightForWidth(&meas, textW);
        } else {
            textW = std::max(0.f, width - sl::kRowPad - cd.w - 16.f - textLeft);
        }
        if (!desc_.empty()) in.descH = std::max(16.f, meas.MeasureText(desc_, DescStyle(), textW).h);
        return sl::ComputeRowLayout(in);
    }

    wchar_t glyph_;
    std::wstring title_, desc_;
    bool stacked_;
    pui::Widget* control_ = nullptr;
    sl::RowLayoutResult layout_{};
};

class AccentSwatch final : public pui::ButtonBase {
public:
    static constexpr float kSize = 32;
    AccentSwatch(int index, const sl::AccentChoice& c) : pui::ButtonBase(pui::Role::RadioButton), index_(index), argb_(c.argb) {
        SetName(std::wstring(c.name) + Tr(L" accent colour", L" 강조 색"));
        SetToolTip(c.name);
        SetFocusable(true);
    }
    std::function<void(int)> onPick;
    int Index() const { return index_; }
    void SetSelected(bool s) { if (selected_ != s) { selected_ = s; Invalidate(); Raise(pui::UiEvent::SelectionChanged); } }
    pui::SizeF DesiredSize(pui::IRenderer*) override { return {kSize, kSize}; }
    void Paint(pui::IRenderer& r, const pui::Theme& t) override {
        pui::RectF b = Bounds();
        pui::PointF c{b.cx(), b.cy()};
        float rad = IsHovered() || VisuallyPressed() ? 13.f : 12.f;
        r.FillEllipse(c, rad, rad, pui::Color(argb_));
        if (selected_) {
            r.StrokeEllipse(c, 15, 15, t.textPrimary, 2);
            float lum = 0.299f * ((argb_ >> 16 & 0xFF) / 255.f) + 0.587f * ((argb_ >> 8 & 0xFF) / 255.f) + 0.114f * ((argb_ & 0xFF) / 255.f);
            r.Text(pui::glyph::Str(pui::glyph::CheckMark), b, pui::IconTextStyle(14), lum > 0.6f ? pui::Color(0xFF000000) : pui::Color(0xFFFFFFFF));
        }
        PaintFocusVisual(r, t, b, 16);
    }
    bool OnKeyDown(int vk, unsigned mods) override {
        if (vk == VK_LEFT || vk == VK_RIGHT) {
            auto& sib = Parent()->Children();
            int i = IndexInParent() + (vk == VK_RIGHT ? 1 : -1);
            if (i >= 0 && i < (int)sib.size()) {
                if (auto* s = dynamic_cast<AccentSwatch*>(sib[(size_t)i].get())) {
                    s->Focus();
                    if (onPick) onPick(s->Index());
                }
            }
            return true;
        }
        return pui::ButtonBase::OnKeyDown(vk, mods);
    }
    bool SupportsSelectionItem() const override { return true; }
    bool IsSelected() const override { return selected_; }
    void SelectItem() override { if (onPick) onPick(index_); }
protected:
    void Activate() override { if (onPick) onPick(index_); }
private:
    int index_;
    std::uint32_t argb_;
    bool selected_ = false;
};

class SwatchGrid final : public pui::Widget, public pui::IHeightForWidth {
public:
    static constexpr float kGap = 8;
    SwatchGrid() : pui::Widget(pui::Role::Group) {
        SetName(Tr(L"Accent colour", L"강조 색"));
        const auto& p = sl::AccentPalette();
        for (int i = 0; i < sl::kAccentCount; i++) Add<AccentSwatch>(i, p[(size_t)i]);
    }
    void SetOnPick(const std::function<void(int)>& f) { for (auto& c : Children()) static_cast<AccentSwatch*>(c.get())->onPick = f; }
    void SetSelected(int idx) { for (auto& c : Children()) { auto* s = static_cast<AccentSwatch*>(c.get()); s->SetSelected(s->Index() == idx); } }
    void OnLayout() override {
        auto cells = sl::ComputeSwatchGrid((int)Children().size(), AccentSwatch::kSize, kGap, Bounds().w());
        for (size_t i = 0; i < cells.size(); i++)
            Children()[i]->SetBounds(pui::RectF::XYWH(Bounds().l + cells[i].x, Bounds().t + cells[i].y, AccentSwatch::kSize, AccentSwatch::kSize));
    }
    pui::SizeF DesiredSize(pui::IRenderer*) override { return sl::SwatchGridSize((int)Children().size(), AccentSwatch::kSize, kGap, 100000.f); }
    float HeightForWidth(pui::IRenderer*, float w) override { return sl::SwatchGridSize((int)Children().size(), AccentSwatch::kSize, kGap, w).h; }
    bool SupportsSelection() const override { return true; }
    bool IsSelectionRequired() const override { return false; }
    std::vector<pui::Widget*> SelectedChildren() const override {
        std::vector<pui::Widget*> v;
        for (auto& c : Children()) if (static_cast<AccentSwatch*>(c.get())->IsSelected()) v.push_back(c.get());
        return v;
    }
};

std::wstring PickAribFont(HWND owner) {
    if (OsDialogsBlocked()) return {};   // tests never show an OS dialog: cancelled
    using Microsoft::WRL::ComPtr;
    ComPtr<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return {};
    constexpr GUID kGuid = {0x9c0f3fd2, 0x18a1, 0x4c5f, {0xb3, 0x8c, 0x84, 0x35, 0x15, 0x68, 0x03, 0x29}};
    dlg->SetClientGuid(kGuid);
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_FILEMUSTEXIST);
    dlg->SetTitle(Tr(L"Select ARIB caption font", L"ARIB 자막 글꼴 선택"));
    COMDLG_FILTERSPEC spec[] = {{Tr(L"TrueType font", L"TrueType 글꼴"), L"*.ttf"}, {Tr(L"OpenType font", L"OpenType 글꼴"), L"*.otf"}, {Tr(L"All files", L"모든 파일"), L"*.*"}};
    dlg->SetFileTypes(3, spec);
    if (FAILED(dlg->Show(owner))) return {};
    ComPtr<IShellItem> it;
    PWSTR p = nullptr;
    std::wstring out;
    if (SUCCEEDED(dlg->GetResult(&it)) && SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) {
        out = p;
        CoTaskMemFree(p);
    }
    return out;
}

int CALLBACK CollectFontFamily(const LOGFONTW* lf, const TEXTMETRICW*, DWORD, LPARAM lp) {
    if (lf->lfFaceName[0] != L'@') reinterpret_cast<std::vector<std::wstring>*>(lp)->push_back(lf->lfFaceName);
    return 1;
}

// Installed font families, sorted; the stored family is always listed so the combo can show it.
std::vector<std::wstring> SubtitleFontChoices(const std::string& current) {
    std::vector<std::wstring> v;
    LOGFONTW lf{};
    lf.lfCharSet = DEFAULT_CHARSET;
    if (HDC dc = GetDC(nullptr)) {
        EnumFontFamiliesExW(dc, &lf, CollectFontFamily, reinterpret_cast<LPARAM>(&v), 0);
        ReleaseDC(nullptr, dc);
    }
    v.push_back(wvp::ToWide(current));
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
    return v;
}

std::wstring CaptionFontDescription(const wvp::AppSettings& settings) {
    if (settings.aribFont.empty()) return Tr(L"Using the bundled WLC Maru ARIB font; the decoder falls back to Windows fonts if unavailable.", L"내장된 WLC Maru ARIB 글꼴을 사용합니다. 사용할 수 없으면 디코더가 Windows 글꼴로 대체합니다.");
    return std::wstring(Tr(L"Custom font: ", L"사용자 지정 글꼴: ")) + wvp::ToWide(settings.aribFont);
}

class SettingsPageImpl final : public pui::Widget {
public:
    SettingsPageImpl(AppContext& ctx, SettingsPageEnv env) : pui::Widget(pui::Role::Pane), ctx_(ctx), env_(std::move(env)) {
        if (!env_.host) env_.host = &ctx_.Ui();
        if (!env_.launchUri) env_.launchUri = [](const wchar_t* uri) { ShellExecuteW(nullptr, L"open", uri, nullptr, nullptr, SW_SHOWNORMAL); };
        SetName(Tr(L"Settings", L"설정"));
        SetAutomationId(L"SettingsPage");
        state_ = sl::StateFromSettings(ctx_.Svc().settings);

        hdr_ = Add<pui::TextBlock>(Tr(L"Settings", L"설정"), pui::TextKind::Title);
        hdr_->SetName(Tr(L"Settings", L"설정"));
        sv_ = Add<pui::ScrollViewer>();
        sv_->SetName(Tr(L"Settings content", L"설정 내용"));
        sv_->SetAutomationId(L"SettingsScroll");
        col_ = sv_->EmplaceContent<pui::StackPanel>(pui::Orientation::Vertical, 2.f);
        col_->SetName(Tr(L"Settings list", L"설정 목록"));
        BuildAppearance();
        BuildPlayback();
        BuildSubtitles();
        BuildAbout();
        Refresh();
        listener_ = env_.host->AddListener([this](pui::Widget* w, pui::UiEvent ev) {
            if (ev == pui::UiEvent::FocusChanged && w && col_ && col_->IsAncestorOf(w) && w != col_) sv_->ScrollIntoView(w);
        });
    }
    ~SettingsPageImpl() override { env_.host->RemoveListener(listener_); }

    void OnLayout() override {
        sl::PageLayout l = sl::ComputePageLayout(Bounds().w(), Bounds().h());
        hdr_->SetBounds(l.header.Offset(Bounds().l, Bounds().t));
        col_->SetPadding({l.contentPadH, 0, l.contentPadH, 32});
        sv_->SetBounds(l.scroll.Offset(Bounds().l, Bounds().t));
        UpdateExtent();
    }

private:
    void Section(const wchar_t* title) {
        if (!col_->Children().empty()) col_->Add<pui::Spacer>(0.f, 14.f);
        auto* t = col_->Add<pui::TextBlock>(title, pui::TextKind::BodyStrong);
        t->SetName(title);
        col_->Add<pui::Spacer>(0.f, 4.f);
    }
    SettingRow* Row(wchar_t glyph, const wchar_t* title, const std::wstring& desc, bool stacked = false) {
        return col_->Add<SettingRow>(glyph, std::wstring(title), desc, stacked);
    }
    pui::ToggleSwitch* Toggle(SettingRow* row, const wchar_t* name, const wchar_t* id, bool on, std::function<void(bool)> fn) {
        auto sw = std::make_unique<pui::ToggleSwitch>();
        sw->SetOn(on);
        sw->SetName(name);
        sw->SetAutomationId(id);
        pui::ToggleSwitch* raw = row->SetControl(std::move(sw));
        raw->onToggled = std::move(fn);
        return raw;
    }

    void BuildAppearance() {
        Section(Tr(L"Appearance", L"모양"));
        auto* themeRow = Row(L'\xE771', Tr(L"App theme", L"앱 테마"), {}, true);
        auto group = std::make_unique<pui::RadioGroup>();
        group->SetHorizontal(true);
        group->SetName(Tr(L"App theme", L"앱 테마"));
        group->SetAutomationId(L"ThemeGroup");
        group->AddOption(Tr(L"Use system setting", L"시스템 설정 사용"))->SetName(Tr(L"Use system setting", L"시스템 설정 사용"));
        group->AddOption(Tr(L"Light", L"밝게"))->SetName(Tr(L"Light", L"밝게"));
        group->AddOption(Tr(L"Dark", L"어둡게"))->SetName(Tr(L"Dark", L"어둡게"));
        group->SetSelectedIndex(state_.themeIndex);
        theme_ = themeRow->SetControl(std::move(group));
        theme_->onSelectionChanged = [this](int i) {
            if (i < 0) return;
            state_.themeIndex = i;
            Commit();
        };

        auto* sysRow = Row(L'\xE790', Tr(L"Use system accent colour", L"시스템 강조 색 사용"), {});
        sysAccent_ = Toggle(sysRow, Tr(L"Use system accent colour", L"시스템 강조 색 사용"), L"SystemAccentToggle", state_.useSystemAccent, [this](bool on) {
            state_.useSystemAccent = on;
            if (!on && state_.accentIndex < 0 && ctx_.Svc().settings.accentColor == 0) state_.accentIndex = 0;
            Commit();
            Refresh();
        });

        accentRow_ = Row(L'\xE790', Tr(L"Accent colour", L"강조 색"), {}, true);
        auto grid = std::make_unique<SwatchGrid>();
        grid->SetAutomationId(L"AccentSwatches");
        swatches_ = accentRow_->SetControl(std::move(grid));
        swatches_->SetOnPick([this](int i) {
            state_.accentIndex = i;
            state_.useSystemAccent = false;
            Commit();
            Refresh();
        });
    }

    void BuildPlayback() {
        Section(Tr(L"Playback", L"재생"));
        auto* top = Row(L'\xE718', Tr(L"Always on top", L"항상 위에"), {}, true);   // E718 = Pin
        auto topGroup = std::make_unique<pui::RadioGroup>();
        topGroup->SetHorizontal(true);
        topGroup->SetName(Tr(L"Always on top", L"항상 위에"));
        topGroup->SetAutomationId(L"AlwaysOnTopGroup");
        for (const wchar_t* label : {Tr(L"Off", L"사용 안 함"), Tr(L"Always", L"항상"), Tr(L"While playing", L"재생 중"), Tr(L"While fullscreen", L"전체 화면 중")}) topGroup->AddOption(label)->SetName(label);
        topGroup->SetSelectedIndex(state_.alwaysOnTopIndex);
        auto* topCtl = top->SetControl(std::move(topGroup));
        topCtl->onSelectionChanged = [this](int i) {
            if (i < 0) return;
            state_.alwaysOnTopIndex = i;
            Commit();
        };

        auto* hw = Row(pui::glyph::VideoSettings, Tr(L"Hardware decoding", L"하드웨어 디코딩"),
                       Tr(L"Auto lets the pipeline planner pick the decoder with the fewest copies, software included; Prefer GPU always uses a GPU decoder when one fits. Changing it re-opens the current item at the same position", L"자동은 파이프라인 계획기가 소프트웨어를 포함해 복사가 가장 적은 디코더를 고르게 하며, GPU 우선은 맞는 GPU 디코더가 있으면 항상 사용합니다. 변경하면 현재 항목이 같은 위치에서 다시 열립니다"), true);
        auto hwGroup = std::make_unique<pui::RadioGroup>();
        hwGroup->SetHorizontal(true);
        hwGroup->SetName(Tr(L"Hardware decoding", L"하드웨어 디코딩"));
        hwGroup->SetAutomationId(L"HardwareDecodingGroup");
        for (const auto& ch : sl::HwDecoderChoices()) hwGroup->AddOption(ch.label)->SetName(ch.label);
        hwGroup->SetSelectedIndex(state_.hwDecoderIndex);
        auto* hwCtl = hw->SetControl(std::move(hwGroup));
        hwCtl->onSelectionChanged = [this](int i) {
            if (i < 0) return;
            state_.hwDecoderIndex = i;
            Commit();
        };

        auto* gpu = Row(pui::glyph::VideoSettings, Tr(L"Video GPU", L"영상 GPU"),
                        Tr(L"Automatic picks the GPU with the fewest copies, preferring the one driving this window's monitor. A specific GPU always wins (e.g. for speed); if it is missing, Automatic is used until it returns", L"자동은 복사가 가장 적은 GPU를 고르며, 이 창의 모니터를 구동하는 GPU를 우선합니다. 특정 GPU를 선택하면 항상 그 GPU가 우선하며(예: 속도), 없으면 다시 나타날 때까지 자동이 사용됩니다"), true);
        gpus_ = ctx_.VideoGpus();
        const wvp::VideoGpuPref& cur = ctx_.Svc().settings.videoGpu;
        std::vector<std::wstring> items = { Tr(L"Automatic", L"자동") };
        int sel = 0;
        for (size_t i = 0; i < gpus_.size(); i++) {
            items.push_back(wvp::ToWide(gpus_[i].name));
            if (!cur.Automatic() && (gpus_[i].luid == cur.luid || (cur.vendorId != 0 && gpus_[i].vendorId == cur.vendorId &&
                                                                     gpus_[i].deviceId == cur.deviceId)))
                sel = static_cast<int>(i) + 1;
        }
        if (!cur.Automatic() && sel == 0) {   // remembered GPU that is not present now: keep it selectable
            gpus_.push_back(cur);
            items.push_back(wvp::ToWide(cur.name) + Tr(L" (not present)", L" (없음)"));
            sel = static_cast<int>(items.size()) - 1;
        }
        auto gpuCombo = std::make_unique<pui::ComboBox>();
        gpuCombo->SetName(Tr(L"Video GPU", L"영상 GPU"));
        gpuCombo->SetAutomationId(L"VideoGpuCombo");
        gpuCombo->SetItems(std::move(items));
        gpuCombo->SetSelectedIndex(sel);
        auto* gpuCtl = gpu->SetControl(std::move(gpuCombo));
        gpuCtl->onSelectionChanged = [this](int i) {
            if (i < 0) return;
            ctx_.Svc().settings.videoGpu = i == 0 ? wvp::VideoGpuPref{} : gpus_[static_cast<size_t>(i - 1)];
            Commit();
            ctx_.VideoGpuSettingChanged();
        };

        auto* ed = Row(pui::glyph::Equalizer, Tr(L"Filters", L"필터"),
                       Tr(L"Filters apply to every file and are saved automatically", L"필터는 모든 파일에 적용되며 자동으로 저장됩니다"));
        auto b = std::make_unique<pui::Button>(Tr(L"Edit filters\x2026", L"필터 편집\x2026"));
        b->SetName(Tr(L"Edit filters", L"필터 편집"));
        b->SetAutomationId(L"EditFiltersButton");
        auto* braw = ed->SetControl(std::move(b));
        braw->onClick = [this] { if (env_.editFilters) env_.editFilters(); else ShowFiltersPanel(ctx_, FiltersTab::Video); };
    }

    pui::ComboBox* Combo(SettingRow* row, const wchar_t* name, const wchar_t* id, std::vector<std::wstring> items, int sel,
                         std::function<void(int)> fn) {
        auto cb = std::make_unique<pui::ComboBox>();
        cb->SetName(name);
        cb->SetAutomationId(id);
        cb->SetItems(std::move(items));
        cb->SetSelectedIndex(sel);
        pui::ComboBox* raw = row->SetControl(std::move(cb));
        raw->onSelectionChanged = [fn = std::move(fn)](int i) { if (i >= 0) fn(i); };
        return raw;
    }

    void BuildSubtitles() {
        Section(Tr(L"Subtitles", L"자막"));
        const auto& settings = ctx_.Svc().settings;
        const wvp::SubtitleStyle& st = settings.subtitle;
        auto edit = [this](std::function<void(wvp::SubtitleStyle&)> fn) {
            fn(ctx_.Svc().settings.subtitle);
            Commit();
        };

        auto* showRow = Row(L'\xE8D2', Tr(L"Show subtitles", L"자막 표시"), Tr(L"Show or hide subtitles of every kind without stopping their decoding.", L"자막 디코딩을 멈추지 않고 모든 종류의 자막을 표시하거나 숨깁니다."));
        Toggle(showRow, Tr(L"Show subtitles", L"자막 표시"), L"ShowCaptionsToggle", settings.showCaptions, [this](bool on) {
            ctx_.Svc().settings.showCaptions = on;
            Commit();
        });

        auto* fontRow = Row(L'\xE8D2', Tr(L"Subtitle font", L"자막 글꼴"), Tr(L"Font family for text subtitles (SRT, SAMI, WebVTT, ASS/SSA).", L"텍스트 자막(SRT, SAMI, WebVTT, ASS/SSA)에 사용할 글꼴입니다."));
        fonts_ = SubtitleFontChoices(st.font);
        int fontSel = (int)(std::find(fonts_.begin(), fonts_.end(), wvp::ToWide(st.font)) - fonts_.begin());
        Combo(fontRow, Tr(L"Subtitle font", L"자막 글꼴"), L"SubtitleFontCombo", fonts_, fontSel, [this, edit](int i) {
            edit([&](wvp::SubtitleStyle& v) { v.font = wvp::ToUtf8(fonts_[(size_t)i]); });
        });
        Toggle(Row(L'\xE8D2', Tr(L"Always use this font", L"항상 이 글꼴 사용"), Tr(L"Off keeps the font of SRT, SAMI and WebVTT as the player's default; ASS/SSA keep their own styles.", L"끄면 자막 파일 형식에 맞는 기본 글꼴을 사용합니다. ASS/SSA는 자체 스타일을 유지합니다.")),
               Tr(L"Always use this font", L"항상 이 글꼴 사용"), L"SubtitleOverrideFontToggle", st.overrideFont,
               [edit](bool on) { edit([&](wvp::SubtitleStyle& v) { v.overrideFont = on; }); });

        auto* sizeRow = Row(L'\xE8D2', Tr(L"Subtitle size", L"자막 크기"), {});
        std::vector<std::wstring> scales;
        for (double v : sl::kSubtitleScalePresets) scales.push_back(sl::PercentLabel(v));
        Combo(sizeRow, Tr(L"Subtitle size", L"자막 크기"), L"SubtitleSizeCombo", scales, sl::NearestPreset(sl::kSubtitleScalePresets, st.fontScale),
              [edit](int i) { edit([&](wvp::SubtitleStyle& v) { v.fontScale = sl::kSubtitleScalePresets[(size_t)i]; }); });
        Toggle(Row(L'\xE8D2', Tr(L"Apply the size", L"크기 적용"), {}), Tr(L"Apply the size", L"크기 적용"), L"SubtitleOverrideSizeToggle", st.overrideSize,
               [edit](bool on) { edit([&](wvp::SubtitleStyle& v) { v.overrideSize = on; }); });

        auto colorNames = [] {
            std::vector<std::wstring> v;
            for (const auto& c : sl::SubtitleColorPresets()) v.push_back(c.name);
            return v;
        };
        auto* textRow = Row(L'\xE8D2', Tr(L"Text colour", L"글자 색"), {});
        Combo(textRow, Tr(L"Text colour", L"글자 색"), L"SubtitleTextColorCombo", colorNames(), sl::ColorPresetIndex(st.primaryRgba),
              [edit](int i) { edit([&](wvp::SubtitleStyle& v) { v.primaryRgba = sl::SubtitleColorPresets()[(size_t)i].rgba; }); });
        auto* outlineColorRow = Row(L'\xE8D2', Tr(L"Outline colour", L"외곽선 색"), {});
        Combo(outlineColorRow, Tr(L"Outline colour", L"외곽선 색"), L"SubtitleOutlineColorCombo", colorNames(), sl::ColorPresetIndex(st.outlineRgba),
              [edit](int i) { edit([&](wvp::SubtitleStyle& v) { v.outlineRgba = sl::SubtitleColorPresets()[(size_t)i].rgba; }); });
        Toggle(Row(L'\xE8D2', Tr(L"Apply the colours", L"색 적용"), {}), Tr(L"Apply the colours", L"색 적용"), L"SubtitleOverrideColorsToggle", st.overrideColors,
               [edit](bool on) { edit([&](wvp::SubtitleStyle& v) { v.overrideColors = on; }); });

        auto* borderRow = Row(L'\xE8D2', Tr(L"Outline width", L"외곽선 두께"), {});
        std::vector<std::wstring> widths;
        for (double v : sl::kSubtitleOutlinePresets) widths.push_back(sl::PixelLabel(v));
        Combo(borderRow, Tr(L"Outline width", L"외곽선 두께"), L"SubtitleOutlineWidthCombo", widths, sl::NearestPreset(sl::kSubtitleOutlinePresets, st.outlineWidth),
              [edit](int i) { edit([&](wvp::SubtitleStyle& v) { v.outlineWidth = sl::kSubtitleOutlinePresets[(size_t)i]; }); });
        Toggle(Row(L'\xE8D2', Tr(L"Apply the outline", L"외곽선 적용"), {}), Tr(L"Apply the outline", L"외곽선 적용"), L"SubtitleOverrideBorderToggle", st.overrideBorder,
               [edit](bool on) { edit([&](wvp::SubtitleStyle& v) { v.overrideBorder = on; }); });

        Toggle(Row(L'\xE8D2', Tr(L"Also apply to ASS/SSA subtitles", L"ASS/SSA 자막에도 적용"), Tr(L"The settings above normally leave ASS/SSA subtitles with their own styles.", L"위 설정은 기본적으로 ASS/SSA 자막의 자체 스타일에는 적용되지 않습니다.")),
               Tr(L"Also apply to ASS/SSA subtitles", L"ASS/SSA 자막에도 적용"), L"SubtitleOverrideAssToggle", st.applyToAss,
               [edit](bool on) { edit([&](wvp::SubtitleStyle& v) { v.applyToAss = on; }); });

        col_->Add<pui::Spacer>(0.f, 10.f);
        auto* aribTitle = col_->Add<pui::TextBlock>(Tr(L"ARIB captions", L"ARIB 자막"), pui::TextKind::BodyStrong);
        aribTitle->SetName(Tr(L"ARIB captions", L"ARIB 자막"));
        col_->Add<pui::Spacer>(0.f, 4.f);

        auto* superRow = Row(L'\xE8D2', Tr(L"Show superimpose", L"슈퍼임포즈 표시"), Tr(L"Show ARIB on-screen text (文字スーパー) independently of captions.", L"자막과 별도로 ARIB 화면 문자(文字スーパー)를 표시합니다."));
        Toggle(superRow, Tr(L"Show superimpose", L"슈퍼임포즈 표시"), L"ShowSuperimposeToggle", settings.showSuperimpose, [this](bool on) {
            ctx_.Svc().settings.showSuperimpose = on;
            Commit();
        });

        captionFontRow_ = Row(L'\xE8D2', Tr(L"ARIB caption font", L"ARIB 자막 글꼴"), CaptionFontDescription(settings));
        auto buttons = std::make_unique<pui::StackPanel>(pui::Orientation::Horizontal, 8.f);
        buttons->SetName(Tr(L"Caption font buttons", L"자막 글꼴 단추"));
        auto* choose = buttons->Add<pui::Button>(Tr(L"Choose font\x2026", L"글꼴 선택\x2026"));
        choose->SetName(Tr(L"Choose caption font", L"자막 글꼴 선택"));
        choose->SetAutomationId(L"CaptionFontBrowseButton");
        choose->onClick = [this] { BrowseCaptionFont(); };
        auto* reset = buttons->Add<pui::Button>(Tr(L"Reset", L"초기화"));
        reset->SetName(Tr(L"Reset caption font", L"자막 글꼴 초기화"));
        reset->SetAutomationId(L"CaptionFontResetButton");
        reset->onClick = [this] { ResetCaptionFont(); };
        captionFontReset_ = reset;
        captionFontRow_->SetControl(std::move(buttons));

        auto* styleRow = Row(L'\xE8D2', Tr(L"Caption style", L"자막 스타일"), Tr(L"Caption colours, size and background follow the Windows caption settings.", L"자막 색, 크기 및 배경은 Windows 자막 설정을 따릅니다."));
        auto link = std::make_unique<pui::HyperlinkButton>(Tr(L"Open Windows caption settings", L"Windows 자막 설정 열기"));
        link->SetName(Tr(L"Open Windows caption settings", L"Windows 자막 설정 열기"));
        link->SetAutomationId(L"CaptionStyleSettingsLink");
        link->SetToolTip(Tr(L"Settings > Accessibility > Captions", L"설정 > 접근성 > 자막"));
        styleRow->SetAutomationId(L"CaptionStyleRow");
        styleRow->SetControl(std::move(link))->onClick = [this] { env_.launchUri(kCaptionStyleUri); };
    }

    void BuildAbout() {
        Section(Tr(L"About", L"정보"));
        Row(pui::glyph::Info, kAppName, std::wstring(Tr(L"Version ", L"버전 ")) + kAppVersion);
        auto* lic = Row(L'\xE8A5', Tr(L"Open source licenses", L"오픈 소스 라이선스"), {});
        auto b = std::make_unique<pui::Button>(Tr(L"View", L"보기"));
        b->SetName(Tr(L"Open source licenses", L"오픈 소스 라이선스"));
        b->SetAutomationId(L"OpenSourceLicensesButton");
        lic->SetControl(std::move(b))->onClick = [this] { ShowLicensesDialog(*env_.host, ctx_.Measurer()); };

        auto* dbg = Row(L'\xE9D9', Tr(L"Debug logging", L"디버그 로그"),
                        Tr(L"Write libffplay and app events to %LOCALAPPDATA%\\aribplayer\\logs\\aribplayer-YYYYMMDD.log. Press Tab in Now playing for the debug overlay", L"libffplay 및 앱 이벤트를 %LOCALAPPDATA%\\aribplayer\\logs\\aribplayer-YYYYMMDD.log에 기록합니다. 지금 재생 중 화면에서 Tab 키를 누르면 디버그 오버레이가 표시됩니다"));
        Toggle(dbg, Tr(L"Debug logging", L"디버그 로그"), L"DebugLoggingToggle", state_.debugLogging, [this](bool on) { state_.debugLogging = on; Commit(); });
    }

    void Commit() {
        sl::ApplyState(ctx_.Svc().settings, state_);
        OnSettingsChanged(ctx_);
    }
    void Refresh() {
        bool showAccent = !state_.useSystemAccent;
        bool visChanged = accentRow_->Visible() != showAccent;
        accentRow_->SetVisible(showAccent);
        swatches_->SetSelected(state_.useSystemAccent ? -1 : sl::AccentIndexFor(ctx_.Svc().settings.accentColor));
        captionFontReset_->SetEnabled(!ctx_.Svc().settings.aribFont.empty());
        captionFontRow_->SetDescription(CaptionFontDescription(ctx_.Svc().settings));
        if (sysAccent_->IsOn() != state_.useSystemAccent) sysAccent_->SetOn(state_.useSystemAccent);
        if (visChanged) col_->RequestLayout();
        UpdateExtent();
        Invalidate();
    }
    void UpdateExtent() {
        float w = sv_->Bounds().w();
        if (w <= 0) return;
        sv_->SetExtentHeight(col_->HeightForWidth(nullptr, w));
        sv_->UpdateLayout();
    }
    void BrowseCaptionFont() {
        std::wstring picked = PickAribFont(env_.owner ? env_.owner : ctx_.MainHwnd());
        if (picked.empty()) return;
        ctx_.Svc().settings.aribFont = wvp::ToUtf8(picked);
        Commit();
        Refresh();
        ctx_.ShowToast(Tr(L"Caption font saved", L"자막 글꼴을 저장했습니다"));
    }
    void ResetCaptionFont() {
        ctx_.Svc().settings.aribFont.clear();
        Commit();
        Refresh();
        ctx_.ShowToast(Tr(L"Caption font reset to the bundled font", L"자막 글꼴을 내장 글꼴로 되돌렸습니다"));
    }

    AppContext& ctx_;
    SettingsPageEnv env_;
    sl::PageState state_;
    std::vector<wvp::VideoGpuPref> gpus_;   // Video GPU combo items after "Automatic"
    pui::TextBlock* hdr_ = nullptr;
    pui::ScrollViewer* sv_ = nullptr;
    pui::StackPanel* col_ = nullptr;
    pui::RadioGroup* theme_ = nullptr;
    pui::ToggleSwitch* sysAccent_ = nullptr;
    SettingRow* accentRow_ = nullptr;
    SwatchGrid* swatches_ = nullptr;
    std::vector<std::wstring> fonts_;
    SettingRow* captionFontRow_ = nullptr;
    pui::Button* captionFontReset_ = nullptr;
    int listener_ = 0;
};
}

std::unique_ptr<pui::Widget> CreateSettingsPage(AppContext& ctx, SettingsPageEnv env) {
    return std::make_unique<SettingsPageImpl>(ctx, std::move(env));
}

}

namespace {

// Minimal AppContext for the Settings page test: no window, no player.
class SettingsTestContext final : public app::AppContext {
public:
    explicit SettingsTestContext(const std::wstring& dir) : svc_(dir) {}
    HWND MainHwnd() const override { return nullptr; }
    pui::UiHost& Ui() override { return host_; }
    wvp::PlaybackController* Controller() override { return nullptr; }
    wvp::FfPlayer* Player() override { return nullptr; }
    app::Services& Svc() override { return svc_; }
    pui::IRenderer& Measurer() override { return measurer_; }
    void SetFullscreen(bool) override {}
    bool IsFullscreen() const override { return false; }
    void SetMiniPlayer(bool) override {}
    bool IsMiniPlayer() const override { return false; }
    app::ShellMode Mode() const override { return app::ShellMode{}; }
    void AppendToCurrentPlaylist(std::vector<wvp::MediaItem>) override {}
    void ReplaceUnnamedPlaylistAtPosition(std::vector<wvp::MediaItem>, double) override {}
    void ShowInfoBar(app::InfoSeverity, const std::wstring&, const std::wstring&) override {}
    void ShowToast(const std::wstring&) override {}
    void ApplyTheme(pui::ThemeMode, uint32_t) override {}
    const pui::Theme& CurrentTheme() const override { return theme_; }
    pui::ThemeMode CurrentThemeMode() const override { return pui::ThemeMode::Dark; }
    float Scale() const override { return 1.f; }
    void RequestRedraw() override {}
    void Execute(app::CommandId) override {}
    void PostToUi(std::function<void()>) override {}
private:
    app::Services svc_;
    pui::UiHost host_;
    pui::NullMeasurer measurer_;
    pui::Theme theme_ = pui::Theme::Make(false);
};

pui::Widget* FindByAutomationId(pui::Widget* w, const std::wstring& id) {
    if (w->AutomationId() == id) return w;
    for (auto& c : w->Children())
        if (pui::Widget* f = FindByAutomationId(c.get(), id)) return f;
    return nullptr;
}

}

LOGIC_TEST(settings_caption_style_row_opens_windows_caption_settings) {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring dir = std::wstring(tmp) + L"aribplayer-settings-caption-" + std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(dir.c_str(), nullptr);
    {
        SettingsTestContext ctx(dir);
        std::vector<std::wstring> launched;
        app::SettingsPageEnv env;
        env.launchUri = [&](const wchar_t* uri) { launched.push_back(uri); };
        auto page = app::CreateSettingsPage(ctx, std::move(env));
        pui::Widget* root = page.get();
        pui::Widget* row = FindByAutomationId(root, L"CaptionStyleRow");
        LT_CHECK(row != nullptr);
        if (row) {
            LT_CHECK(row->Name() == L"Caption style");
            LT_CHECK(row->GetRole() == pui::Role::Group);
            LT_CHECK(!row->HelpText().empty());
        }
        auto* link = dynamic_cast<pui::HyperlinkButton*>(FindByAutomationId(root, L"CaptionStyleSettingsLink"));
        LT_CHECK(link != nullptr);
        if (link) {
            LT_CHECK(link->Name() == L"Open Windows caption settings");
            LT_CHECK(link->GetRole() == pui::Role::Hyperlink);
            LT_CHECK(link->SupportsInvoke());
            LT_CHECK(launched.empty());
            link->Invoke();
            LT_CHECK_EQ(launched.size(), (size_t)1);
            if (!launched.empty()) LT_CHECK(launched[0] == L"ms-settings:easeofaccess-closedcaptioning");
        }
    }
    RemoveDirectoryW(dir.c_str());
}
