#pragma once
// ARIB caption font on macOS: the counterpart of the Windows app's ApplyCaptionOpenOptions font part
// (features/SettingsService.cpp: arib_font = "file:<exe dir>\fonts\WLCMARU2004ARIBU.TTF").
//
// The darwin libffplay renders captions with libaribcaption's CoreText font provider (ARIBCC_USE_CORETEXT; the "file:"
// form of the font option is a Windows-only path of libavcodec/libaribcaption.c that selects the DirectWrite
// provider). The CoreText provider (libaribcaption src/renderer/font_provider_coretext.cpp, GetFontFace) looks every
// name of the comma-separated "font" option up as a FAMILY name: a CTFontDescriptor with kCTFontFamilyNameAttribute =
// the name and kCTFontWeightTrait = 0 -> CTFontCreateWithFontDescriptor, then checks the glyph of the character.
// libavcodec passes the list with force_default = true, which replaces libaribcaption's own Japanese defaults
// (Hiragino Maru Gothic ProN, Hiragino Sans, sans-serif). So the Mac host registers the TTF for the process
// (CTFontManagerRegisterFontsForURL, kCTFontManagerScopeProcess: visible to CoreText lookups in this process, the
// engine's threads included) and passes its family name followed by those defaults - the same shape as Windows, where
// libaribcaption.c appends "Windows TV MaruGothic, MS Gothic, sans-serif" after the file font.
#include <string>

namespace mainview {

struct AribFont {
    std::string file;        // the TTF registered
    std::string family;      // its CoreText family name ("WadaLabChuMaruGo2004ARIB" for the bundled font)
    std::string postscript;  // its PostScript name
    std::string option;      // the arib_font open option value: "<family>,Hiragino Maru Gothic ProN,Hiragino Sans,sans-serif"
    std::string resolved;    // the file the provider's lookup of `family` resolves to (must equal `file`)
    std::string error;       // non-empty when registration or the lookup failed; option then holds the defaults only
    bool Ok() const { return error.empty(); }
};

/// The bundled font: <app>/Contents/Resources/fonts/WLCMARU2004ARIBU.TTF (copied there by the build), else the
/// repository's assets/fonts/WLCMARU2004ARIBU.TTF next to the build tree. "" when neither exists.
std::string BundledAribFontPath();

/// Registers `ttfPath` for the process (idempotent per path) and builds the arib_font option. Main thread, at startup.
AribFont RegisterAribFont(const std::string& ttfPath);

/// The bundled caption font, registered once for the process.
const AribFont& BundledCaptionFont();

/// The exact lookup of libaribcaption's CoreText provider for one family name: the font file it resolves to ("" when
/// CoreText returns nothing). Used to verify that the registered family is what the engine will render with.
std::string ResolveLikeLibaribcaption(const std::string& family);

}
