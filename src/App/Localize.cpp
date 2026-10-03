#include "Localize.h"

#include <windows.h>

#include "LogicTest.h"
#include "pui/UiText.h"

namespace app {
namespace {
UiLanguage g_lang = UiLanguage::English;
bool g_resolved = false;
}

UiLanguage CurrentUiLanguage() {
    if (!g_resolved) {
        g_lang = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_KOREAN ? UiLanguage::Korean : UiLanguage::English;
        g_resolved = true;
    }
    return g_lang;
}

void SetUiLanguage(UiLanguage lang) {
    g_lang = lang;
    g_resolved = true;
}

namespace {
[[maybe_unused]] const bool g_uiTextRegistered = (pui::SetUiTextChooser([](const wchar_t* en, const wchar_t* ko) { return Tr(en, ko); }), true);
}

}

namespace app {

LOGIC_TEST(localize_tr_follows_forced_language) {
    const UiLanguage before = CurrentUiLanguage();
    SetUiLanguage(UiLanguage::Korean);
    LT_CHECK(std::wstring(Tr(L"Close", L"닫기")) == L"닫기");
    LT_CHECK(TrW(L"a", L"가") == L"가");
    LT_CHECK(std::wstring(pui::UiText(L"Close", L"닫기")) == L"닫기");
    SetUiLanguage(UiLanguage::English);
    LT_CHECK(std::wstring(Tr(L"Close", L"닫기")) == L"Close");
    LT_CHECK(TrW(L"a", L"가") == L"a");
    SetUiLanguage(before);
}

}
