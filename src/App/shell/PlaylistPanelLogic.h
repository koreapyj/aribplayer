#pragma once
// Pure logic of the playlist panel (covered by --logictest, see PlaylistPanelTests.cpp): footer summary text, the search
// filter, the unnamed playlist's chip label, the panel-scoped accelerator table and the move-selection permutation used for
// playlists that are not driving the controller. No widgets, no Win32.
#include <algorithm>
#include <cmath>
#include <cwctype>
#include <string>
#include <vector>

#include "Localize.h"
#include "pui/Geometry.h"
#include "wvp/MediaItem.h"
#include "wvp/PlaybackController.h"

namespace app::plp {

/// The unnamed (persistent) playlist has no file name: its chip reads "Default playlist".
inline std::wstring DefaultPlaylistLabel() { return Tr(L"Default playlist", L"기본 재생목록"); }
inline std::wstring ChipLabel(const std::wstring& playlistName) { return playlistName.empty() ? DefaultPlaylistLabel() : playlistName; }

/// Sum of the known (finite, positive) durations in seconds; unknown ones are excluded.
inline double TotalKnownDuration(const std::vector<wvp::MediaItem>& items) {
    double sum = 0;
    for (const auto& it : items)
        if (it.duration && std::isfinite(*it.duration) && *it.duration > 0) sum += *it.duration;
    return sum;
}

/// "H:MM:SS" (hours not zero padded; no day rollover).
inline std::wstring FormatTotal(double seconds) {
    if (!(seconds >= 0) || !std::isfinite(seconds)) seconds = 0;
    long long total = (long long)std::floor(seconds);
    wchar_t buf[48];
    swprintf(buf, 48, L"%lld:%02lld:%02lld", total / 3600, (total / 60) % 60, total % 60);
    return buf;
}

/// "N items · H:MM:SS" / "N개 항목 · H:MM:SS"; the time part is left out when no duration is known.
inline std::wstring SummaryText(int count, double totalSeconds) {
    std::wstring s = CurrentUiLanguage() == UiLanguage::Korean
                         ? std::to_wstring(count) + L"개 항목"
                         : std::to_wstring(count) + (count == 1 ? L" item" : L" items");
    if (totalSeconds > 0) s += L" · " + FormatTotal(totalSeconds);
    return s;
}

/// Case-insensitive "contains" on the item's display title; an empty query keeps everything.
inline bool FilterMatches(const wvp::MediaItem& item, const std::wstring& query) {
    if (query.empty()) return true;
    auto lower = [](std::wstring s) { for (auto& c : s) c = (wchar_t)std::towlower(c); return s; };
    return lower(item.DisplayTitle()).find(lower(query)) != std::wstring::npos;
}

/// Row text style: the current item keeps its accent look, a played one is dimmed, the rest is normal.
enum class RowStyle { Normal, Played, Current };
inline RowStyle RowStyleFor(bool current, bool played) { return current ? RowStyle::Current : played ? RowStyle::Played : RowStyle::Normal; }

/// Hover marquee of an overlong row title. `hoverMs`: time since the hover began. Returns the scroll offset in DIPs
/// (0..textW-rectW), or -1 when the ellipsis look applies (not overflowing, initial 500 ms wait, or the 1 s rest at the start).
/// Cycle: scroll left at 40 DIP/s, 1 s pause at the end, jump back, 1 s pause (ellipsis), repeat.
inline float MarqueeOffset(unsigned long long hoverMs, float textW, float rectW) {
    constexpr float kSpeed = 40.f;
    constexpr unsigned long long kDelayMs = 500, kPauseMs = 1000;
    const float over = textW - rectW;
    if (!(over > 0.5f) || hoverMs < kDelayMs) return -1.f;
    const unsigned long long scrollMs = (unsigned long long)std::ceil(over / kSpeed * 1000.f);
    const unsigned long long t = (hoverMs - kDelayMs) % (scrollMs + 2 * kPauseMs);
    if (t < scrollMs) return std::min(over, kSpeed * (float)t / 1000.f);
    if (t < scrollMs + kPauseMs) return over;
    return -1.f;
}

enum class PanelAction {
    None, Play,
    SelectAll, InvertSelection,
    RemoveSelected, RemoveUnselected, RemoveAbove, RemoveMissing,
    MoveTop, MoveUp, MoveDown, MoveBottom,
    AddFiles, AddFolder, AddUrl,
    NewAlbum, RenameAlbum, DeleteAlbum, FocusSearch,
    Sort, Reverse, SortTitle, SortFileName, SortExtension, SortSize, SortDuration, SortDate, Shuffle, SortFolder
};

/// The panel's accelerator table. Only consulted for keys the focused widget did not
/// handle and only while focus is inside the panel. `inList`: focus is in the item list (plain keys apply only there, so
/// typing in the search box never plays a row). Ctrl+Del reaches here because the list ignores that modifier.
inline PanelAction ActionForKey(int vk, unsigned mods, bool inList) {
    const bool ctrl = (mods & pui::ModCtrl) != 0, alt = (mods & pui::ModAlt) != 0, shift = (mods & pui::ModShift) != 0;
    if (shift) return PanelAction::None;
    if (!ctrl && !alt) return inList && vk == 'P' ? PanelAction::Play : PanelAction::None;
    if (alt && !ctrl) return PanelAction::None;
    switch (vk) {   // Ctrl only
    case pui::key::Home: return PanelAction::MoveTop;
    case pui::key::Up: return PanelAction::MoveUp;
    case pui::key::Down: return PanelAction::MoveDown;
    case pui::key::End: return PanelAction::MoveBottom;
    case pui::key::Delete: return PanelAction::RemoveUnselected;
    case 'A': return PanelAction::SelectAll;
    case 'J': return PanelAction::InvertSelection;
    case 'T': return PanelAction::RemoveMissing;
    case 'I': return PanelAction::AddFiles;
    case 'O': return PanelAction::AddFolder;
    case 'U': return PanelAction::AddUrl;
    case 'N': return PanelAction::NewAlbum;
    case 'R': return PanelAction::RenameAlbum;
    case 'D': return PanelAction::DeleteAlbum;
    case 'S': return PanelAction::FocusSearch;
    case '1': return PanelAction::Sort;
    case '2': return PanelAction::Reverse;
    case '3': return PanelAction::SortTitle;
    case '4': return PanelAction::SortFileName;
    case '5': return PanelAction::SortExtension;
    case '6': return PanelAction::SortSize;
    case '7': return PanelAction::SortDuration;
    case '8': return PanelAction::SortDate;
    case '9': return PanelAction::Shuffle;
    case '0': return PanelAction::SortFolder;
    default: return PanelAction::None;
    }
}

/// Menu hint text of an action ("Ctrl+I"); empty when it has none.
inline std::wstring AccelText(PanelAction a) {
    switch (a) {
    case PanelAction::AddFiles: return L"Ctrl+I";
    case PanelAction::AddFolder: return L"Ctrl+O";
    case PanelAction::AddUrl: return L"Ctrl+U";
    case PanelAction::RemoveSelected: return L"Del";
    case PanelAction::RemoveUnselected: return L"Ctrl+Del";
    case PanelAction::RemoveMissing: return L"Ctrl+T";
    case PanelAction::NewAlbum: return L"Ctrl+N";
    case PanelAction::RenameAlbum: return L"Ctrl+R";
    case PanelAction::DeleteAlbum: return L"Ctrl+D";
    case PanelAction::MoveTop: return L"Ctrl+Home";
    case PanelAction::MoveUp: return L"Ctrl+Up";
    case PanelAction::MoveDown: return L"Ctrl+Down";
    case PanelAction::MoveBottom: return L"Ctrl+End";
    case PanelAction::Sort: return L"Ctrl+1";
    case PanelAction::Reverse: return L"Ctrl+2";
    case PanelAction::SortTitle: return L"Ctrl+3";
    case PanelAction::SortFileName: return L"Ctrl+4";
    case PanelAction::SortExtension: return L"Ctrl+5";
    case PanelAction::SortSize: return L"Ctrl+6";
    case PanelAction::SortDuration: return L"Ctrl+7";
    case PanelAction::SortDate: return L"Ctrl+8";
    case PanelAction::Shuffle: return L"Ctrl+9";
    case PanelAction::SortFolder: return L"Ctrl+0";
    case PanelAction::SelectAll: return L"Ctrl+A";
    case PanelAction::InvertSelection: return L"Ctrl+J";
    case PanelAction::FocusSearch: return L"Ctrl+S";
    default: return {};
    }
}

/// New order (result[newIndex] = oldIndex) after moving the selected rows per `op`, with the PlaybackController::MoveSelection
/// rules (Up/Down keep a block that already touches the edge in place). `selected` = old indices (any order, invalid ignored).
/// `newSelection` receives the selected rows' new indices, ascending.
inline std::vector<int> MovePermutation(int count, const std::vector<int>& selected, wvp::MoveOp op, std::vector<int>* newSelection = nullptr) {
    std::vector<char> sel((size_t)std::max(0, count), 0);
    for (int i : selected) if (i >= 0 && i < count) sel[(size_t)i] = 1;
    std::vector<int> order((size_t)std::max(0, count));
    for (int i = 0; i < count; i++) order[(size_t)i] = i;
    std::vector<char> flag = sel;   // follows `order`
    switch (op) {
    case wvp::MoveOp::Top:
    case wvp::MoveOp::Bottom: {
        std::vector<int> picked, rest;
        for (int i = 0; i < count; i++) (sel[(size_t)i] ? picked : rest).push_back(i);
        order.clear();
        if (op == wvp::MoveOp::Top) { order = picked; order.insert(order.end(), rest.begin(), rest.end()); }
        else { order = rest; order.insert(order.end(), picked.begin(), picked.end()); }
        break;
    }
    case wvp::MoveOp::Up:
        for (int i = 1; i < count; i++)
            if (flag[(size_t)i] && !flag[(size_t)i - 1]) { std::swap(order[(size_t)i], order[(size_t)i - 1]); std::swap(flag[(size_t)i], flag[(size_t)i - 1]); }
        break;
    case wvp::MoveOp::Down:
        for (int i = count - 2; i >= 0; i--)
            if (flag[(size_t)i] && !flag[(size_t)i + 1]) { std::swap(order[(size_t)i], order[(size_t)i + 1]); std::swap(flag[(size_t)i], flag[(size_t)i + 1]); }
        break;
    }
    if (newSelection) {
        newSelection->clear();
        for (int i = 0; i < count; i++) if (sel[(size_t)order[(size_t)i]]) newSelection->push_back(i);
    }
    return order;
}

/// Where the old selection ends up after a permutation (result[new] = old): old indices -> new indices, ascending.
inline std::vector<int> RemapSelection(const std::vector<int>& order, const std::vector<int>& selected) {
    std::vector<int> inv(order.size(), -1), out;
    for (size_t n = 0; n < order.size(); n++) if (order[n] >= 0 && (size_t)order[n] < inv.size()) inv[(size_t)order[n]] = (int)n;
    for (int s : selected) if (s >= 0 && (size_t)s < inv.size() && inv[(size_t)s] >= 0) out.push_back(inv[(size_t)s]);
    std::sort(out.begin(), out.end());
    return out;
}

/// Rows 0..count-1 that are not in `selected`.
/// Rows above the topmost selected one (selected rows and everything below stay); empty without a selection.
inline std::vector<int> RowsAbove(const std::vector<int>& selected) {
    std::vector<int> out;
    if (selected.empty()) return out;
    const int top = *std::min_element(selected.begin(), selected.end());
    for (int i = 0; i < top; i++) out.push_back(i);
    return out;
}

inline std::vector<int> Complement(int count, const std::vector<int>& selected) {
    std::vector<char> sel((size_t)std::max(0, count), 0);
    for (int i : selected) if (i >= 0 && i < count) sel[(size_t)i] = 1;
    std::vector<int> out;
    for (int i = 0; i < count; i++) if (!sel[(size_t)i]) out.push_back(i);
    return out;
}

}
