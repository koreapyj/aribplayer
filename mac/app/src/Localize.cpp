// macOS replacement of src/App/Localize.cpp (that one reads GetUserDefaultUILanguage and carries a LOGIC_TEST): the
// language comes from the preferred UI language of the process (CFLocale / NSLocale), forced with SetUiLanguage or the
// ARIBPLAYER_LANG environment variable ("ko" / "en"). The pui::UiText chooser is registered like the original.
#include "Localize.h"

#include <CoreFoundation/CoreFoundation.h>

#include <cstdlib>
#include <cstring>

#include "pui/UiText.h"

namespace app {
namespace {
UiLanguage g_lang = UiLanguage::English;
bool g_resolved = false;

bool PreferredIsKorean() {
    if (const char* env = std::getenv("ARIBPLAYER_LANG")) return std::strncmp(env, "ko", 2) == 0;
    CFArrayRef langs = CFLocaleCopyPreferredLanguages();
    bool ko = false;
    if (langs && CFArrayGetCount(langs) > 0) {
        auto first = (CFStringRef)CFArrayGetValueAtIndex(langs, 0);
        ko = CFStringHasPrefix(first, CFSTR("ko"));
    }
    if (langs) CFRelease(langs);
    return ko;
}
}

UiLanguage CurrentUiLanguage() {
    if (!g_resolved) {
        g_lang = PreferredIsKorean() ? UiLanguage::Korean : UiLanguage::English;
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
