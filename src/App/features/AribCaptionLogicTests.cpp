#include <filesystem>
#include <windows.h>

#include "LogicTest.h"
#include "features/SettingsService.h"

namespace app {

LOGIC_TEST(arib_bundled_font_path_is_next_to_executable_and_deployed) {
    wchar_t exe[MAX_PATH * 2] = {};
    DWORD n = GetModuleFileNameW(nullptr, exe, (DWORD)std::size(exe));
    LT_CHECK(n > 0 && n < std::size(exe));
    if (!n || n >= std::size(exe)) return;

    const std::filesystem::path expected = std::filesystem::path(exe).parent_path() /
        L"fonts" / L"WLCMARU2004ARIBU.TTF";
    const std::wstring actual = BundledAribFontPath();
    LT_CHECK_EQ(actual, expected.wstring());
    LT_CHECK(GetFileAttributesW(actual.c_str()) != INVALID_FILE_ATTRIBUTES);
}

}
