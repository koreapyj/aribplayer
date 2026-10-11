#pragma once
// Playlist helpers shared by the playlist panel and the playlist menus:
//  * pure logic (item count text, name validation, confirm dialog layout) - covered by LOGIC_TESTs in features/ListsTests.cpp;
//  * the in-app confirm dialog, the name prompt and the "Copy" sub-menu content.
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "AppContext.h"
#include "pui/Buttons.h"
#include "pui/Menu.h"
#include "pui/Widget.h"
#include "wvp/MediaItem.h"
#include "wvp/PlaylistStore.h"

namespace app::lists {

/// "0 items", "1 item", "N items".
std::wstring ItemCountText(int n);
/// Trims leading/trailing whitespace (what a user typed into the name prompt); UTF-8.
std::string TrimName(const std::string& name);
/// Error text for a playlist name, empty when acceptable. `renamingFrom` = the current name when renaming (the same name
/// compared case-insensitively is accepted: it is a no-op / case change).
std::wstring ValidatePlaylistName(const wvp::PlaylistStore& store, const std::string& name, const std::string& renamingFrom = {});

struct ConfirmLayout {
    pui::RectF card, title, message, confirm, cancel;
};
ConfirmLayout ComputeConfirmLayout(pui::SizeF client);

class ConfirmDialog final : public pui::Widget {
public:
    ConfirmDialog(std::wstring title, std::wstring message, std::wstring confirmText, std::function<void()> onConfirm);
    pui::Button* ConfirmButton() const { return confirm_; }
    pui::Button* CancelButton() const { return cancel_; }
    void Close();
    void OnLayout() override;
    void Paint(pui::IRenderer& r, const pui::Theme& t) override;
    bool OnPointerDown(const pui::PointerEvent&) override { return true; }
    bool OnPointerUp(const pui::PointerEvent&) override { return true; }
    bool OnKeyDown(int vk, unsigned mods) override;
private:
    std::wstring title_, message_;
    std::function<void()> onConfirm_;
    pui::Button* confirm_ = nullptr;
    pui::Button* cancel_ = nullptr;
};
/// Focus starts on Cancel.
ConfirmDialog* ShowConfirmDialog(pui::UiHost& host, const std::wstring& title, const std::wstring& message,
                                 const std::wstring& confirmText, std::function<void()> onConfirm);


/// Name prompt with validation (PromptText). nullopt = cancelled.
std::optional<std::string> PromptPlaylistName(AppContext& ctx, const std::wstring& title, const std::wstring& label,
                                              const std::string& initial, const std::wstring& okText,
                                              const std::string& renamingFrom = {});
bool SavePlaylistFile(AppContext& ctx, const std::string& name, const std::vector<wvp::MediaItem>& items);
/// Names the unnamed playlist, activates the named copy, then empties the single unnamed slot.
bool NameUnnamedPlaylist(AppContext& ctx, const std::string& name, const std::vector<wvp::MediaItem>& items, int startIndex);
bool AppendToPlaylist(AppContext& ctx, const std::string& name, const std::vector<wvp::MediaItem>& items);
/// "Copy" sub-menu content: every editable playlist except `exclude` (or a disabled "(No playlists)"), a separator and "New playlist…".
std::vector<pui::MenuItemDesc> AddToPlaylistItems(AppContext& ctx, std::function<std::vector<wvp::MediaItem>()> getItems,
                                                  const std::string& exclude = {});

}
