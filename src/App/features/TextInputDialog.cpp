#include "features/TextInputDialog.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>

#include <algorithm>
#include <memory>
#include <vector>

#include "AppContext.h"
#include "features/DialogGuard.h"
#include "LogicTest.h"

namespace app {

namespace {
constexpr wchar_t kClass[] = L"aribplayer.TextPrompt";
constexpr int kIdEdit = 101, kIdLabel = 102, kIdError = 103;
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

int S(int dip, int dpi) { return MulDiv(dip, dpi, 96); }

pui::Color Flatten(pui::Color over, pui::Color under) {
    float a = over.a();
    auto ch = [&](float o, float u) { return (uint32_t)(std::clamp(o * a + u * (1 - a), 0.f, 1.f) * 255.f + 0.5f); };
    return pui::Color(0xFF000000u | (ch(over.r(), under.r()) << 16) | (ch(over.g(), under.g()) << 8) | ch(over.b(), under.b()));
}
COLORREF Ref(pui::Color c) {
    return RGB((c.v >> 16) & 0xFF, (c.v >> 8) & 0xFF, c.v & 0xFF);
}

struct Dlg {
    TextPromptOptions opt;
    pui::Theme theme;
    DialogColors colors{};
    int dpi = 96;
    HWND hwnd = nullptr, label = nullptr, edit = nullptr, error = nullptr, ok = nullptr, cancel = nullptr;
    HFONT font = nullptr;
    HBRUSH bgBrush = nullptr, editBrush = nullptr;
    bool done = false, accepted = false;
    std::wstring result;
};

HFONT MakeFont(int dpi) {
    auto make = [&](const wchar_t* face) {
        LOGFONTW lf{};
        lf.lfHeight = -MulDiv(14, dpi, 96);
        lf.lfWeight = FW_NORMAL;
        lf.lfQuality = CLEARTYPE_QUALITY;
        lf.lfCharSet = DEFAULT_CHARSET;
        wcscpy_s(lf.lfFaceName, face);
        return CreateFontIndirectW(&lf);
    };
    HFONT f = make(L"Segoe UI Variable Text");
    if (!f) return make(L"Segoe UI");
    HDC dc = GetDC(nullptr);
    HGDIOBJ old = SelectObject(dc, f);
    wchar_t face[LF_FACESIZE] = {};
    GetTextFaceW(dc, LF_FACESIZE, face);
    SelectObject(dc, old);
    ReleaseDC(nullptr, dc);
    if (_wcsicmp(face, L"Segoe UI Variable Text") != 0) {
        DeleteObject(f);
        f = make(L"Segoe UI");
    }
    return f;
}

void ApplyState(Dlg& d) {
    int len = GetWindowTextLengthW(d.edit);
    std::wstring text((size_t)len + 1, L'\0');
    GetWindowTextW(d.edit, text.data(), len + 1);
    text.resize((size_t)len);
    PromptState st = EvaluatePrompt(d.opt.validate, text);
    EnableWindow(d.ok, st.okEnabled ? TRUE : FALSE);
    wchar_t cur[512] = {};
    GetWindowTextW(d.error, cur, ARRAYSIZE(cur));
    if (st.error != cur) {
        SetWindowTextW(d.error, st.error.c_str());
        InvalidateRect(d.hwnd, nullptr, TRUE);
    }
}

void Finish(Dlg& d, bool accept) {
    if (accept) {
        if (!IsWindowEnabled(d.ok)) return;
        int len = GetWindowTextLengthW(d.edit);
        d.result.assign((size_t)len + 1, L'\0');
        GetWindowTextW(d.edit, d.result.data(), len + 1);
        d.result.resize((size_t)len);
    }
    d.accepted = accept;
    d.done = true;
}

LRESULT CALLBACK DlgProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    Dlg* d = reinterpret_cast<Dlg*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    switch (m) {
    case WM_NCCREATE: {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        break;
    }
    case WM_ERASEBKGND:
        if (d) {
            RECT rc;
            GetClientRect(h, &rc);
            FillRect(reinterpret_cast<HDC>(wp), &rc, d->bgBrush);
            return 1;
        }
        break;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLOREDIT:
        if (d) {
            HDC dc = reinterpret_cast<HDC>(wp);
            HWND ctl = reinterpret_cast<HWND>(lp);
            bool isEdit = ctl == d->edit;
            SetTextColor(dc, ctl == d->error ? d->colors.error : d->colors.text);
            SetBkColor(dc, isEdit ? d->colors.editBackground : d->colors.background);
            if (!isEdit) SetBkMode(dc, TRANSPARENT);
            return reinterpret_cast<LRESULT>(isEdit ? d->editBrush : d->bgBrush);
        }
        break;
    case WM_COMMAND:
        if (!d) break;
        switch (LOWORD(wp)) {
        case kIdEdit:
            if (HIWORD(wp) == EN_CHANGE) ApplyState(*d);
            return 0;
        case IDOK: Finish(*d, true); return 0;
        case IDCANCEL: Finish(*d, false); return 0;
        }
        break;
    case DM_GETDEFID: return MAKELRESULT(IDOK, DC_HASDEFID);
    case WM_CLOSE:
        if (d) Finish(*d, false);
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

void EnsureClass() {
    static bool done = false;
    if (done) return;
    done = true;
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = DlgProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);
}

// d must outlive the window.
bool CreateWindowFor(Dlg& d, HWND owner) {
    EnsureClass();
    d.colors = ComputeDialogColors(d.theme);
    d.bgBrush = CreateSolidBrush(d.colors.background);
    d.editBrush = CreateSolidBrush(d.colors.editBackground);
    d.font = MakeFont(d.dpi);

    TextDialogLayout lay0 = ComputeTextDialogLayout(d.dpi, 0);
    int labelH = 0;
    {
        HDC dc = GetDC(nullptr);
        HGDIOBJ old = SelectObject(dc, d.font);
        RECT rc{0, 0, lay0.label.w, 0};
        DrawTextW(dc, d.opt.label.c_str(), (int)d.opt.label.size(), &rc, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
        labelH = rc.bottom - rc.top;
        SelectObject(dc, old);
        ReleaseDC(nullptr, dc);
    }
    TextDialogLayout lay = ComputeTextDialogLayout(d.dpi, labelH);

    const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU, exStyle = WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT;
    RECT frame{0, 0, lay.clientW, lay.clientH};
    AdjustWindowRectExForDpi(&frame, style, FALSE, exStyle, (UINT)d.dpi);
    int w = frame.right - frame.left, hgt = frame.bottom - frame.top;
    int x = 0, y = 0;
    RECT area{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    RECT ref = area;
    if (owner && IsWindow(owner)) {
        GetWindowRect(owner, &ref);
        MONITORINFO mi{sizeof(mi)};
        if (GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST), &mi)) area = mi.rcWork;
    }
    x = ref.left + ((ref.right - ref.left) - w) / 2;
    y = ref.top + ((ref.bottom - ref.top) - hgt) / 2;
    x = std::max<int>(area.left, std::min<int>(x, area.right - w));
    y = std::max<int>(area.top, std::min<int>(y, area.bottom - hgt));

    HINSTANCE inst = GetModuleHandleW(nullptr);
    d.hwnd = CreateWindowExW(exStyle, kClass, d.opt.title.c_str(), style, x, y, w, hgt, owner, nullptr, inst, &d);
    if (!d.hwnd) return false;
    BOOL dark = d.theme.light ? FALSE : TRUE;
    DwmSetWindowAttribute(d.hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));

    auto child = [&](const wchar_t* cls, const wchar_t* text, DWORD st, DWORD ex, const TextDialogRect& r, int id) {
        HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | st, r.x, r.y, r.w, r.h, d.hwnd,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), inst, nullptr);
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(d.font), TRUE);
        return c;
    };
    d.label = child(L"STATIC", d.opt.label.c_str(), SS_LEFT | SS_NOPREFIX, 0, lay.label, kIdLabel);
    d.edit = child(L"EDIT", d.opt.initial.c_str(), ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE, lay.edit, kIdEdit);
    d.error = child(L"STATIC", L"", SS_LEFT | SS_NOPREFIX, 0, lay.error, kIdError);
    d.ok = child(L"BUTTON", d.opt.okText.c_str(), BS_DEFPUSHBUTTON | WS_TABSTOP, 0, lay.ok, IDOK);
    d.cancel = child(L"BUTTON", Tr(L"Cancel", L"취소"), BS_PUSHBUTTON | WS_TABSTOP, 0, lay.cancel, IDCANCEL);
    SendMessageW(d.edit, EM_LIMITTEXT, 4096, 0);
    if (!d.opt.placeholder.empty()) SendMessageW(d.edit, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(d.opt.placeholder.c_str()));
    if (dark) {
        SetWindowTheme(d.ok, L"DarkMode_Explorer", nullptr);
        SetWindowTheme(d.cancel, L"DarkMode_Explorer", nullptr);
        SetWindowTheme(d.edit, L"DarkMode_CFD", nullptr);
    }
    ApplyState(d);
    return true;
}

void Destroy(Dlg& d) {
    if (d.hwnd) DestroyWindow(d.hwnd);
    d.hwnd = nullptr;
    if (d.font) DeleteObject(d.font);
    if (d.bgBrush) DeleteObject(d.bgBrush);
    if (d.editBrush) DeleteObject(d.editBrush);
    d.font = nullptr;
    d.bgBrush = d.editBrush = nullptr;
}

}

TextDialogLayout ComputeTextDialogLayout(int dpi, int labelHeightPx) {
    constexpr int kClientW = 420, kPad = 20, kGapLabelEdit = 8, kEditH = 32, kGapEditError = 4, kErrorH = 36, kGapError = 8,
                  kBtnH = 32, kBtnW = 96, kBtnGap = 8;
    TextDialogLayout l;
    l.clientW = S(kClientW, dpi);
    const int pad = S(kPad, dpi);
    const int inner = l.clientW - 2 * pad;
    int lh = std::max(labelHeightPx, S(20, dpi));
    int y = pad;
    l.label = {pad, y, inner, lh};
    y += lh + S(kGapLabelEdit, dpi);
    l.edit = {pad, y, inner, S(kEditH, dpi)};
    y += l.edit.h + S(kGapEditError, dpi);
    l.error = {pad, y, inner, S(kErrorH, dpi)};
    y += l.error.h + S(kGapError, dpi);
    const int bw = S(kBtnW, dpi), bh = S(kBtnH, dpi), gap = S(kBtnGap, dpi);
    l.cancel = {l.clientW - pad - bw, y, bw, bh};
    l.ok = {l.cancel.x - gap - bw, y, bw, bh};
    l.clientH = y + bh + pad;
    return l;
}

PromptState EvaluatePrompt(const std::function<std::wstring(const std::wstring&)>& validate, const std::wstring& text) {
    PromptState s;
    if (validate) {
        std::wstring err = validate(text);
        s.okEnabled = err.empty();
        if (!s.okEnabled && !text.empty()) s.error = err;
    } else {
        s.okEnabled = text.find_first_not_of(L" \t\r\n") != std::wstring::npos;
    }
    return s;
}

DialogColors ComputeDialogColors(const pui::Theme& t) {
    pui::Color surface = t.flyoutSurface;
    surface = pui::Color(0xFF000000u | (surface.v & 0x00FFFFFFu));
    DialogColors c;
    c.background = Ref(surface);
    c.editBackground = Ref(Flatten(t.ctlInputActive, surface));
    c.text = Ref(Flatten(t.textPrimary, surface));
    c.secondaryText = Ref(Flatten(t.textSecondary, surface));
    c.error = Ref(t.critical);
    return c;
}

std::optional<std::wstring> PromptText(AppContext& ctx, const TextPromptOptions& options) {
    if (OsDialogsBlocked()) return std::nullopt;   // tests never show an OS dialog: cancelled
    Dlg d;
    d.opt = options;
    d.theme = ctx.CurrentTheme();
    HWND owner = ctx.MainHwnd();
    d.dpi = owner ? (int)GetDpiForWindow(owner) : 96;
    if (d.dpi < 96) d.dpi = 96;
    if (!CreateWindowFor(d, owner)) {
        Destroy(d);
        return std::nullopt;
    }
    const bool ownerWasEnabled = owner && IsWindowEnabled(owner);
    if (owner) EnableWindow(owner, FALSE);
    ShowWindow(d.hwnd, SW_SHOW);
    SetForegroundWindow(d.hwnd);
    SetFocus(d.edit);
    SendMessageW(d.edit, EM_SETSEL, 0, -1);

    MSG m;
    while (!d.done) {
        BOOL r = GetMessageW(&m, nullptr, 0, 0);
        if (r <= 0) {
            if (r == 0) PostQuitMessage((int)m.wParam);   // pass WM_QUIT on to the outer loop
            break;
        }
        if (m.message == WM_KEYDOWN && (m.hwnd == d.hwnd || IsChild(d.hwnd, m.hwnd))) {
            if (m.wParam == VK_ESCAPE) {
                SendMessageW(d.hwnd, WM_COMMAND, IDCANCEL, 0);
                continue;
            }
            if (m.wParam == VK_RETURN && m.hwnd == d.edit) {
                SendMessageW(d.hwnd, WM_COMMAND, IDOK, 0);
                continue;
            }
        }
        if (!IsDialogMessageW(d.hwnd, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    // Re-enable the owner first so activation returns to it when the dialog goes away.
    if (owner && ownerWasEnabled) EnableWindow(owner, TRUE);
    Destroy(d);
    if (owner && IsWindow(owner) && IsWindowVisible(owner)) SetForegroundWindow(owner);   // never a hidden (test) owner
    if (d.accepted) return d.result;
    return std::nullopt;
}

namespace {
// The test hook owns its Dlg until the window is destroyed.
std::vector<std::unique_ptr<Dlg>>& TestDialogs() {
    static std::vector<std::unique_ptr<Dlg>> v;
    return v;
}
}

HWND CreateTextDialogWindowHiddenForTest(const TextPromptOptions& options, const pui::Theme& theme, int dpi, HWND owner) {
    auto d = std::make_unique<Dlg>();
    d->opt = options;
    d->theme = theme;
    d->dpi = dpi;
    if (!CreateWindowFor(*d, owner)) return nullptr;
    HWND h = d->hwnd;
    TestDialogs().push_back(std::move(d));
    return h;
}

void DestroyTextDialogWindowHiddenForTest(HWND hwnd) {
    auto& dialogs = TestDialogs();
    auto it = std::find_if(dialogs.begin(), dialogs.end(), [hwnd](const auto& d) { return d && d->hwnd == hwnd; });
    if (it == dialogs.end()) return;
    Destroy(**it);
    dialogs.erase(it);
}

namespace {
bool Inside(const TextDialogRect& r, int w, int h) { return r.x >= 0 && r.y >= 0 && r.right() <= w && r.bottom() <= h && r.w > 0 && r.h > 0; }
bool Disjoint(const TextDialogRect& a, const TextDialogRect& b) { return a.right() <= b.x || b.right() <= a.x || a.bottom() <= b.y || b.bottom() <= a.y; }
}

LOGIC_TEST(textdialog_layout_all_dpis) {
    for (int dpi : {96, 120, 144, 168, 192, 288}) {
        for (int lh : {0, S(20, dpi), S(60, dpi)}) {
            auto l = ComputeTextDialogLayout(dpi, lh);
            LT_CHECK_EQ(l.clientW, S(420, dpi));
            for (const TextDialogRect* r : {&l.label, &l.edit, &l.error, &l.ok, &l.cancel}) LT_CHECK(Inside(*r, l.clientW, l.clientH));
            LT_CHECK(Disjoint(l.label, l.edit));
            LT_CHECK(Disjoint(l.edit, l.error));
            LT_CHECK(Disjoint(l.error, l.ok));
            LT_CHECK(Disjoint(l.ok, l.cancel));
            LT_CHECK_EQ(l.cancel.right() + S(20, dpi), l.clientW);
            LT_CHECK_EQ(l.ok.y, l.cancel.y);
            LT_CHECK_EQ(l.edit.w, l.clientW - 2 * S(20, dpi));
            LT_CHECK(l.edit.h >= S(32, dpi));
            LT_CHECK_EQ(l.clientH, l.ok.bottom() + S(20, dpi));
        }
    }
    LT_CHECK(ComputeTextDialogLayout(96, 80).clientH > ComputeTextDialogLayout(96, 20).clientH);
    LT_CHECK(ComputeTextDialogLayout(192, 40).clientW == 2 * ComputeTextDialogLayout(96, 20).clientW);
}

LOGIC_TEST(textdialog_evaluate_prompt) {
    auto v = [](const std::wstring& t) { return t == L"ok" ? std::wstring() : std::wstring(L"bad"); };
    auto a = EvaluatePrompt(v, L"ok");
    LT_CHECK(a.okEnabled);
    LT_CHECK(a.error.empty());
    auto b = EvaluatePrompt(v, L"nope");
    LT_CHECK(!b.okEnabled);
    LT_CHECK(b.error == L"bad");
    auto c = EvaluatePrompt(v, L"");
    LT_CHECK(!c.okEnabled);
    LT_CHECK(c.error.empty());   // no error shown for an untouched empty field
    LT_CHECK(EvaluatePrompt({}, L"x").okEnabled);
    LT_CHECK(!EvaluatePrompt({}, L"   ").okEnabled);
}

LOGIC_TEST(textdialog_colors_follow_theme) {
    auto dark = ComputeDialogColors(pui::Theme::Make(false));
    auto light = ComputeDialogColors(pui::Theme::Make(true));
    LT_CHECK(dark.background != light.background);
    LT_CHECK(dark.text != light.text);
    LT_CHECK(GetRValue(dark.background) < 0x60);
    LT_CHECK(GetRValue(light.background) > 0xC0);
    LT_CHECK(GetRValue(dark.text) > 0xC0);
    LT_CHECK(GetRValue(light.text) < 0x40);
}

LOGIC_TEST(textdialog_hidden_window_controls) {
    TextPromptOptions o;
    o.title = L"Open URL";
    o.label = L"Enter the address";
    o.okText = L"Open";
    o.placeholder = L"https://";
    o.validate = [](const std::wstring& t) { return t == L"good" ? std::wstring() : std::wstring(L"Invalid address"); };
    HWND owner = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC", L"textdialog-test-owner", WS_POPUP,
                                 -32000, -32000, 480, 320, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    LT_CHECK(owner != nullptr);
    HWND h = CreateTextDialogWindowHiddenForTest(o, pui::Theme::Make(false), 96, owner);
    LT_CHECK(h != nullptr);
    if (!h) { if (owner) DestroyWindow(owner); return; }
    LT_CHECK(GetWindow(h, GW_OWNER) == owner);
    LT_CHECK(!IsWindowVisible(h));
    HWND edit = GetDlgItem(h, kIdEdit), ok = GetDlgItem(h, IDOK), cancel = GetDlgItem(h, IDCANCEL), err = GetDlgItem(h, kIdError);
    LT_CHECK(edit && ok && cancel && err && GetDlgItem(h, kIdLabel));
    if (edit && ok && err) {
        LT_CHECK(!IsWindowEnabled(ok));   // empty text is invalid
        SetWindowTextW(edit, L"bad");     // EN_CHANGE reaches the dialog synchronously
        LT_CHECK(!IsWindowEnabled(ok));
        wchar_t buf[64] = {};
        GetWindowTextW(err, buf, 64);
        LT_CHECK(std::wstring(buf) == L"Invalid address");
        SetWindowTextW(edit, L"good");
        LT_CHECK(IsWindowEnabled(ok));
        GetWindowTextW(err, buf, 64);
        LT_CHECK(std::wstring(buf).empty());
        GetWindowTextW(ok, buf, 64);
        LT_CHECK(std::wstring(buf) == L"Open");
        LT_CHECK((GetWindowLongW(ok, GWL_STYLE) & BS_DEFPUSHBUTTON) != 0);
        LT_CHECK((GetWindowLongW(edit, GWL_STYLE) & ES_AUTOHSCROLL) != 0);
    }
    DestroyTextDialogWindowHiddenForTest(h);
    DestroyWindow(owner);
}

}
