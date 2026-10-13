#include "features/SettingsLogic.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <functional>

#include "BarLogic.h"
#include "LogicTest.h"
#include "Localize.h"
#include "Services.h"
#include "features/FxModel.h"
#include "features/SettingsService.h"
#include "wvp/CapabilitySet.h"
#include "wvp/FilterManager.h"
#include "wvp/PlaybackController.h"
#include "wvp/win32/Utf.h"

namespace app::sl {

std::wstring SubtitleFontSummary(const std::string& family, int weight, bool italic, double points) {
    std::wstring style;
    switch ((std::clamp(weight, 100, 900) + 50) / 100) {
    case 1: style = L"Thin"; break;
    case 2: style = L"ExtraLight"; break;
    case 3: style = L"Light"; break;
    case 5: style = L"Medium"; break;
    case 6: style = L"SemiBold"; break;
    case 7: style = L"Bold"; break;
    case 8: style = L"ExtraBold"; break;
    case 9: style = L"Black"; break;
    default: break;
    }
    if (italic) style += style.empty() ? L"Italic" : L" Italic";
    if (style.empty()) style = L"Regular";
    wchar_t size[32];
    swprintf_s(size, points == std::floor(points) ? L"%.0f pt" : L"%.1f pt", points);
    return wvp::ToWide(family) + L", " + style + L", " + size;
}
std::wstring RgbaHex(std::uint32_t rgba) {
    wchar_t buf[16];
    swprintf_s(buf, L"#%06X", (unsigned)(rgba >> 8));
    return buf;
}
std::wstring PixelLabel(double px) {
    if (px <= 0) return Tr(L"None", L"없음");
    wchar_t buf[32];
    swprintf_s(buf, px == std::floor(px) ? L"%.0f px" : L"%.1f px", px);
    return buf;
}

pui::ThemeMode ToThemeMode(wvp::ThemeSetting s) {
    switch (s) {
    case wvp::ThemeSetting::Light: return pui::ThemeMode::Light;
    case wvp::ThemeSetting::Dark: return pui::ThemeMode::Dark;
    default: return pui::ThemeMode::System;
    }
}
wvp::ThemeSetting ToThemeSetting(pui::ThemeMode m) {
    switch (m) {
    case pui::ThemeMode::Light: return wvp::ThemeSetting::Light;
    case pui::ThemeMode::Dark: return wvp::ThemeSetting::Dark;
    default: return wvp::ThemeSetting::System;
    }
}
int ThemeIndex(wvp::ThemeSetting s) {
    switch (s) {
    case wvp::ThemeSetting::Light: return 1;
    case wvp::ThemeSetting::Dark: return 2;
    default: return 0;
    }
}
wvp::ThemeSetting ThemeFromIndex(int i) {
    return i == 1 ? wvp::ThemeSetting::Light : i == 2 ? wvp::ThemeSetting::Dark : wvp::ThemeSetting::System;
}

const std::array<AccentChoice, kAccentCount>& AccentPalette() {
    static const std::array<AccentChoice, kAccentCount> p = {{
        {Tr(L"Blue", L"파랑"), 0xFF0078D4}, {Tr(L"Sky", L"하늘"), 0xFF0099BC}, {Tr(L"Teal", L"청록"), 0xFF038387}, {Tr(L"Green", L"초록"), 0xFF10893E},
        {Tr(L"Gold", L"황금"), 0xFFFFB900}, {Tr(L"Orange", L"주황"), 0xFFF7630C}, {Tr(L"Red", L"빨강"), 0xFFE81123}, {Tr(L"Pink", L"분홍"), 0xFFE3008C},
        {Tr(L"Purple", L"보라"), 0xFF881798}, {Tr(L"Indigo", L"남색"), 0xFF6B69D6}, {Tr(L"Slate", L"청회색"), 0xFF68768A}, {Tr(L"Gray", L"회색"), 0xFF7A7574},
    }};
    return p;
}
int AccentIndexFor(std::uint32_t argb) {
    if (argb == 0) return -1;
    const auto& p = AccentPalette();
    for (int i = 0; i < kAccentCount; i++)
        if ((p[(size_t)i].argb & 0x00FFFFFFu) == (argb & 0x00FFFFFFu)) return i;
    return -1;
}

int AlwaysOnTopIndex(wvp::AlwaysOnTop m) {
    switch (m) {
        case wvp::AlwaysOnTop::Enable: return 1;
        case wvp::AlwaysOnTop::WhenPlaying: return 2;
        case wvp::AlwaysOnTop::WhenFullscreen: return 3;
        default: return 0;
    }
}
wvp::AlwaysOnTop AlwaysOnTopFromIndex(int i) {
    switch (i) {
        case 1: return wvp::AlwaysOnTop::Enable;
        case 2: return wvp::AlwaysOnTop::WhenPlaying;
        case 3: return wvp::AlwaysOnTop::WhenFullscreen;
        default: return wvp::AlwaysOnTop::Disable;
    }
}
bool ShouldBeTopmost(wvp::AlwaysOnTop mode, bool playing, bool fullscreen) {
    switch (mode) {
        case wvp::AlwaysOnTop::Enable: return true;
        case wvp::AlwaysOnTop::WhenPlaying: return playing;
        case wvp::AlwaysOnTop::WhenFullscreen: return fullscreen;
        default: return false;
    }
}
wvp::AlwaysOnTop NextAlwaysOnTop(wvp::AlwaysOnTop mode) {
    return AlwaysOnTopFromIndex((AlwaysOnTopIndex(mode) + 1) % kAlwaysOnTopCount);
}

TimeDisplayMode ToggleTimeDisplay(TimeDisplayMode m) {
    return m == TimeDisplayMode::Elapsed ? TimeDisplayMode::Remaining : TimeDisplayMode::Elapsed;
}

std::wstring FormatTimeDisplay(TimeDisplayMode m, double position, double duration) {
    if (m == TimeDisplayMode::Elapsed) return FormatTime(position);
    double left = duration > 0 && position >= 0 ? duration - position : 0;
    return L"-" + FormatTime(left);
}

PageState StateFromSettings(const wvp::AppSettings& s) {
    PageState st;
    st.themeIndex = ThemeIndex(s.theme);
    st.useSystemAccent = s.accentColor == 0;
    st.accentIndex = AccentIndexFor(s.accentColor);
    st.alwaysOnTopIndex = AlwaysOnTopIndex(s.alwaysOnTop);
    st.debugLogging = s.debugLogging;
    return st;
}
void ApplyState(wvp::AppSettings& s, const PageState& st) {
    s.theme = ThemeFromIndex(st.themeIndex);
    if (st.useSystemAccent) s.accentColor = 0;
    else if (st.accentIndex >= 0 && st.accentIndex < kAccentCount) s.accentColor = AccentPalette()[(size_t)st.accentIndex].argb;
    else if (s.accentColor == 0) s.accentColor = AccentPalette()[0].argb;   // system accent switched off: start from the first swatch
    s.alwaysOnTop = AlwaysOnTopFromIndex(st.alwaysOnTopIndex);
    s.debugLogging = st.debugLogging;
}

std::optional<wvp::WindowPlacement> ValidatePlacement(const wvp::WindowPlacement& p, const std::vector<MonitorRect>& monitors) {
    if (!p.valid || monitors.empty()) return std::nullopt;
    if (p.w <= 0 || p.h <= 0) return std::nullopt;
    int maxW = 0, maxH = 0;
    for (auto& m : monitors) { maxW = std::max(maxW, m.r - m.l); maxH = std::max(maxH, m.b - m.t); }
    wvp::WindowPlacement q = p;
    q.w = std::clamp(q.w, kMinWindowW, std::max(kMinWindowW, maxW));
    q.h = std::clamp(q.h, kMinWindowH, std::max(kMinWindowH, maxH));
    // A 100 px wide piece of the caption (top 48 px) at the left or right end must overlap a monitor so the window can be dragged.
    const int capH = 48, capW = std::min(q.w, 100);
    bool reachable = false;
    for (auto& m : monitors) {
        int it = std::max(q.y, m.t), ib = std::min(q.y + capH, m.b);
        if (ib - it < 20) continue;
        int rl = std::max(q.x + q.w - capW, m.l), rr = std::min(q.x + q.w, m.r);
        int ll = std::max(q.x, m.l), lr = std::min(q.x + capW, m.r);
        if (rr - rl >= 50 || lr - ll >= 50) { reachable = true; break; }
    }
    if (!reachable) return std::nullopt;
    return q;
}

wvp::WindowPlacement RescalePlacement(const wvp::WindowPlacement& p, int targetDpi) {
    if (p.dpi <= 0 || targetDpi <= 0 || p.dpi == targetDpi) return p;
    wvp::WindowPlacement q = p;
    q.w = (int)std::lround(p.w * (double)targetDpi / p.dpi);
    q.h = (int)std::lround(p.h * (double)targetDpi / p.dpi);
    q.dpi = targetDpi;
    return q;
}

PageLayout ComputePageLayout(float pageW, float pageH) {
    PageLayout l;
    l.header = pui::RectF::XYWH(kPageMarginH, kHeaderTop, std::max(0.f, pageW - 2 * kPageMarginH), kHeaderH);
    float top = kHeaderTop + kHeaderH + 4;
    l.scroll = pui::RectF{0, top, std::max(0.f, pageW), std::max(top, pageH)};
    float avail = pageW - 2 * kPageMarginH;
    l.contentPadH = kPageMarginH + (avail > kMaxContentW ? (avail - kMaxContentW) * 0.5f : 0.f);
    return l;
}

RowLayoutResult ComputeRowLayout(const RowInput& in) {
    RowLayoutResult o;
    const bool hasDesc = in.descH > 0;
    const float gap = hasDesc ? 2.f : 0.f;
    const float textH = in.titleH + gap + in.descH;
    o.textLeft = kRowPad + (in.hasGlyph ? kGlyphCol : 0.f);
    const float vpad = 13.f;
    if (!in.stacked) {
        float h = std::max({hasDesc ? kRowMinHDesc : kRowMinH, textH + 2 * vpad, in.controlH + 24.f});
        o.height = h;
        float cl = std::max(o.textLeft, in.width - kRowPad - in.controlW);
        o.control = pui::RectF::XYWH(cl, (h - in.controlH) * 0.5f, std::min(in.controlW, std::max(0.f, in.width - kRowPad - o.textLeft)), in.controlH);
        o.textRight = std::max(o.textLeft, cl - 16.f);
        float ty = (h - textH) * 0.5f;
        float tw = std::max(0.f, o.textRight - o.textLeft);
        o.title = pui::RectF::XYWH(o.textLeft, ty, tw, in.titleH);
        o.desc = hasDesc ? pui::RectF::XYWH(o.textLeft, ty + in.titleH + gap, tw, in.descH) : pui::RectF{};
    } else {
        o.textRight = std::max(o.textLeft, in.width - kRowPad);
        float tw = o.textRight - o.textLeft;
        o.title = pui::RectF::XYWH(o.textLeft, vpad, tw, in.titleH);
        o.desc = hasDesc ? pui::RectF::XYWH(o.textLeft, vpad + in.titleH + gap, tw, in.descH) : pui::RectF{};
        float cy = vpad + textH + 10.f;
        o.control = pui::RectF::XYWH(o.textLeft, cy, std::min(in.controlW, tw), in.controlH);
        o.height = cy + in.controlH + 14.f;
    }
    float gy = in.stacked ? o.title.t + (in.titleH - 16.f) * 0.5f : (o.height - 16.f) * 0.5f;
    o.glyph = pui::RectF::XYWH(kRowPad, gy, 16, 16);
    return o;
}

std::vector<SwatchCell> ComputeSwatchGrid(int count, float size, float gap, float maxWidth) {
    std::vector<SwatchCell> v;
    float x = 0, y = 0;
    for (int i = 0; i < count; i++) {
        if (x > 0 && x + size > maxWidth) { x = 0; y += size + gap; }
        v.push_back({x, y});
        x += size + gap;
    }
    return v;
}
pui::SizeF SwatchGridSize(int count, float size, float gap, float maxWidth) {
    auto g = ComputeSwatchGrid(count, size, gap, maxWidth);
    float w = 0, h = 0;
    for (auto& c : g) { w = std::max(w, c.x + size); h = std::max(h, c.y + size); }
    return {w, h};
}

namespace {
struct TempDir {
    std::filesystem::path p;
    TempDir() {
        wchar_t buf[MAX_PATH]{};
        GetTempPathW(MAX_PATH, buf);
        p = std::filesystem::path(buf) / (L"aribplayer-settings-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        std::filesystem::create_directories(p);
    }
    ~TempDir() { std::error_code ec; std::filesystem::remove_all(p, ec); }
};
}

LOGIC_TEST(settings_pin_cycle_and_time_toggle) {
    using wvp::AlwaysOnTop;
    AlwaysOnTop m = AlwaysOnTop::Disable;
    const AlwaysOnTop want[] = {AlwaysOnTop::Enable, AlwaysOnTop::WhenPlaying, AlwaysOnTop::WhenFullscreen, AlwaysOnTop::Disable};
    for (AlwaysOnTop w : want) { m = NextAlwaysOnTop(m); LT_CHECK(m == w); }
    LT_CHECK(ToggleTimeDisplay(TimeDisplayMode::Elapsed) == TimeDisplayMode::Remaining);
    LT_CHECK(ToggleTimeDisplay(TimeDisplayMode::Remaining) == TimeDisplayMode::Elapsed);
    LT_CHECK(FormatTimeDisplay(TimeDisplayMode::Elapsed, 83, 200) == L"01:23");
    LT_CHECK(FormatTimeDisplay(TimeDisplayMode::Remaining, 160, 200) == L"-00:40");
    LT_CHECK(FormatTimeDisplay(TimeDisplayMode::Remaining, 5, 0) == L"-00:00");
}

LOGIC_TEST(settings_theme_mapping) {
    LT_CHECK(ToThemeMode(wvp::ThemeSetting::System) == pui::ThemeMode::System);
    LT_CHECK(ToThemeMode(wvp::ThemeSetting::Light) == pui::ThemeMode::Light);
    LT_CHECK(ToThemeMode(wvp::ThemeSetting::Dark) == pui::ThemeMode::Dark);
    for (auto s : {wvp::ThemeSetting::System, wvp::ThemeSetting::Light, wvp::ThemeSetting::Dark}) {
        LT_CHECK(ToThemeSetting(ToThemeMode(s)) == s);
        LT_CHECK(ThemeFromIndex(ThemeIndex(s)) == s);
    }
    LT_CHECK_EQ(ThemeIndex(wvp::ThemeSetting::Dark), 2);
    LT_CHECK(ThemeFromIndex(-1) == wvp::ThemeSetting::System);
    LT_CHECK(ThemeFromIndex(9) == wvp::ThemeSetting::System);
}

LOGIC_TEST(settings_accent_palette) {
    const auto& p = AccentPalette();
    LT_CHECK_EQ((int)p.size(), kAccentCount);
    for (int i = 0; i < kAccentCount; i++) {
        LT_CHECK_EQ((unsigned)(p[(size_t)i].argb >> 24), 0xFFu);
        LT_CHECK_EQ(AccentIndexFor(p[(size_t)i].argb), i);                // unique
    }
    LT_CHECK_EQ(AccentIndexFor(0), -1);
    LT_CHECK_EQ(AccentIndexFor(0xFF123456), -1);
    LT_CHECK_EQ(AccentIndexFor(0x800078D4), 0);   // alpha ignored
}

LOGIC_TEST(settings_state_mapping) {
    wvp::AppSettings s;
    PageState st = StateFromSettings(s);
    LT_CHECK(st.useSystemAccent);
    LT_CHECK_EQ(st.themeIndex, 0);
    LT_CHECK_EQ(st.alwaysOnTopIndex, 0);
    st.themeIndex = 2; st.useSystemAccent = false; st.accentIndex = 6;
    ApplyState(s, st);
    LT_CHECK(s.theme == wvp::ThemeSetting::Dark);
    LT_CHECK_EQ((unsigned)s.accentColor, (unsigned)AccentPalette()[6].argb);
    LT_CHECK(!s.debugLogging);
    st.debugLogging = true; ApplyState(s, st);
    LT_CHECK(s.debugLogging && StateFromSettings(s).debugLogging);
    LT_CHECK(StateFromSettings(s) == st);
    st.useSystemAccent = true; ApplyState(s, st);
    LT_CHECK_EQ((unsigned)s.accentColor, 0u);
    s.accentColor = 0xFF123456;                       // custom accent kept when the page shows no swatch
    PageState c = StateFromSettings(s);
    LT_CHECK(!c.useSystemAccent && c.accentIndex == -1);
    ApplyState(s, c);
    LT_CHECK_EQ((unsigned)s.accentColor, 0xFF123456u);
    wvp::AppSettings z; PageState zs = StateFromSettings(z); zs.useSystemAccent = false; ApplyState(z, zs);
    LT_CHECK_EQ((unsigned)z.accentColor, (unsigned)AccentPalette()[0].argb);
}

LOGIC_TEST(always_on_top_evaluation) {
    using M = wvp::AlwaysOnTop;
    for (bool playing : {false, true})
        for (bool fs : {false, true}) {
            LT_CHECK(!ShouldBeTopmost(M::Disable, playing, fs));
            LT_CHECK(ShouldBeTopmost(M::Enable, playing, fs));
            LT_CHECK(ShouldBeTopmost(M::WhenPlaying, playing, fs) == playing);
            LT_CHECK(ShouldBeTopmost(M::WhenFullscreen, playing, fs) == fs);
        }
    LT_CHECK(!ShouldBeTopmost(M::WhenPlaying, false, true) && ShouldBeTopmost(M::WhenPlaying, true, true));
    LT_CHECK(ShouldBeTopmost(M::WhenPlaying, true, false) && !ShouldBeTopmost(M::WhenPlaying, false, false));
    LT_CHECK(!ShouldBeTopmost(M::WhenFullscreen, true, false) && ShouldBeTopmost(M::WhenFullscreen, true, true));
    LT_CHECK(ShouldBeTopmost(M::WhenFullscreen, false, true) && !ShouldBeTopmost(M::WhenFullscreen, false, false));
    for (int i = 0; i < kAlwaysOnTopCount; i++) LT_CHECK_EQ(AlwaysOnTopIndex(AlwaysOnTopFromIndex(i)), i);
    LT_CHECK(AlwaysOnTopFromIndex(-1) == M::Disable && AlwaysOnTopFromIndex(9) == M::Disable);
    wvp::AppSettings s; PageState st = StateFromSettings(s);
    st.alwaysOnTopIndex = 3; ApplyState(s, st);
    LT_CHECK(s.alwaysOnTop == M::WhenFullscreen && StateFromSettings(s).alwaysOnTopIndex == 3);
}

LOGIC_TEST(settings_service_roundtrip_temp_dir) {
    TempDir td;
    app::Services svc(td.p.wstring());
    LT_CHECK(svc.settingsStore.Path().rfind(wvp::ToUtf8(td.p.wstring()), 0) == 0);
    svc.settings.theme = wvp::ThemeSetting::Light;
    svc.settings.accentColor = AccentPalette()[3].argb;
    svc.settings.alwaysOnTop = wvp::AlwaysOnTop::WhenPlaying;
    svc.settings.showSuperimpose = false;
    svc.settings.window = {10, 20, 1000, 600, true, true};
    LT_CHECK(svc.settingsStore.Save(svc.settings));
    app::Services again(td.p.wstring());
    again.settings = again.settingsStore.Load();
    LT_CHECK(again.settings == svc.settings);
}

LOGIC_TEST(settings_placement_validation) {
    std::vector<MonitorRect> mons = {{0, 0, 1920, 1080}, {1920, 0, 3840, 1080}};
    wvp::WindowPlacement ok{100, 100, 1280, 720, false, true};
    auto v = ValidatePlacement(ok, mons);
    LT_CHECK(v.has_value() && *v == ok);
    wvp::WindowPlacement invalid = ok; invalid.valid = false;
    LT_CHECK(!ValidatePlacement(invalid, mons).has_value());
    wvp::WindowPlacement second{2000, 50, 1000, 600, false, true};          // on the second monitor
    LT_CHECK(ValidatePlacement(second, mons).has_value());
    LT_CHECK(!ValidatePlacement(second, {{0, 0, 1920, 1080}}).has_value());  // second monitor unplugged
    wvp::WindowPlacement off{5000, 5000, 1000, 600, false, true};
    LT_CHECK(!ValidatePlacement(off, mons).has_value());
    wvp::WindowPlacement under{100, 2000, 1000, 600, false, true};          // caption below the screen
    LT_CHECK(!ValidatePlacement(under, mons).has_value());
    wvp::WindowPlacement partial{-900, 100, 1000, 600, false, true};        // mostly off to the left, right end reachable
    LT_CHECK(ValidatePlacement(partial, mons).has_value());
    wvp::WindowPlacement tiny{100, 100, 10, 10, false, true};
    auto t = ValidatePlacement(tiny, mons);
    LT_CHECK(t.has_value() && t->w == kMinWindowW && t->h == kMinWindowH);
    wvp::WindowPlacement huge{0, 0, 9000, 9000, true, true};
    auto hg = ValidatePlacement(huge, mons);
    LT_CHECK(hg.has_value() && hg->w == 1920 && hg->h == 1080 && hg->maximized);
    LT_CHECK(!ValidatePlacement(ok, {}).has_value());
}

LOGIC_TEST(settings_placement_dpi_rescale) {
    wvp::WindowPlacement p{100, 50, 1200, 800, false, true, 96};
    auto same = RescalePlacement(p, 96);
    LT_CHECK(same == p);
    auto up = RescalePlacement(p, 192);                 // saved on a 100% monitor, restored on a 200% one
    LT_CHECK_EQ(up.w, 2400); LT_CHECK_EQ(up.h, 1600); LT_CHECK_EQ(up.dpi, 192);
    LT_CHECK_EQ(up.x, 100); LT_CHECK_EQ(up.y, 50);      // screen position is not scaled
    wvp::WindowPlacement hi{0, 0, 2400, 1600, true, true, 192};
    auto down = RescalePlacement(hi, 144);
    LT_CHECK_EQ(down.w, 1800); LT_CHECK_EQ(down.h, 1200); LT_CHECK(down.maximized && down.valid);
    LT_CHECK(RescalePlacement(p, 0) == p);              // unknown target DPI: untouched
    wvp::WindowPlacement legacy{0, 0, 1000, 600, false, true};   // dpi defaults to 96
    LT_CHECK_EQ(legacy.dpi, 96);
    LT_CHECK_EQ(RescalePlacement(legacy, 120).w, 1250);
}

LOGIC_TEST(settings_page_layout) {
    for (float w : {500.f, 600.f, 752.f, 1280.f, 2400.f}) {
        PageLayout l = ComputePageLayout(w, w == 500.f ? 244.f : 720.f);
        float h = w == 500.f ? 244.f : 720.f;
        LT_CHECK(l.header.l == kPageMarginH && l.header.r <= w);
        LT_CHECK(l.scroll.t >= l.header.b && l.scroll.b == h && l.scroll.w() == w);
        float colW = w - 2 * l.contentPadH;
        LT_CHECK(colW <= kMaxContentW + 0.01f);
        LT_CHECK(l.contentPadH >= kPageMarginH);
        if (w == 500.f) LT_CHECK_EQ(colW, 428.f);   // 500-DIP minimum window: the page is the full width
    }
    LT_CHECK_EQ(ComputePageLayout(2400, 720).contentPadH, kPageMarginH + (2400 - 72 - 1000) / 2.f);
}

LOGIC_TEST(settings_row_layout) {
    RowInput in; in.width = 800; in.controlW = 100; in.controlH = 32; in.titleH = 20; in.descH = 16;
    RowLayoutResult r = ComputeRowLayout(in);
    LT_CHECK(r.height >= kRowMinHDesc);
    LT_CHECK(r.control.r <= 800 - kRowPad + 0.01f);
    LT_CHECK(r.control.t >= 0 && r.control.b <= r.height);
    LT_CHECK(r.title.r <= r.control.l && r.desc.r <= r.control.l);
    LT_CHECK(r.title.b <= r.desc.t && r.desc.b <= r.height);
    LT_CHECK_EQ(r.title.l, kRowPad + kGlyphCol);
    in.hasGlyph = false; in.descH = 0;
    RowLayoutResult n = ComputeRowLayout(in);
    LT_CHECK_EQ(n.title.l, kRowPad);
    LT_CHECK(n.desc.Empty());
    LT_CHECK(n.height >= kRowMinH);
    in.controlH = 100;                                 // tall controls grow the row and stay vertically centred
    RowLayoutResult t = ComputeRowLayout(in);
    LT_CHECK(t.height >= 124 && std::fabs(t.control.cy() - t.height / 2) < 0.01f);
    in.width = 150; in.controlW = 300; in.controlH = 32; in.hasGlyph = true;   // narrow: nothing negative or overflowing
    RowLayoutResult nw = ComputeRowLayout(in);
    LT_CHECK(nw.control.l >= nw.textLeft && nw.title.w() >= 0 && nw.control.r <= 150);
    in.width = 380; in.controlW = 200; in.descH = 40;
    RowLayoutResult minWindow = ComputeRowLayout(in);   // settings column at a 500-DIP shell minimum
    LT_CHECK(minWindow.height >= kRowMinHDesc && minWindow.title.l == minWindow.textLeft);
    LT_CHECK(minWindow.title.l >= 0 && minWindow.title.r <= 380 && minWindow.desc.r <= 380);
    LT_CHECK(minWindow.control.l >= minWindow.textLeft && minWindow.control.r <= 380);
    LT_CHECK(minWindow.control.t >= 0 && minWindow.control.b <= minWindow.height);
    RowInput s; s.width = 800; s.controlW = 300; s.controlH = 96; s.descH = 16; s.stacked = true;
    RowLayoutResult st = ComputeRowLayout(s);
    LT_CHECK(st.control.t >= st.desc.b && st.control.b <= st.height);
    LT_CHECK_EQ(st.control.l, st.textLeft);
    s.width = 200; s.controlW = 500;
    LT_CHECK(ComputeRowLayout(s).control.r <= 200);
}

LOGIC_TEST(settings_swatch_grid) {
    auto g = ComputeSwatchGrid(12, 32, 8, 1000);
    LT_CHECK_EQ((int)g.size(), 12);
    for (auto& c : g) LT_CHECK_EQ(c.y, 0.f);                  // one line
    LT_CHECK_EQ(g[1].x, 40.f);
    auto w = ComputeSwatchGrid(12, 32, 8, 200);               // wraps: 5 per line
    LT_CHECK_EQ(w[5].y, 40.f);
    LT_CHECK_EQ(w[5].x, 0.f);
    pui::SizeF sz = SwatchGridSize(12, 32, 8, 200);
    LT_CHECK(sz.w <= 200 && sz.h == 40 * 2 + 32);
    LT_CHECK_EQ(SwatchGridSize(0, 32, 8, 200).w, 0.f);
}

namespace {
class LiveFake : public wvp::IMediaPlayer {
public:
    std::vector<std::pair<std::string, wvp::OpenOptions>> opened;
    std::vector<std::string> vf, af;
    wvp::PlayerState state = wvp::PlayerState::Idle;
    double pos = 0;
    int pauses = 0;
    wvp::PlayerState State() const override { return state; }
    int Volume() const override { return 100; }
    void SetVolume(int) override {}
    bool IsMuted() const override { return false; }
    void SetMuted(bool) override {}
    double Position() override { return pos; }
    double Duration() override { return 100; }
    int Open(const std::string& u, const wvp::OpenOptions& o) override {
        opened.emplace_back(u, o);
        bool startPaused = false;   // libffplay ends such an open paused with the landing frame shown
        for (auto& e : o) if (e.first == "start_paused" && e.second == "1") startPaused = true;
        state = startPaused ? wvp::PlayerState::Paused : wvp::PlayerState::Playing;
        return 0;
    }
    int Stop() override { state = wvp::PlayerState::Stopped; return 0; }
    int Play() override { return 0; }
    int Pause() override { pauses++; state = wvp::PlayerState::Paused; return 0; }
    int TogglePause() override { return 0; }
    int Seek(double, bool) override { return 0; }
    int CycleStream(wvp::StreamType) override { return 0; }
    int SetVideoFilter(const std::string& v) override { vf.push_back(v); return 0; }
    int SetAudioFilter(const std::string& a) override { af.push_back(a); return 0; }
    int SendFilterCommand(const std::string&, const std::string&, const std::string&) override { return 0; }
    std::optional<wvp::MediaInfo> GetMediaInfo() override { return std::nullopt; }
    std::optional<wvp::BgraFrame> Snapshot() override { return std::nullopt; }
};
std::string OptOf(const wvp::OpenOptions& o, const char* k) {
    for (auto& e : o) if (e.first == k) return e.second;
    return "<none>";
}
}

namespace {
// A FilterManager planning for 1080p H.264 on an NVIDIA GPU.
void PlanFor(wvp::FilterManager& fm) {
    fm.SetContent({"h264", 8, 1920, 1080, false});
    fm.SetCapabilities(wvp::fakecaps::Nvidia());
}
}

LOGIC_TEST(settings_every_open_path_waits_for_the_plan) {
    std::vector<wvp::MediaItem> one = {wvp::MediaItem("C:\\a.ts")};
    std::vector<std::function<void(wvp::PlaybackController&)>> paths = {
        [&](wvp::PlaybackController& c) { c.SetQueue(one, 0, true); },        // command line / drop / recent
        [&](wvp::PlaybackController& c) { c.Enqueue(one); c.Play(); },        // single instance / add to queue
        [&](wvp::PlaybackController& c) { c.Enqueue(one); c.PlayAt(0); },     // pending queue
        [&](wvp::PlaybackController& c) { c.SetQueue({wvp::MediaItem("a.mp4"), wvp::MediaItem("b.mp4")}, 0, true); c.Next(); },
    };
    for (auto& open : paths) {
        LiveFake p; wvp::PlaybackController c(p); wvp::FilterManager fm;
        c.SetFilters(fm.Provider());
        open(c);
        LT_CHECK(p.opened.empty() && c.HasPendingOpen());   // no capabilities yet: nothing is opened
        c.ApplyFilterChange(fm.SetCapabilities(wvp::fakecaps::Nvidia()));
        LT_CHECK_EQ(p.opened.size(), (size_t)1);
        LT_CHECK_EQ(OptOf(p.opened[0].second, "hwaccel"), std::string("d3d11va"));
        LT_CHECK(!c.HasPendingOpen());
    }
}

LOGIC_TEST(settings_live_reopen_while_paused_presents_a_frame) {
    LiveFake p; wvp::PlaybackController c(p); wvp::FilterManager fm;
    wvp::AppSettings before, after;
    PlanFor(fm);
    c.SetFilters(fm.Provider());
    c.SetQueue({wvp::MediaItem("m.ts")}, 0, true);
    p.pos = 42.5; p.state = wvp::PlayerState::Paused;
    after.showSuperimpose = false;
    LiveApplyResult r = ApplySettingsLive(c, before, after);
    LT_CHECK(r.reopened && !r.toast.empty());
    LT_CHECK_EQ(OptOf(p.opened[1].second, "hwaccel"), OptOf(p.opened[0].second, "hwaccel"));
    LT_CHECK(OptOf(p.opened[1].second, "ss").rfind("42.5", 0) == 0);
    p.Prepared.raise(wvp::MediaPreparedInfo{});
    LT_CHECK_EQ(OptOf(p.opened[1].second, "start_paused"), std::string("1"));
    LT_CHECK(p.pauses == 0 && p.state == wvp::PlayerState::Paused);   // the landing frame is shown, still paused
}

LOGIC_TEST(settings_filters_survive_restart_and_next_file) {
    TempDir td;
    app::Services first(td.p.wstring());
    wvp::FilterSettings edited;
    edited.deinterlace = wvp::DeinterlaceMode::On;
    edited.normalize = true;
    edited.speed = 1.5;
    first.filters.Apply(edited);
    first.settings.filters = first.filters.Settings();    // shutdown snapshot
    LT_CHECK(first.settingsStore.Save(first.settings));

    app::Services restarted(td.p.wstring());
    restarted.settings = restarted.settingsStore.Load();
    restarted.filters.Apply(restarted.settings.filters); // startup, before first open
    restarted.filters.SetCapabilities(wvp::fakecaps::None());
    LT_CHECK(restarted.filters.Settings() == edited);
    LiveFake p;
    wvp::PlaybackController c(p);
    c.SetFilters(restarted.filters.Provider());
    c.SetQueue({wvp::MediaItem("a.ts"), wvp::MediaItem("b.ts")}, 0, true);
    LT_CHECK_EQ(p.opened.size(), (size_t)1);
    LT_CHECK(OptOf(p.opened[0].second, "vf").find("yadif") != std::wstring::npos);
    LT_CHECK(OptOf(p.opened[0].second, "af").find("atempo") != std::wstring::npos);
    c.Next();
    LT_CHECK_EQ(p.opened.size(), (size_t)2);
    LT_CHECK_EQ(OptOf(p.opened[1].second, "vf"), OptOf(p.opened[0].second, "vf"));
    LT_CHECK_EQ(OptOf(p.opened[1].second, "af"), OptOf(p.opened[0].second, "af"));
    LT_CHECK(restarted.filters.Settings() == edited);
}

LOGIC_TEST(settings_zoom_crop_survives_unrelated_edit_and_restart) {
    TempDir td;
    app::Services first(td.p.wstring());
    app::fx::ZoomState zoom;
    zoom.on = true;
    zoom.aspect = 2.35;
    wvp::FilterSettings user;
    user.customVideoFilter = "hue=s=0";
    app::fx::ApplyUser(first.filters, user, zoom);
    first.settings.filters = first.filters.Settings();
    LT_CHECK(first.settings.filters.customVideoFilter.find(app::fx::kZoomInstance) != std::wstring::npos);
    LT_CHECK(first.settingsStore.Save(first.settings));

    app::Services restarted(td.p.wstring());
    restarted.settings = restarted.settingsStore.Load();
    restarted.filters.Apply(restarted.settings.filters);
    zoom.on = restarted.settings.filters.customVideoFilter.find(app::fx::kZoomInstance) != std::wstring::npos;
    auto edited = app::fx::UserView(restarted.filters.Settings());
    edited.speed = 1.25;                    // an unrelated edit must retain the zoom crop
    app::fx::ApplyUser(restarted.filters, edited, zoom);
    LT_CHECK(restarted.filters.Settings().customVideoFilter == first.settings.filters.customVideoFilter);
    restarted.settings.filters = restarted.filters.Settings();
    LT_CHECK(restarted.settingsStore.Save(restarted.settings));
    app::Services again(td.p.wstring());
    again.settings = again.settingsStore.Load();
    LT_CHECK(again.settings.filters.customVideoFilter == first.settings.filters.customVideoFilter);
    LT_CHECK_EQ(again.settings.filters.speed, 1.25);
}

LOGIC_TEST(settings_build_stamp_format) {
    LT_CHECK_EQ(FormatBuildTime(0), std::wstring());
    std::wstring s = FormatBuildTime(133700000000000000ULL);
    LT_CHECK(s.rfind(L"built 20", 0) == 0 && s.size() == 25);
    std::wstring e = ExeBuildStamp();
    LT_CHECK(e.find(L".exe built 20") != std::wstring::npos);
}

LOGIC_TEST(settings_subtitle_value_labels) {
    LT_CHECK(SubtitleFontSummary("Malgun Gothic", 400, false, 45) == L"Malgun Gothic, Regular, 45 pt");
    LT_CHECK(SubtitleFontSummary("Gulim", 700, true, 36.5) == L"Gulim, Bold Italic, 36.5 pt");
    LT_CHECK(SubtitleFontSummary("A", 300, true, 12) == L"A, Light Italic, 12 pt");
    LT_CHECK(RgbaHex(0x11AAFFFFu) == L"#11AAFF");
}

LOGIC_TEST(settings_live_filters_in_planner_mode_reopen_when_the_decoder_must_change) {
    LiveFake p; wvp::PlaybackController c(p); wvp::FilterManager fm;
    fm.SetContent({"mpeg2video", 8, 1920, 1080, false});
    fm.SetCapabilities(wvp::fakecaps::Nvidia());
    // d3d11va would take ivtc_d3d11 without a reopen; the CPU recipe is cheapest after software decoding of MPEG-2
    fm.Blacklist("filter:ivtc_d3d11");
    c.SetFilters(fm.Provider());
    c.SetQueue({wvp::MediaItem("m.ts")}, 0, true);
    p.pos = 7.5; p.state = wvp::PlayerState::Playing;
    const size_t vfCalls = p.vf.size();
    wvp::FilterSettings edited;
    edited.deinterlace = wvp::DeinterlaceMode::Ivtc;
    wvp::FilterChange fc = fm.Apply(edited);
    c.ApplyFilterChange(fc);
    LT_CHECK(fc.reopen);
    LT_CHECK_EQ(p.opened.size(), (size_t)2);
    LT_CHECK_EQ(p.vf.size(), vfCalls);
    LT_CHECK_EQ(OptOf(p.opened[1].second, "vf"), std::string("fieldmatch,bwdif=mode=send_frame:parity=auto:deint=interlaced,decimate"));
    LT_CHECK_EQ(OptOf(p.opened[1].second, "hwaccel"), std::string("<none>"));
    size_t opened = p.opened.size();
    c.ApplyFilterChange(fc);
    LT_CHECK_EQ(p.opened.size(), opened + 1);
    LT_CHECK(app::fx::HasEffect(fc));
    LT_CHECK(!app::fx::HasEffect(wvp::FilterChange{}));
}

}
