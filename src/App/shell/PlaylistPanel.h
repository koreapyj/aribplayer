#pragma once
// Playlist panel beside the video (windowed player view; also opened as a right-side light-dismiss popup when the window is
// too narrow to dock it).
// Data: the VIEWED playlist (chip click; independent of what plays). The current playlist is read live from the
// PlaybackController (edits go through Move/Reorder/RemoveMany/MoveSelection, so the playing item keeps playing); other
// playlists are snapshots of the store, edited and saved through AppContext::SavePlaylistItems.
// Nothing in the panel holds keyboard focus except the search box (and only while the user types in it). Panel-scoped
// accelerators (PlaylistPanelLogic.h ActionForKey) apply while the panel is ACTIVE: the app marks it active when the user clicks
// inside it and inactive on a click on the video / bar / title; OnKeyDown is the single entry (the app offers keys to an active
// docked panel, the host routes them to an overlay popup panel, the search box bubbles its Ctrl keys here).
#include <functional>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include "AppContext.h"
#include "pui/Menu.h"
#include "pui/TextBox.h"
#include "pui/VirtualList.h"
#include "pui/Widget.h"
#include "shell/PlaylistPanelLogic.h"
#include "shell/ShellControls.h"

namespace app {

class PlaylistPanel : public pui::Widget {
public:
    /// overlay = the popup instance (opaque flyout surface under the panel fill).
    explicit PlaylistPanel(AppContext& ctx, bool overlay = false);
    ~PlaylistPanel() override;
    /// Full screen: the panel is flush with the screen edges, so its corners are square.
    void SetSquareCorners(bool square) { squareCorners_ = square; }

    /// Panel shown / reopened: views the current (last played) playlist with its current item selected and scrolled into view.
    void Refresh();
    /// The controller's queue / current item changed: refreshes list, selection state and summary; scrolls to the current item.
    void OnQueueChanged();
    void OnCurrentChanged();
    /// App switched the current playlist (play from a page, open files ...): the panel follows it.
    void OnCurrentPlaylistChanged();
    void OnPlaylistRenamed(const std::string& from, const std::string& to);
    void OnPlaylistDeleted(const std::string& name);
    void OnPlaylistsChanged();
    /// The app just saved `url` as played: updates the cached flag (no history read) and repaints.
    void OnItemPlayed(const std::string& url);
    /// A row of the viewed list is selected (Play with nothing loaded starts it, like a double click).
    bool CanPlaySelection() const { auto s = SelectedRows(); return !s.empty() && s.front() < (int)Items().size(); }
    void PlaySelection() { auto s = SelectedRows(); if (!s.empty()) PlayRow(s.front()); }

    const std::string& Viewed() const { return viewed_; }
    bool ViewedReadOnly() const { return readOnly_; }
    /// Data events arrived while hidden (or never filled): the owner calls Refresh() when the panel is shown.
    bool Stale() const { return stale_; }
    /// Files dropped on the panel: appended to the viewed playlist (at the drop row when feasible). False when refused.
    bool DropPaths(const std::vector<std::wstring>& paths, pui::PointF dip);
    bool AcceptsDrops() const { return !readOnly_; }
    void FocusSearch();                    // also activates the panel
    /// Active = the user last clicked inside the panel: its accelerators and list keys (Del, Enter, arrows) get keys before the player.
    bool Active() const { return active_; }
    void SetActive(bool a) { active_ = a; }

    pui::VirtualList* List() const;
    pui::TextBox* Search() const { return search_; }
    int ChipCount() const { return (int)chips_.size(); }
    const std::string& ChipName(int i) const { return chipNames_[(size_t)i]; }
    bool ChipReadOnly(int i) const { return chipReadOnly_[(size_t)i]; }
    TextButton* Chip(int i) const { return chips_[(size_t)i]; }
    GlyphButton* PlusButton() const { return plus_; }
    const std::vector<wvp::MediaItem>& Items() const;
    /// Performs a panel action as if its accelerator was pressed.
    bool Perform(plp::PanelAction a);

    void OnLayout() override;
    void Paint(pui::IRenderer& r, const pui::Theme& t) override;
    bool OnKeyDown(int vk, unsigned mods) override;
    // the panel surface swallows stray clicks; a right press must stay unhandled so the host raises OnContextMenu (rows, chips)
    bool OnPointerDown(const pui::PointerEvent& e) override { return e.button != pui::PointerButton::Right; }
    bool OnPointerWheel(const pui::PointerEvent& e) override { (void)e; return true; }  // never falls through to the volume wheel

private:
    class ItemSource;
    class ChipStrip;
    class ChipButton;
    class ChipArrow;
    class PanelList;

    bool Live() const;
    int CurrentRow() const;
    bool Editable() const { return !readOnly_; }
    void Reload();                          // rebuilds chips + list for viewed_ (keeps it)
    void SetViewed(const std::string& name);
    void RebuildChips();
    void LayoutChips();
    void ReloadSnapshot();
    void RefreshPlayed();                   // re-reads the played flags of the shown items (one history read)
    void Changed();                         // list content changed: SourceChanged + summary + button states
    void UpdateButtons();
    void UpdateSummary();
    void UpdateFilter();
    void ScrollToCurrent();
    void EnsureChipVisible(int index);
    void ScrollChipsPage(int dir);          // hover arrow click: dir -1 / +1

    std::vector<int> SelectedRows() const;
    void SelectRows(std::vector<int> rows);
    void PlayRow(int row);
    void RemoveRows(std::vector<int> rows);
    void RemoveMissing();
    void Move(wvp::MoveOp op);
    void SortBy(plp::PanelAction a);
    void ApplyOrder(const std::vector<int>& order, const std::vector<int>& keepSelected);
    void Commit(std::vector<wvp::MediaItem> items);   // snapshot playlists: save + reload
    void AddSource(plp::PanelAction a);
    void AfterAdd();
    void NewAlbum();
    void RenameAlbum(const std::string& name);
    void DeleteAlbum(const std::string& name);
    void RefreshAlbum(const std::string& name);
    void EditableCopyOfAlbum(const std::string& name);
    void SaveDefaultAsAlbum();
    void AlbumAdded(const std::string& name);   // import flows: shows the new playlist
    void ShowPlusMenu();
    void ShowChipMenu(int index);
    void ShowRowMenu(int row, pui::PointF pt);
    void ShowSortMenu();
    std::vector<pui::MenuItemDesc> BuildPlaylistMenu();
    std::vector<pui::MenuItemDesc> BuildItemMenu(int row);
    void ShowMenuFor(TextButton* anchor, std::vector<pui::MenuItemDesc> items);
    void ShowMenuForButton(GlyphButton* anchor, std::vector<pui::MenuItemDesc> items);
    pui::MenuItemDesc Item(const wchar_t* en, const wchar_t* ko, plp::PanelAction a, bool enabled = true);
    void PaintRow(pui::IRenderer& r, const pui::Theme& t, const pui::RowPaintArgs& a);
    float Px() const;                       // one device pixel in DIPs

    AppContext& ctx_;
    bool overlay_;
    bool squareCorners_ = false;
    std::string viewed_;                    // playlist name; empty = the unnamed (default) playlist
    bool readOnly_ = false, chipsDirty_ = true, active_ = false;
    std::vector<wvp::MediaItem> items_;     // snapshot while the viewed playlist is not the live current one
    std::wstring filter_, summary_;
    std::unordered_set<std::string> played_;   // URLs shown dimmed; refreshed on load / queue change / save, never per paint
    float chipScroll_ = 0, chipMax_ = 0;    // chipMax_ = overflow width (0 = chips fit)
    bool chipReveal_ = false;               // viewed chip must be scrolled into view once the strip has a size
    bool stale_ = false;                    // data events arrived while hidden: Refresh() on the next show
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);   // async add flows may finish after the panel is gone

    TextButton* tab_ = nullptr;
    ChipStrip* strip_ = nullptr;
    ChipArrow *arrowL_ = nullptr, *arrowR_ = nullptr;   // hover scroll buttons over the strip edges (painted after the strip)
    GlyphButton* plus_ = nullptr;
    pui::TextBox* search_ = nullptr;
    PanelList* list_ = nullptr;
    GlyphButton* sort_ = nullptr;           // right of the search box; opens the Sort menu
    std::vector<TextButton*> chips_;
    std::vector<std::string> chipNames_;
    std::vector<bool> chipReadOnly_;
    std::unique_ptr<ItemSource> source_;
    pui::RectF footerRect_{}, summaryRect_{};
    float dividerY_ = 0;
};

}
