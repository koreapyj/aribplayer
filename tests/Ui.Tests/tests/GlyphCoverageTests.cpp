// Every private-use code point the app draws with FontKind::Icon must exist in "Segoe Fluent Icons".
// Sources checked: every L'\uXXXX' constant in pui/Glyphs.h and every PUA character (raw UTF-8 or \uXXXX escape) found in
// the sources of PlayerApp (src/App) and PlayerUi (src/Ui). Skipped (with a message) when the font is not installed.
#include <dwrite.h>
#include <wrl/client.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>

#include "pui/Glyphs.h"
#include "wvptest.h"

#pragma comment(lib, "dwrite.lib")

namespace {
namespace fs = std::filesystem;

std::string ReadAll(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool IsHex(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

/// Adds PUA code points found in text: \uXXXX escapes and UTF-8 sequences U+E000..U+F8FF (EE xx xx / EF xx xx).
void Collect(const std::string& s, std::set<unsigned>& out) {
    for (size_t i = 0; i < s.size(); i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\\' && i + 5 < s.size() && s[i + 1] == 'u' && IsHex(s[i + 2]) && IsHex(s[i + 3]) && IsHex(s[i + 4]) && IsHex(s[i + 5])) {
            unsigned v = (unsigned)std::stoul(s.substr(i + 2, 4), nullptr, 16);
            if (v >= 0xE000 && v <= 0xF8FF) out.insert(v);
        } else if ((c == 0xEE || c == 0xEF) && i + 2 < s.size() && ((unsigned char)s[i + 1] & 0xC0) == 0x80 &&
                   ((unsigned char)s[i + 2] & 0xC0) == 0x80) {
            unsigned v = ((c & 0x0Fu) << 12) | (((unsigned char)s[i + 1] & 0x3Fu) << 6) | ((unsigned char)s[i + 2] & 0x3Fu);
            if (v >= 0xE000 && v <= 0xF8FF) out.insert(v);
        }
    }
}

// __FILE__ is <repo>\tests\Ui.Tests\tests\GlyphCoverageTests.cpp.
fs::path RepoRoot() { return fs::path(__FILE__).parent_path().parent_path().parent_path().parent_path(); }
}

TEST_CASE(Glyph_AllCodePointsExistInSegoeFluentIcons) {
    using Microsoft::WRL::ComPtr;
    ComPtr<IDWriteFactory> dw;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(dw.GetAddressOf())))) {
        std::printf("  SKIP: DirectWrite unavailable\n");
        return;
    }
    ComPtr<IDWriteFontCollection> coll;
    dw->GetSystemFontCollection(&coll, FALSE);
    UINT32 idx = 0;
    BOOL exists = FALSE;
    if (!coll || FAILED(coll->FindFamilyName(L"Segoe Fluent Icons", &idx, &exists)) || !exists) {
        std::printf("  SKIP: font \"Segoe Fluent Icons\" is not installed on this machine\n");
        return;
    }
    ComPtr<IDWriteFontFamily> fam;
    REQUIRE(SUCCEEDED(coll->GetFontFamily(idx, &fam)));
    ComPtr<IDWriteFont> font;
    REQUIRE(SUCCEEDED(fam->GetFirstMatchingFont(DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STRETCH_NORMAL, DWRITE_FONT_STYLE_NORMAL, &font)));

    std::set<unsigned> cps;
    fs::path src = RepoRoot() / "src";
    Collect(ReadAll(src / "Ui" / "include" / "pui" / "Glyphs.h"), cps);
    size_t fromHeader = cps.size();
    CHECK(fromHeader > 30);
    for (const char* dir : {"App", "Ui/src", "Ui/include"}) {
        CHECK(fs::is_directory(src / dir));   // a wrong path would otherwise scan nothing and pass
        std::error_code ec;
        for (fs::recursive_directory_iterator it(src / dir, ec), end; !ec && it != end; it.increment(ec)) {
            auto ext = it->path().extension();
            if (!it->is_regular_file() || (ext != ".cpp" && ext != ".h")) continue;
            if (it->path().filename() == "GlyphCoverageTests.cpp") continue;
            Collect(ReadAll(it->path()), cps);
        }
    }
    std::string missing;
    for (unsigned cp : cps) {
        BOOL has = FALSE;
        font->HasCharacter(cp, &has);
        if (!has) {
            char buf[16];
            std::snprintf(buf, sizeof buf, " U+%04X", cp);
            missing += buf;
        }
    }
    if (!missing.empty()) std::printf("  glyphs missing from Segoe Fluent Icons:%s\n", missing.c_str());
    CHECK(missing.empty());
}

TEST_CASE(Glyph_ChromeAndPinCodePoints) {
    CHECK_EQ((unsigned)pui::glyph::Pin, 0xE718u);
    CHECK_EQ((unsigned)pui::glyph::Pinned, 0xE840u);
    CHECK_EQ((unsigned)pui::glyph::ChromeMinimize, 0xE921u);
    CHECK_EQ((unsigned)pui::glyph::ChromeMaximize, 0xE922u);
    CHECK_EQ((unsigned)pui::glyph::ChromeRestore, 0xE923u);
    CHECK_EQ((unsigned)pui::glyph::ChromeClose, 0xE8BBu);
    CHECK_EQ((unsigned)pui::glyph::ClosedCaption, 0xE7F0u);
    CHECK_EQ((unsigned)pui::glyph::Sort, 0xE8CBu);
}
