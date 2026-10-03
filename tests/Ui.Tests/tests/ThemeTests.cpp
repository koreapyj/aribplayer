#include "pui/Buttons.h"
#include "wvptest.h"

using namespace pui;

TEST_CASE(Theme_DarkTokensMatchSpec) {
    Theme t = Theme::Make(false);
    CHECK(!t.light);
    CHECK_EQ(t.accent.v, 0xFFFF8232u);                       // Zest fallback
    CHECK_EQ(t.accentHover.v, 0xE6FF8232u);                  // accent at 0.9
    CHECK_EQ(t.accentPressed.v, 0xCCFF8232u);                // accent at 0.8
    CHECK_EQ(t.barFill.v, 0xB31C1C1Cu);                      // ControlOnImageFillColorDefault
    CHECK_EQ(t.barStroke.v, 0x19000000u);                    // CardStrokeColorDefault
    CHECK_EQ(t.textPrimary.v, 0xFFFFFFFFu);
    CHECK_EQ(t.textSecondary.v, 0xC5FFFFFFu);
    CHECK_EQ(t.textDisabled.v, 0x5DFFFFFFu);
    CHECK_EQ(t.subtleSecondary.v, 0x0FFFFFFFu);
    CHECK_EQ(t.subtleTertiary.v, 0x0AFFFFFFu);
    CHECK_EQ(t.subtleDisabled.v, 0x00FFFFFFu);
    CHECK_EQ(t.ctlDefault.v, 0x0FFFFFFFu);
    CHECK_EQ(t.ctlSecondary.v, 0x15FFFFFFu);
    CHECK_EQ(t.ctlTertiary.v, 0x08FFFFFFu);
    CHECK_EQ(t.ctlDisabled.v, 0x0BFFFFFFu);
    CHECK_EQ(t.ringDisabled.v, 0x14FFFFFFu);
    CHECK_EQ(t.ctlStrongFill.v, 0x8BFFFFFFu);                // slider track
    CHECK_EQ(t.ctlStrongFillDisabled.v, 0x28FFFFFFu);
    CHECK_EQ(t.ctlSolidFill.v, 0xFF454545u);                 // slider thumb outer
    CHECK_EQ(t.ctlStrokeSecondary.v, 0x18FFFFFFu);           // thumb border
    CHECK_EQ(t.flyoutStroke.v, 0x33000000u);
    CHECK_EQ(t.flyoutSurface.v, 0xFF2C2C2Cu);
}

TEST_CASE(Theme_LightTokensMatchSpec) {
    Theme t = Theme::Make(true);
    CHECK(t.light);
    CHECK_EQ(t.accent.v, 0xFFD03F0Au);
    CHECK_EQ(t.accentHover.v, 0xE6D03F0Au);
    CHECK_EQ(t.accentPressed.v, 0xCCD03F0Au);
    CHECK_EQ(t.textOnAccent.v, 0xFFFFFFFFu);
    CHECK_EQ(t.barFill.v, 0xC9FFFFFFu);
    CHECK_EQ(t.barStroke.v, 0x0F000000u);
    CHECK_EQ(t.textPrimary.v, 0xE4000000u);
    CHECK_EQ(t.textSecondary.v, 0x9E000000u);
    CHECK_EQ(t.textDisabled.v, 0x5C000000u);
    CHECK_EQ(t.subtleSecondary.v, 0x09000000u);
    CHECK_EQ(t.subtleTertiary.v, 0x06000000u);
    CHECK_EQ(t.subtleDisabled.v, 0x00000000u);
    CHECK_EQ(t.ctlDefault.v, 0xB3FFFFFFu);
    CHECK_EQ(t.ctlSecondary.v, 0x80F9F9F9u);
    CHECK_EQ(t.ctlTertiary.v, 0x4DF9F9F9u);
    CHECK_EQ(t.ctlDisabled.v, 0x4DF9F9F9u);
    CHECK_EQ(t.ringDisabled.v, 0x0F000000u);
    CHECK_EQ(t.ctlStrongFill.v, 0x72000000u);
    CHECK_EQ(t.ctlStrongFillDisabled.v, 0x51000000u);
    CHECK_EQ(t.ctlSolidFill.v, 0xFFFFFFFFu);
    CHECK_EQ(t.ctlStrokeSecondary.v, 0x29000000u);
    CHECK_EQ(t.flyoutStroke.v, 0x0F000000u);
    CHECK_EQ(t.flyoutSurface.v, 0xFFF9F9F9u);
}

TEST_CASE(Theme_SystemAccentIsUsedForAccentTokens) {
    Theme d = Theme::Make(false, Color(0xFF3A96DD));
    CHECK_EQ(d.accent.v, 0xFF3A96DDu);
    CHECK_EQ(d.accentHover.v, 0xE63A96DDu);
    CHECK_EQ(d.accentPressed.v, 0xCC3A96DDu);
    Theme l = Theme::Make(true, Color(0xFF005A9E));
    CHECK_EQ(l.accent.v, 0xFF005A9Eu);
    CHECK_EQ(l.textOnAccent.v, 0xFFFFFFFFu);
}

TEST_CASE(Theme_MetricsAndStatusTokens) {
    Theme t = Theme::Make(false);
    CHECK_EQ(t.controlRadius, 4.f);
    CHECK_EQ(t.overlayRadius, 8.f);
    CHECK_EQ(t.fontBody, 14.f);
    CHECK_EQ(t.fontCaption, 12.f);
    Theme l = Theme::Make(true);
    for (const Theme* th : {&t, &l}) {
        CHECK(th->critical != th->criticalBg);
        CHECK(th->caution != th->cautionBg);
        CHECK(th->success != th->successBg);
    }
    CHECK(t.critical != l.critical);
}

TEST_CASE(Theme_LiveSwitchRepaintsControlsWithNewTokens) {
    UiHost h;
    h.SetSize({400, 200});
    auto* root = h.SetRoot(std::make_unique<Widget>(Role::Pane));
    auto* b = root->Add<Button>(L"Theme");
    b->SetBounds({0, 0, 100, 32});
    Theme dark = Theme::Make(false), light = Theme::Make(true);
    h.SetTheme(dark);
    h.ClearDirty();
    RecordingRenderer rr;
    h.Paint(rr);
    CHECK(rr.calls[0].color == dark.ctlDefault);
    CHECK(rr.calls.back().color == dark.textPrimary);
    h.SetTheme(light);
    CHECK(h.IsDirty());
    rr.Clear();
    h.Paint(rr);
    CHECK(rr.calls[0].color == light.ctlDefault);
    CHECK(rr.calls.back().color == light.textPrimary);
    CHECK(rr.calls[0].color != dark.ctlDefault);
    h.SetTheme(Theme::Make(false, Color(0xFF3A96DD)));
    auto* ab = root->Add<Button>(L"Accent", ButtonStyle::Accent);
    ab->SetBounds({0, 40, 100, 72});
    rr.Clear();
    h.Paint(rr);
    bool accentFill = false;
    for (auto& c : rr.calls) accentFill = accentFill || c.color.v == 0xFF3A96DDu;
    CHECK(accentFill);
}

TEST_CASE(Geometry_SnapDipLandsOnDevicePixels) {
    for (float sc : {1.f, 1.25f, 1.5f, 2.f}) {
        for (float d : {0.f, 3.f, 87.f, 100.4f, 241.7f}) {
            const float px = SnapDip(d, sc) * sc;
            CHECK(std::fabs(px - std::round(px)) < 1e-3f);
            CHECK(std::fabs(SnapDip(d, sc) - d) <= 0.5f / sc + 1e-4f);
        }
    }
}
