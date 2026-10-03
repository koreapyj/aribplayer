#include "features/ListsCommon.h"
#include "features/DialogGuard.h"
#include "Localize.h"

#include <algorithm>

#include "features/OpenFlows.h"
#include "features/TextInputDialog.h"
#include "pui/Glyphs.h"

namespace app::lists {

std::wstring ItemCountText(int n) {
    if (n < 0) n = 0;
    return TrW(n == 1 ? L"1 item" : std::to_wstring(n) + L" items", std::to_wstring(n) + L"개 항목");
}

std::wstring TrimName(const std::wstring& name) {
    size_t b = 0, e = name.size();
    while (b < e && iswspace(name[b])) b++;
    while (e > b && iswspace(name[e - 1])) e--;
    return name.substr(b, e - b);
}

std::wstring ValidatePlaylistName(const wvp::PlaylistStore& store, const std::wstring& rawName, const std::wstring& renamingFrom) {
    std::wstring name = TrimName(rawName);
    if (name.empty()) return Tr(L"Enter a name for the playlist.", L"재생목록 이름을 입력하세요.");
    if (!wvp::PlaylistStore::IsValidName(name))
        return Tr(L"A playlist name can't contain < > : \" / \\ | ? * and can have at most 100 characters.", L"재생목록 이름에는 < > : \" / \\ | ? * 문자를 사용할 수 없으며 최대 100자까지 입력할 수 있습니다.");
    if (!renamingFrom.empty() && _wcsicmp(name.c_str(), renamingFrom.c_str()) == 0) return {};
    if (store.Exists(name)) return Tr(L"A playlist with this name already exists.", L"같은 이름의 재생목록이 이미 있습니다.");
    return {};
}

ConfirmLayout ComputeConfirmLayout(pui::SizeF client) {
    ConfirmLayout L;
    float w = std::clamp(client.w - 32.f, 200.f, 420.f), h = 172.f;
    float x = std::max(0.f, (client.w - w) * 0.5f), y = std::max(0.f, (client.h - h) * 0.5f);
    L.card = {x, y, x + w, y + h};
    L.title = {x + 24, y + 20, x + w - 24, y + 48};
    L.message = {x + 24, y + 54, x + w - 24, y + h - 68};
    float bw = (w - 48 - 8) * 0.5f, by = y + h - 56;
    L.confirm = {x + 24, by, x + 24 + bw, by + 32};
    L.cancel = {x + 24 + bw + 8, by, x + w - 24, by + 32};
    return L;
}


ConfirmDialog::ConfirmDialog(std::wstring title, std::wstring message, std::wstring confirmText, std::function<void()> onConfirm)
    : pui::Widget(pui::Role::Group), title_(std::move(title)), message_(std::move(message)), onConfirm_(std::move(onConfirm)) {
    SetName(title_);
    SetHelpText(message_);
    SetAutomationId(L"ConfirmDialog");
    confirm_ = Add<pui::Button>(confirmText, pui::ButtonStyle::Accent);
    confirm_->SetName(confirmText);
    confirm_->onClick = [this] {
        auto cb = onConfirm_;
        Close();
        if (cb) cb();
    };
    cancel_ = Add<pui::Button>(Tr(L"Cancel", L"취소"));
    cancel_->SetName(Tr(L"Cancel", L"취소"));
    cancel_->onClick = [this] { Close(); };
}

void ConfirmDialog::Close() {
    if (pui::UiHost* h = Host()) h->ClosePopup(this);
}

void ConfirmDialog::OnLayout() {
    ConfirmLayout L = ComputeConfirmLayout({Bounds().w(), Bounds().h()});
    float ox = Bounds().l, oy = Bounds().t;
    confirm_->SetBounds(L.confirm.Offset(ox, oy));
    cancel_->SetBounds(L.cancel.Offset(ox, oy));
}

void ConfirmDialog::Paint(pui::IRenderer& r, const pui::Theme& t) {
    ConfirmLayout L = ComputeConfirmLayout({Bounds().w(), Bounds().h()});
    float ox = Bounds().l, oy = Bounds().t;
    r.FillRect(Bounds(), t.smokeFill);
    pui::RectF card = L.card.Offset(ox, oy);
    r.FillRoundRect(card, t.overlayRadius, t.flyoutSurface);
    r.StrokeRoundRect(card.Deflate(0.5f, 0.5f, 0.5f, 0.5f), t.overlayRadius, t.flyoutStroke, 1);
    pui::TextStyle ts = pui::BodyTextStyle(20, 600);
    r.Text(title_, L.title.Offset(ox, oy), ts, t.textPrimary);
    pui::TextStyle ms = pui::BodyTextStyle(14, 400);
    ms.wrap = true;
    ms.valign = pui::ParaAlign::Top;
    r.Text(message_, L.message.Offset(ox, oy), ms, t.textSecondary);
}

bool ConfirmDialog::OnKeyDown(int vk, unsigned mods) {
    (void)mods;
    if (vk == pui::key::Escape) { Close(); return true; }
    return false;
}

ConfirmDialog* ShowConfirmDialog(pui::UiHost& host, const std::wstring& title, const std::wstring& message,
                                 const std::wstring& confirmText, std::function<void()> onConfirm) {
    auto dlg = std::make_unique<ConfirmDialog>(title, message, confirmText, std::move(onConfirm));
    ConfirmDialog* raw = dlg.get();
    raw->SetBounds({0, 0, host.Size().w, host.Size().h});
    pui::PopupOptions o;
    o.modal = true;
    o.lightDismiss = false;
    o.takeFocus = true;
    host.OpenPopup(std::move(dlg), std::move(o));
    host.SetFocus(raw->CancelButton(), true);
    return raw;
}


std::optional<std::wstring> PromptPlaylistName(AppContext& ctx, const std::wstring& title, const std::wstring& label,
                                               const std::wstring& initial, const std::wstring& okText,
                                               const std::wstring& renamingFrom) {
    AppContext* c = &ctx;
    TextPromptOptions o;
    o.title = title;
    o.label = label;
    o.initial = initial;
    o.okText = okText;
    o.placeholder = Tr(L"Playlist name", L"재생목록 이름");
    o.validate = [c, renamingFrom](const std::wstring& v) { return ValidatePlaylistName(c->Svc().playlists, v, renamingFrom); };
    auto r = PromptText(ctx, o);
    if (!r) return std::nullopt;
    std::wstring name = TrimName(*r);
    std::wstring err = ValidatePlaylistName(ctx.Svc().playlists, name, renamingFrom);
    if (!err.empty()) {
        ctx.ShowInfoBar(InfoSeverity::Warning, title, err);
        return std::nullopt;
    }
    return name;
}

bool SavePlaylistFile(AppContext& ctx, const std::wstring& name, const std::vector<wvp::MediaItem>& items) {
    if (ctx.Svc().playlists.Save(name, items)) { ctx.PlaylistsChanged(); return true; }
    ctx.ShowInfoBar(InfoSeverity::Error, Tr(L"Could not save the playlist", L"재생목록을 저장할 수 없습니다"), TrW(L"The playlist \"" + name + L"\" could not be written.", L"재생목록 \"" + name + L"\"을(를) 쓸 수 없습니다.") );
    return false;
}

bool NameUnnamedPlaylist(AppContext& ctx, const std::wstring& name, const std::vector<wvp::MediaItem>& items, int startIndex) {
    if (items.empty() || !SavePlaylistFile(ctx, name, items)) return false;
    PlayPlaylistEntry(ctx, name, startIndex, false);
    ctx.SavePlaylistItems(L"", {});   // the named copy is current before the unnamed slot is cleared
    return true;
}

bool AppendToPlaylist(AppContext& ctx, const std::wstring& name, const std::vector<wvp::MediaItem>& items) {
    if (items.empty()) return false;
    auto all = ctx.Svc().playlists.Load(name);
    all.insert(all.end(), items.begin(), items.end());
    if (!SavePlaylistFile(ctx, name, all)) return false;
    ctx.ShowToast(TrW(L"Added " + ItemCountText((int)items.size()) + L" to " + name, name + L"에 " + ItemCountText((int)items.size()) + L" 추가함"));
    return true;
}

std::vector<pui::MenuItemDesc> AddToPlaylistItems(AppContext& ctx, std::function<std::vector<wvp::MediaItem>()> getItems,
                                                  const std::wstring& exclude) {
    AppContext* c = &ctx;
    std::vector<pui::MenuItemDesc> out;
    for (const auto& info : ctx.Svc().playlists.List()) {
        if (info.isReference) continue;  // referenced sources are not editable destinations
        if (!exclude.empty() && _wcsicmp(info.name.c_str(), exclude.c_str()) == 0) continue;
        std::wstring name = info.name;
        out.push_back(pui::MenuItemDesc::Item(name, [c, name, getItems] { AppendToPlaylist(*c, name, getItems()); }));
    }
    if (out.empty()) {
        auto none = pui::MenuItemDesc::Item(Tr(L"(No playlists)", L"(재생목록 없음)"), [] {});
        none.Disabled();
        out.push_back(std::move(none));
    }
    out.push_back(pui::MenuItemDesc::Separator());
    out.push_back(pui::MenuItemDesc::Item(
        Tr(L"New playlist…", L"새 재생목록…"),
        [c, getItems] {
            auto items = getItems();
            if (items.empty()) return;
            auto name = PromptPlaylistName(*c, Tr(L"New playlist", L"새 재생목록"), Tr(L"Playlist name", L"재생목록 이름"), c->Svc().playlists.CreateUniqueName(), Tr(L"Create", L"만들기"));
            if (!name) return;
            if (SavePlaylistFile(*c, *name, items)) c->ShowToast(TrW(L"Created " + *name + L" with " + ItemCountText((int)items.size()), *name + L" 재생목록을 만들었습니다 (" + ItemCountText((int)items.size()) + L")"));
        },
        {}, pui::glyph::Add));
    return out;
}

}
