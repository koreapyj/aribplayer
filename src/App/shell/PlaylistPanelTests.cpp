// Pure: no window, no Direct2D.
#include "LogicTest.h"
#include "shell/PlaylistPanelLogic.h"

using namespace app;

namespace {
wvp::MediaItem Item(const wchar_t* url, const wchar_t* title = L"", std::optional<double> dur = std::nullopt) {
    return wvp::MediaItem(url, title, dur);
}
}

LOGIC_TEST(playlist_panel_summary_text) {
    SetUiLanguage(UiLanguage::English);
    LT_CHECK(plp::SummaryText(8, 0) == L"8 items");
    LT_CHECK(plp::SummaryText(1, 0) == L"1 item");
    LT_CHECK(plp::SummaryText(3, 3 * 3600 + 25 * 60 + 9.7) == L"3 items · 3:25:09");
    LT_CHECK(plp::SummaryText(2, 754) == L"2 items · 0:12:34");
    SetUiLanguage(UiLanguage::Korean);
    LT_CHECK(plp::SummaryText(8, 0) == L"8개 항목");
    LT_CHECK(plp::SummaryText(8, 3661) == L"8개 항목 · 1:01:01");
    SetUiLanguage(UiLanguage::English);
}

LOGIC_TEST(playlist_panel_row_style_current_over_played) {
    LT_CHECK(plp::RowStyleFor(true, true) == plp::RowStyle::Current);
    LT_CHECK(plp::RowStyleFor(true, false) == plp::RowStyle::Current);
    LT_CHECK(plp::RowStyleFor(false, true) == plp::RowStyle::Played);
    LT_CHECK(plp::RowStyleFor(false, false) == plp::RowStyle::Normal);
}

LOGIC_TEST(playlist_panel_marquee_offset) {
    LT_CHECK_EQ(plp::MarqueeOffset(5000, 100.f, 100.f), -1.f);     // fits: never scrolls
    LT_CHECK_EQ(plp::MarqueeOffset(100, 300.f, 100.f), -1.f);      // initial wait
    LT_CHECK_EQ(plp::MarqueeOffset(1500, 300.f, 100.f), 40.f);     // 1 s of scrolling at 40 DIP/s
    LT_CHECK_EQ(plp::MarqueeOffset(500 + 5000 + 500, 300.f, 100.f), 200.f);   // pause at the end
    LT_CHECK_EQ(plp::MarqueeOffset(500 + 5000 + 1500, 300.f, 100.f), -1.f);   // back at the start
}

LOGIC_TEST(playlist_panel_total_duration_skips_unknown) {
    std::vector<wvp::MediaItem> items{Item(L"a", L"", 60.0), Item(L"b"), Item(L"c", L"", 30.5), Item(L"d", L"", -4.0), Item(L"e", L"", 0.0)};
    LT_CHECK_EQ(plp::TotalKnownDuration(items), 90.5);
    LT_CHECK_EQ(plp::TotalKnownDuration({}), 0.0);
}

LOGIC_TEST(playlist_panel_filter_is_case_insensitive_on_title) {
    wvp::MediaItem a = Item(L"C:\\x\\Lecture 03.mp4", L"Intro To Binding");
    LT_CHECK(plp::FilterMatches(a, L""));
    LT_CHECK(plp::FilterMatches(a, L"binding"));
    LT_CHECK(plp::FilterMatches(a, L"TO BIND"));
    LT_CHECK(!plp::FilterMatches(a, L"lecture"));   // title only, not the path
    wvp::MediaItem untitled = Item(L"C:\\x\\Lecture 03.mp4");
    LT_CHECK(plp::FilterMatches(untitled, L"LECTURE"));   // falls back to the display title (file name)
}

LOGIC_TEST(playlist_panel_chip_label_of_unnamed_playlist) {
    SetUiLanguage(UiLanguage::English);
    LT_CHECK(plp::ChipLabel(L"") == L"Default playlist");
    LT_CHECK(plp::ChipLabel(L"Watch later") == L"Watch later");
    SetUiLanguage(UiLanguage::Korean);
    LT_CHECK(plp::ChipLabel(L"") == L"기본 재생목록");
    SetUiLanguage(UiLanguage::English);
}

LOGIC_TEST(playlist_panel_accelerators) {
    using A = plp::PanelAction;
    LT_CHECK(plp::ActionForKey('P', 0, true) == A::Play);
    LT_CHECK(plp::ActionForKey('P', 0, false) == A::None);   // typing in the search box
    LT_CHECK(plp::ActionForKey('O', pui::ModCtrl, false) == A::AddFolder);
    LT_CHECK(plp::ActionForKey('I', pui::ModCtrl, false) == A::AddFiles);
    LT_CHECK(plp::ActionForKey('U', pui::ModCtrl, false) == A::AddUrl);
    LT_CHECK(plp::ActionForKey('T', pui::ModCtrl, false) == A::RemoveMissing);
    LT_CHECK(plp::ActionForKey(pui::key::Delete, pui::ModCtrl, true) == A::RemoveUnselected);
    LT_CHECK(plp::ActionForKey(pui::key::Delete, pui::ModAlt, true) == A::None);   // Remove all is gone
    LT_CHECK(plp::ActionForKey(pui::key::Home, pui::ModCtrl, true) == A::MoveTop);
    LT_CHECK(plp::ActionForKey(pui::key::End, pui::ModCtrl, true) == A::MoveBottom);
    LT_CHECK(plp::ActionForKey('1', pui::ModCtrl, true) == A::Sort);
    LT_CHECK(plp::ActionForKey('2', pui::ModCtrl, true) == A::Reverse);
    LT_CHECK(plp::ActionForKey('9', pui::ModCtrl, true) == A::Shuffle);
    LT_CHECK(plp::ActionForKey('0', pui::ModCtrl, true) == A::SortFolder);
    LT_CHECK(plp::ActionForKey('O', pui::ModCtrl | pui::ModShift, true) == A::None);
    LT_CHECK(plp::ActionForKey(pui::key::F6, 0, true) == A::None);
    // every bound action with a hint reports the same text it is bound to
    LT_CHECK(plp::AccelText(A::AddFiles) == L"Ctrl+I" && plp::AccelText(A::RemoveAbove).empty() && plp::AccelText(A::Play).empty());
}

LOGIC_TEST(playlist_panel_rows_above) {
    LT_CHECK(plp::RowsAbove({}).empty());
    LT_CHECK(plp::RowsAbove({0, 2}).empty());
    LT_CHECK((plp::RowsAbove({3, 5}) == std::vector<int>{0, 1, 2}));
}

LOGIC_TEST(playlist_panel_move_permutation) {
    std::vector<int> sel;
    // Up: a block at the top stays, the rest moves one step
    auto o = plp::MovePermutation(5, {0, 1, 3}, wvp::MoveOp::Up, &sel);
    LT_CHECK((o == std::vector<int>{0, 1, 3, 2, 4}));
    LT_CHECK((sel == std::vector<int>{0, 1, 2}));
    o = plp::MovePermutation(5, {1, 4}, wvp::MoveOp::Down, &sel);
    LT_CHECK((o == std::vector<int>{0, 2, 1, 3, 4}));
    LT_CHECK((sel == std::vector<int>{2, 4}));
    o = plp::MovePermutation(5, {1, 3}, wvp::MoveOp::Top, &sel);
    LT_CHECK((o == std::vector<int>{1, 3, 0, 2, 4}));
    LT_CHECK((sel == std::vector<int>{0, 1}));
    o = plp::MovePermutation(5, {1, 3}, wvp::MoveOp::Bottom, &sel);
    LT_CHECK((o == std::vector<int>{0, 2, 4, 1, 3}));
    LT_CHECK((sel == std::vector<int>{3, 4}));
    // remap a selection through a permutation, and the complement
    LT_CHECK((plp::RemapSelection({2, 0, 1}, {0, 2}) == std::vector<int>{0, 1}));
    LT_CHECK((plp::Complement(5, {1, 3}) == std::vector<int>{0, 2, 4}));
}
