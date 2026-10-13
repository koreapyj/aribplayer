#pragma once
// Glyphs.h names "Segoe Fluent Icons" code points, a font that does not exist on macOS. This table maps every code point
// used by the app to an SF Symbol name (plus an older-name fallback where the preferred symbol is recent).
#include <cstddef>

namespace puimac {

struct SymbolEntry {
    wchar_t glyph;          // Glyphs.h code point
    const char* puiName;    // pui::glyph::<name> (first name when two share a code point)
    const char* symbol;     // SF Symbol name
    const char* fallback;   // older SF Symbol name tried when `symbol` is unavailable (nullptr = none)
    const char* note;       // "" = faithful match; otherwise what differs
    float rotation = 0;     // degrees clockwise, drawn about the symbol's centre (Fluent's Pin is tilted, SF's pin upright)
};

const SymbolEntry* FindSymbol(wchar_t glyph);
const SymbolEntry* SymbolTable(size_t* count);

}
