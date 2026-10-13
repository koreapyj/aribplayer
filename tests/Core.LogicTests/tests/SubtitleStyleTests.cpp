#include "wvptest.h"

#include "wvp/SubtitleStyle.h"

using namespace wvp;

namespace {

constexpr unsigned kAppDefines = kSubFont | kSubFontStyle | kSubSize | kSubTextColor | kSubOutlineColor | kSubBorder;

SubtitleStyle AppSource() {
    SubtitleStyle s;
    s.useSystemCaptionStyle = false;
    s.font = "Gulim";
    s.fontWeight = 700;
    s.italic = true;
    s.fontSizePt = 30;
    s.primaryRgba = 0xFFFF00FFu;
    s.outlineRgba = 0x112233FFu;
    s.outlineWidth = 4;
    return s;
}

WindowsCaptionProperties AllWindows() {
    WindowsCaptionProperties w;
    w.fontStyle = 3;           // MonospacedWithoutSerifs
    w.fontSize = 3;            // 150 %
    w.fontColor = 6;           // Yellow
    w.fontOpacity = 2;         // 75 %
    w.fontEffect = 5;          // DropShadow
    w.backgroundColor = 5;     // Blue
    w.backgroundOpacity = 3;   // 25 %
    return w;
}

}

TEST_CASE(Subtitle_windows_caption_font_styles_map_to_the_windows_families) {
    const char* families[] = {"Courier New", "Times New Roman", "Consolas", "Tahoma", "Segoe Print", "Segoe Script", "Tahoma"};
    for (int i = 1; i <= 7; i++) {
        WindowsCaptionProperties w;
        w.fontStyle = i;
        auto m = MapWindowsCaptionProperties(w);
        REQUIRE(m.font.has_value());
        CHECK_EQ(*m.font, std::string(families[i - 1]));
    }
}

TEST_CASE(Subtitle_windows_caption_defaults_define_nothing) {
    auto m = MapWindowsCaptionProperties(WindowsCaptionProperties{});
    CHECK(!m.font && !m.fontScale && !m.textRgba && !m.edge && !m.backRgba);
    WindowsCaptionProperties embossed;
    embossed.fontEffect = 2;   // Raised
    CHECK(!MapWindowsCaptionProperties(embossed).edge);
    embossed.fontEffect = 3;   // Depressed
    CHECK(!MapWindowsCaptionProperties(embossed).edge);

    SubtitleStyle s = AppSource();
    s.useSystemCaptionStyle = true;
    s.overrideFont = s.overrideSize = s.overrideColors = s.overrideBorder = true;
    SubtitleRenderStyle r = ResolveSubtitleStyle(s, WindowsCaptionProperties{});
    CHECK_EQ(r.fallbackFlags, 0u);
    CHECK_EQ(r.overrideFlags, 0u);
}

TEST_CASE(Subtitle_windows_caption_values) {
    auto m = MapWindowsCaptionProperties(AllWindows());
    CHECK(m.fontScale && *m.fontScale == 1.5);
    CHECK(m.textRgba && *m.textRgba == 0xFFFF00BFu);
    CHECK(m.edge && *m.edge == CaptionEdge::DropShadow);
    CHECK(m.backRgba && *m.backRgba == 0x0000FF40u);

    WindowsCaptionProperties opacityOnly;   // the colour stays the Windows default look (white text, black background)
    opacityOnly.fontOpacity = 3;
    opacityOnly.backgroundOpacity = 4;
    auto o = MapWindowsCaptionProperties(opacityOnly);
    CHECK(o.textRgba && *o.textRgba == 0xFFFFFF40u);
    CHECK(o.backRgba && *o.backRgba == 0x00000000u);
    WindowsCaptionProperties colourOnly;
    colourOnly.fontColor = 3;   // Red, opaque
    CHECK(*MapWindowsCaptionProperties(colourOnly).textRgba == 0xFF0000FFu);
}

TEST_CASE(Subtitle_windows_source_ignores_the_app_values) {
    SubtitleStyle s = AppSource();
    s.useSystemCaptionStyle = true;
    SubtitleRenderStyle r = ResolveSubtitleStyle(s, AllWindows());
    CHECK_EQ(r.fallbackFlags, kSubFont | kSubSize | kSubTextColor | kSubBorder | kSubBackground);
    CHECK_EQ(r.overrideFlags, 0u);
    CHECK_EQ(r.font, std::string("Consolas"));
    CHECK(r.fontSizePt == 0 && r.fontScale == 1.5);
    CHECK_EQ(r.fontWeight, 400);
    CHECK(!r.italic);
    CHECK_EQ(r.primaryRgba, 0xFFFF00BFu);
    CHECK_EQ(r.outlineRgba, SubtitleRenderStyle{}.outlineRgba);
    CHECK(r.outlineWidth == 0 && r.shadow == kSystemCaptionShadowPx);
    CHECK_EQ(r.backRgba, 0x0000FF40u);

    WindowsCaptionProperties uniform;
    uniform.fontEffect = 4;
    r = ResolveSubtitleStyle(s, uniform);
    CHECK_EQ(r.fallbackFlags, (unsigned)kSubBorder);
    CHECK(r.outlineWidth == kSystemCaptionOutlinePx && r.shadow == 0);
    uniform.fontEffect = 1;   // None
    r = ResolveSubtitleStyle(s, uniform);
    CHECK(r.outlineWidth == 0 && r.shadow == 0);
}

TEST_CASE(Subtitle_app_source_defines_everything_but_the_background) {
    SubtitleRenderStyle r = ResolveSubtitleStyle(AppSource(), AllWindows());
    CHECK_EQ(r.fallbackFlags, kAppDefines);
    CHECK_EQ(r.overrideFlags, 0u);
    CHECK_EQ(r.font, std::string("Gulim"));
    CHECK(r.fontWeight == 700 && r.italic);
    CHECK(r.fontSizePt == 30 && r.fontScale == 1.0);
    CHECK(r.primaryRgba == 0xFFFF00FFu && r.outlineRgba == 0x112233FFu);
    CHECK(r.outlineWidth == 4 && r.shadow == 0);
}

TEST_CASE(Subtitle_override_switches_select_their_properties_of_the_active_source) {
    SubtitleStyle s = AppSource();
    s.overrideFont = true;
    CHECK_EQ(ResolveSubtitleStyle(s, {}).overrideFlags, kSubFont | kSubFontStyle);
    s.overrideFont = false;
    s.overrideSize = true;
    CHECK_EQ(ResolveSubtitleStyle(s, {}).overrideFlags, (unsigned)kSubSize);
    s.overrideSize = false;
    s.overrideColors = true;   // the app has no background
    CHECK_EQ(ResolveSubtitleStyle(s, {}).overrideFlags, kSubTextColor | kSubOutlineColor);
    s.overrideColors = false;
    s.overrideBorder = true;
    CHECK_EQ(ResolveSubtitleStyle(s, {}).overrideFlags, (unsigned)kSubBorder);

    s.useSystemCaptionStyle = true;
    s.overrideBorder = false;
    s.overrideFont = s.overrideColors = true;
    WindowsCaptionProperties w;
    w.fontStyle = 1;
    w.backgroundOpacity = 1;   // no font colour: the colour switch only takes the background
    SubtitleRenderStyle r = ResolveSubtitleStyle(s, w);
    CHECK_EQ(r.overrideFlags, kSubFont | kSubBackground);
    CHECK_EQ(r.fallbackFlags, kSubFont | kSubBackground);
    CHECK_EQ(r.backRgba, 0x000000FFu);
}

TEST_CASE(Subtitle_apply_to_ass_is_passed_through) {
    SubtitleStyle s = AppSource();
    CHECK(!ResolveSubtitleStyle(s, {}).applyToAss);
    s.applyToAss = true;
    CHECK(ResolveSubtitleStyle(s, {}).applyToAss);
}
