#include "puimac/SymbolMap.h"

#include "pui/Glyphs.h"

namespace puimac {

namespace {
using namespace pui::glyph;
// Order = Glyphs.h order. Properties/Info, OpenUrl/Link, Equalizer/Sliders and CheckMark/Success share code points in
// Glyphs.h, so they necessarily share one symbol each (see the notes).
const SymbolEntry kTable[] = {
    {Play, "Play", "play.fill", nullptr, ""},
    {Pause, "Pause", "pause.fill", nullptr, ""},
    {Stop, "Stop", "stop.fill", nullptr, ""},
    {PreviousLegacy, "PreviousLegacy", "backward.end.fill", nullptr, ""},
    {NextLegacy, "NextLegacy", "forward.end.fill", nullptr, ""},
    {Shuffle, "Shuffle", "shuffle", nullptr, ""},
    {PlaybackRate, "PlaybackRate", "gauge.with.needle", "speedometer", ""},
    {Live, "Live", "dot.radiowaves.left.and.right", nullptr, ""},
    {Volume, "Volume", "speaker.wave.2.fill", nullptr, ""},
    {FullScreen, "FullScreen", "arrow.up.left.and.arrow.down.right", nullptr, ""},
    {BackToWindow, "BackToWindow", "arrow.down.right.and.arrow.up.left", nullptr, ""},
    {MiniPlayer, "MiniPlayer", "pip.enter", nullptr, ""},
    {ExitMiniPlayer, "ExitMiniPlayer", "pip.exit", nullptr, ""},
    {Subtitles, "Subtitles", "captions.bubble", nullptr, ""},
    {Equalizer, "Equalizer / Sliders", "slider.horizontal.3", nullptr, ""},
    {VideoSettings, "VideoSettings", "video", nullptr, ""},
    {VideoEnhancer, "VideoEnhancer", "wand.and.stars", nullptr, ""},
    {Properties, "Properties / Info", "info.circle", nullptr, "Fluent uses one code point for both; SF has no separate 'properties' sheet symbol"},
    {Hamburger, "Hamburger", "line.3.horizontal", "line.horizontal.3", ""},
    {Music, "Music", "music.note", nullptr, ""},
    {Settings, "Settings", "gearshape", nullptr, ""},
    {Search, "Search", "magnifyingglass", nullptr, ""},
    {ChevronDown, "ChevronDown", "chevron.down", nullptr, ""},
    {ChevronDownSmall, "ChevronDownSmall", "chevron.down", nullptr, "same symbol as ChevronDown; the small variant is only a size"},
    {ChevronLeftSmall, "ChevronLeftSmall", "chevron.left", nullptr, ""},
    {ChevronRightSmall, "ChevronRightSmall", "chevron.right", nullptr, ""},
    {Add, "Add", "plus", nullptr, ""},
    {Delete, "Delete", "trash", nullptr, ""},
    {Rename, "Rename", "pencil", nullptr, ""},
    {Save, "Save", "square.and.arrow.down", nullptr, "macOS has no floppy-disk symbol; 'square.and.arrow.down' is the platform's save/export metaphor"},
    {Copy, "Copy", "doc.on.doc", nullptr, ""},
    {Refresh, "Refresh", "arrow.clockwise", nullptr, ""},
    {OpenFile, "OpenFile", "doc", nullptr, ""},
    {OpenFolder, "OpenFolder", "folder", nullptr, ""},
    {OpenUrl, "OpenUrl / Link", "link", nullptr, "shared code point: both render as a chain link"},
    {Playlist, "Playlist", "music.note.list", nullptr, ""},
    // Fluent's Pin / Pinned lean 45 degrees (head top right, needle bottom left); SF's pin stands upright: turned to match.
    // Pinned (always on top) is the filled pin.
    {Pin, "Pin", "pin", nullptr, "SF's pin rotated 45 degrees clockwise to lean like Fluent's", 45},
    {Pinned, "Pinned", "pin.fill", nullptr, "SF's pin.fill rotated 45 degrees clockwise to lean like Fluent's", 45},
    {ChromeMinimize, "ChromeMinimize", "minus", nullptr, "unused on macOS (traffic lights are native)"},
    {ChromeMaximize, "ChromeMaximize", "square", nullptr, "unused on macOS"},
    {ChromeRestore, "ChromeRestore", "square.on.square", nullptr, "unused on macOS; approximate"},
    {ChromeClose, "ChromeClose", "xmark", nullptr, "unused on macOS"},
    {ClosedCaption, "ClosedCaption", "captions.bubble.fill", nullptr, "SF has no 'CC' badge; filled captions bubble distinguishes it from Subtitles"},
    {Sort, "Sort", "arrow.up.arrow.down", nullptr, ""},
    {CheckMark, "CheckMark / Success", "checkmark", nullptr, "shared code point: the InfoBar Success icon is a plain check (not checkmark.circle.fill)"},
    {Close, "Close", "xmark", nullptr, ""},
    {Warning, "Warning", "exclamationmark.triangle.fill", nullptr, ""},
    {Error, "Error", "xmark.circle.fill", nullptr, ""},
    // Not in Glyphs.h: the bar's mute / volume-level glyphs are raw code points in src/App/BarLogic.h (VolumeGlyph):
    // E74F mute, E992 (0), E993 (1-33), E994 (34-66), E995 (67-100).
    {L'\uE74F', "Mute (BarLogic)", "speaker.slash.fill", nullptr, ""},
    {L'\uE992', "Volume0 (BarLogic)", "speaker.fill", nullptr, ""},
    {L'\uE993', "Volume1 (BarLogic)", "speaker.wave.1.fill", nullptr, ""},
    {L'\uE994', "Volume2 (BarLogic)", "speaker.wave.2.fill", nullptr, ""},
    {L'\uE995', "Volume3 (BarLogic)", "speaker.wave.3.fill", nullptr, ""},
    // Not in Glyphs.h: raw code points of the Settings page rows (src/App/pages/SettingsPage.cpp).
    {L'\uE771', "Personalize (SettingsPage: App theme)", "paintbrush", nullptr, ""},
    {L'\uE790', "Color (SettingsPage: accent colour)", "paintpalette", nullptr, ""},
    {L'\uE8D2', "Caption rows (SettingsPage)", "captions.bubble", nullptr, ""},
    {L'\uE8A5', "Document (SettingsPage: licences)", "doc.text", nullptr, ""},
    {L'\uE9D9', "Diagnostic (SettingsPage: debug logging)", "ladybug", nullptr, ""},
};
}

const SymbolEntry* FindSymbol(wchar_t glyph) {
    for (const SymbolEntry& e : kTable)
        if (e.glyph == glyph) return &e;
    return nullptr;
}

const SymbolEntry* SymbolTable(size_t* count) {
    if (count) *count = sizeof(kTable) / sizeof(kTable[0]);
    return kTable;
}

}
