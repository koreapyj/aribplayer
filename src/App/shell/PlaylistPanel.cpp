#include "shell/PlaylistPanel.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <random>

#include "BarLogic.h"
#include "Localize.h"
#include "features/DialogGuard.h"
#include "features/NativeMenu.h"
#include "features/OpenFlows.h"
#include "features/ListsCommon.h"
#include "pui/Glyphs.h"
#include "wvp/PlaylistSort.h"

#include <Windows.h>
#include <shellapi.h>

namespace app {

namespace G = pui::glyph;
using pui::RectF;
using plp::PanelAction;

namespace {
constexpr float kPad = 8, kHeaderH = 48, kChipRowH = 48, kChipH = 32, kSearchRowH = 40, kRowH = 42, kRowGap = 2;
constexpr float kSummaryRowH = 32, kFooterPad = 6, kSortW = 32;
constexpr float kFooterH = 1 + kFooterPad + kSummaryRowH + kFooterPad;
constexpr float kIndexW = 16, kRowPadX = 12, kArrowW = 24;

pui::Color WithAlpha(pui::Color c, int a) { return pui::Color((c.v & 0x00FFFFFFu) | ((uint32_t)a << 24)); }

bool IsLocalPath(const std::wstring& url) { return url.find(L"://") == std::wstring::npos; }
}

/// IListSource over the viewed playlist (live controller queue or the snapshot).
class PlaylistPanel::ItemSource final : public pui::IListSource {
public:
    explicit ItemSource(PlaylistPanel& p) : p_(p) {}
    int Count() const override { return (int)p_.Items().size(); }
    std::wstring Text(int row, int) const override {
        const auto& it = p_.Items();
        return row >= 0 && row < (int)it.size() ? it[(size_t)row].DisplayTitle() : std::wstring();
    }
    bool IsCurrent(int row) const override { return row >= 0 && row == p_.CurrentRow(); }
private:
    PlaylistPanel& p_;
};

/// Press-drag horizontal scrolling shared by the strip and its chips: a left press that moves past the slop becomes a scroll
/// drag (the chip then swallows the release so nothing is clicked); a press without that movement stays a click.
struct ChipDrag {
    bool down = false, active = false;
    float x0 = 0, prev = 0, slop = 4;
    std::function<void(float)> scroll;
    void Down(float x) { down = true; active = false; x0 = prev = x; }
    bool Move(float x) {
        if (!down) return false;
        if (!active && std::fabs(x - x0) >= slop) { active = true; prev = x; }
        if (active) { if (scroll) scroll(prev - x); prev = x; }
        return active;
    }
    bool Up() { const bool a = active; down = active = false; return a; }
};

/// Playlist chip: a TextButton whose press-drag scrolls the strip instead of clicking.
class PlaylistPanel::ChipButton final : public TextButton {
public:
    using TextButton::TextButton;
    ChipDrag* drag = nullptr;
    bool OnPointerDown(const pui::PointerEvent& e) override {
        if (!TextButton::OnPointerDown(e)) return false;
        if (drag) drag->Down(e.pt.x);
        return true;
    }
    bool OnPointerMove(const pui::PointerEvent& e) override { return drag && drag->Move(e.pt.x); }
    bool OnPointerUp(const pui::PointerEvent& e) override {
        if (drag && e.button == pui::PointerButton::Left && drag->Up()) return true;   // end of a scroll drag: no click
        return TextButton::OnPointerUp(e);
    }
};

/// Chip container: clips its (scrolled) chips, turns the wheel into horizontal scrolling and drags on empty strip area.
class PlaylistPanel::ChipStrip final : public pui::Widget {
public:
    ChipStrip() : pui::Widget(pui::Role::Group) { SetClipChildren(true); SetHitTestVisible(false); }
    std::function<void(float)> onScroll;
    ChipDrag drag;
    bool OnPointerWheel(const pui::PointerEvent& e) override {
        if (onScroll && e.wheelDelta != 0) onScroll(-e.wheelDelta * 48.f);
        return true;
    }
    bool OnPointerDown(const pui::PointerEvent& e) override {
        if (e.button != pui::PointerButton::Left) return false;
        drag.Down(e.pt.x);
        return true;
    }
    bool OnPointerMove(const pui::PointerEvent& e) override { return drag.Move(e.pt.x); }
    bool OnPointerUp(const pui::PointerEvent& e) override {
        if (e.button != pui::PointerButton::Left) return false;
        drag.Up();
        return true;
    }
    pui::Widget* HitTest(pui::PointF p) override {
        pui::Widget* w = pui::Widget::HitTest(p);
        return w ? w : (Visible() && Bounds().Contains(p) ? this : nullptr);   // empty strip area still takes the wheel
    }
};

/// Hover scroll button over the left / right edge of the chip strip. Only drawn and hit-testable while
/// `shown()` (strip overflows in that direction and the pointer is over the chip row); opaque base so chips do not bleed through.
class PlaylistPanel::ChipArrow final : public pui::Widget {
public:
    ChipArrow(std::wstring name, wchar_t glyph) : pui::Widget(pui::Role::Button), glyph_(glyph) {
        SetName(std::move(name));
        SetFocusable(false);
    }
    std::function<bool()> shown;
    std::function<void()> onClick;
    pui::Widget* HitTest(pui::PointF p) override { return shown && shown() ? pui::Widget::HitTest(p) : nullptr; }
    void Paint(pui::IRenderer& r, const pui::Theme& t) override {
        if (!shown || !shown()) return;
        const RectF b = Bounds();
        r.FillRoundRect(b, t.controlRadius, t.flyoutSurface);
        r.FillRoundRect(b, t.controlRadius, IsPressed() ? t.ctlTertiary : IsHovered() ? t.ctlSecondary : t.ctlDefault);
        pui::TextStyle ts;
        ts.font = pui::FontKind::Icon;
        ts.size = 12;
        ts.align = pui::TextAlign::Center;
        ts.ellipsis = false;
        r.Text(std::wstring(1, glyph_), b, ts, t.textPrimary);
    }
    bool OnPointerDown(const pui::PointerEvent& e) override { return e.button == pui::PointerButton::Left; }
    bool OnPointerUp(const pui::PointerEvent& e) override {
        if (e.button == pui::PointerButton::Left && Bounds().Contains(e.pt) && onClick) onClick();
        return true;
    }
    bool SupportsInvoke() const override { return true; }
    void Invoke() override { if (onClick) onClick(); }
private:
    wchar_t glyph_;
};

/// The list with Ctrl+Home/End/Up/Down/Delete left to the panel's accelerators (the base list ignores the Ctrl modifier there).
class PlaylistPanel::PanelList final : public pui::VirtualList {
public:
    bool OnKeyDown(int vk, unsigned mods) override {
        if (mods & pui::ModCtrl) {
            switch (vk) {
            case pui::key::Home: case pui::key::End: case pui::key::Up: case pui::key::Down: case pui::key::Delete: return false;
            default: break;
            }
        }
        return pui::VirtualList::OnKeyDown(vk, mods);
    }
};

PlaylistPanel::PlaylistPanel(AppContext& ctx, bool overlay) : pui::Widget(pui::Role::Pane), ctx_(ctx), overlay_(overlay) {
    SetName(Tr(L"Playlist panel", L"재생목록 패널"));
    SetAutomationId(overlay ? L"PlaylistPanelOverlay" : L"PlaylistPanel");
    SetClipChildren(true);
    source_ = std::make_unique<ItemSource>(*this);

    tab_ = Add<TextButton>(Tr(L"Playlist", L"재생목록"));
    tab_->SetSelected(true);
    tab_->SetFocusable(false);        // single tab: a header, not a control
    tab_->SetHitTestVisible(false);

    strip_ = Add<ChipStrip>();
    strip_->onScroll = [this](float d) { chipScroll_ += d; LayoutChips(); };
    strip_->drag.scroll = strip_->onScroll;
    auto overChips = [this] {   // pointer over the chip row (strip, a chip or an arrow)
        pui::UiHost* h = Host();
        pui::Widget* hv = h ? h->Hovered() : nullptr;
        return hv && (strip_->IsAncestorOf(hv) || hv == arrowL_ || hv == arrowR_);
    };
    arrowL_ = Add<ChipArrow>(Tr(L"Scroll playlists left", L"재생목록 왼쪽으로 스크롤"), G::ChevronLeftSmall);
    arrowR_ = Add<ChipArrow>(Tr(L"Scroll playlists right", L"재생목록 오른쪽으로 스크롤"), G::ChevronRightSmall);
    arrowL_->shown = [this, overChips] { return chipScroll_ > 0.5f && overChips(); };
    arrowR_->shown = [this, overChips] { return chipScroll_ < chipMax_ - 0.5f && overChips(); };
    arrowL_->onClick = [this] { ScrollChipsPage(-1); };
    arrowR_->onClick = [this] { ScrollChipsPage(1); };
    plus_ = Add<GlyphButton>(Tr(L"Playlists menu", L"재생목록 메뉴"), G::Str(G::Add), 14.f, GlyphButton::Shape::Rounded);
    plus_->SetToolTip(Tr(L"New playlist, import ... (Ctrl+N: new playlist)", L"새 재생목록, 가져오기 ... (Ctrl+N: 새 재생목록)"));
    plus_->onInvoke = [this] { ShowPlusMenu(); };

    search_ = Add<pui::TextBox>();
    search_->SetPlaceholder(Tr(L"Search", L"검색"));
    search_->SetName(Tr(L"Search playlist (Ctrl+S)", L"재생목록 검색 (Ctrl+S)"));
    search_->onChanged = [this](const std::wstring& s) { filter_ = s; UpdateFilter(); };

    list_ = Add<PanelList>();
    list_->SetName(Tr(L"Playlist items", L"재생목록 항목"));
    list_->SetAutomationId(L"PlaylistPanelList");
    list_->SetRowHeight(kRowH);
    list_->SetSelectionMode(pui::SelectionMode::Extended);
    list_->SetReorderEnabled(true);
    list_->SetShowGlyphs(false);
    list_->SetSource(source_.get());
    list_->SetEmptyText(Tr(L"This playlist is empty. Add files or drop media here.", L"재생목록이 비어 있습니다. 파일을 추가하거나 여기에 끌어다 놓으세요."));
    list_->SetRowPainter([this](pui::IRenderer& r, const pui::Theme& t, const pui::RowPaintArgs& a) { PaintRow(r, t, a); });
    list_->onInvoke = [this](int row) { PlayRow(row); };
    list_->onDelete = [this](std::vector<int> rows) { RemoveRows(std::move(rows)); };
    list_->onReorder = [this](std::vector<int> rows, int before) {
        if (!Editable()) return;
        ApplyOrder(pui::vl::ReorderOrder((int)Items().size(), rows, before), rows);
    };
    list_->onSelectionChanged = [this] { UpdateButtons(); };
    list_->onContextMenu = [this](int row, pui::PointF pt, bool) { ShowRowMenu(row, pt); };

    sort_ = Add<GlyphButton>(Tr(L"Sort", L"정렬"), G::Str(G::Sort), 14.f, GlyphButton::Shape::Rounded);
    sort_->SetToolTip(Tr(L"Sort", L"정렬"));
    sort_->onInvoke = [this] { ShowSortMenu(); };
    stale_ = true;   // filled by Refresh() when the panel is first shown
}

PlaylistPanel::~PlaylistPanel() { *alive_ = false; }

pui::VirtualList* PlaylistPanel::List() const { return list_; }

bool PlaylistPanel::Live() const { return ctx_.Controller() && ctx_.IsCurrentPlaylist(viewed_); }

const std::vector<wvp::MediaItem>& PlaylistPanel::Items() const {
    if (Live()) return ctx_.Controller()->Items();
    return items_;
}

int PlaylistPanel::CurrentRow() const { return Live() ? ctx_.Controller()->CurrentIndex() : -1; }

void PlaylistPanel::ReloadSnapshot() {
    items_ = Live() ? std::vector<wvp::MediaItem>{} : ctx_.PlaylistItems(viewed_);
    readOnly_ = !viewed_.empty() && ctx_.IsPlaylistReadOnly(viewed_);
    RefreshPlayed();
}

void PlaylistPanel::RefreshPlayed() {
    played_.clear();
    const auto& items = Items();
    std::vector<std::wstring> urls;
    urls.reserve(items.size());
    for (const auto& it : items) urls.push_back(it.url);
    const std::vector<bool> flags = ctx_.PlayedFlags(urls);
    for (size_t i = 0; i < urls.size() && i < flags.size(); i++) if (flags[i]) played_.insert(urls[i]);
}

void PlaylistPanel::OnItemPlayed(const std::wstring& url) {
    if (!played_.insert(url).second) return;
    if (IsEffectivelyVisible()) Invalidate();
}

void PlaylistPanel::Refresh() {
    viewed_ = ctx_.CurrentPlaylistName();   // shown / reopened: the view returns to the last played playlist (browsing chips is temporary)
    Reload();
    int row = CurrentRow();
    if (row < 0 && viewed_ == ctx_.CurrentPlaylistName()) {   // nothing current yet (fresh start): the last played item of the restored list
        const auto state = ctx_.Svc().playlists.GetPlaylistPlaybackState(viewed_);
        const auto& items = Items();
        auto found = std::find_if(items.begin(), items.end(), [&](const wvp::MediaItem& it) { return !state.lastItem.empty() && it.url == state.lastItem; });
        if (found != items.end()) row = (int)(found - items.begin());
        else if (state.index >= 0 && state.index < (int)items.size() && !state.lastItem.empty()) row = state.index;
        if (row >= 0) {
            const int v = list_->ViewRowOf(row);
            if (v >= 0) list_->ScrollIntoViewRow(v);
        }
    }
    if (row >= 0) SelectRows({row});
}

void PlaylistPanel::Reload() {
    stale_ = false;
    RebuildChips();
    ReloadSnapshot();
    Changed();
    ScrollToCurrent();
}

void PlaylistPanel::SetViewed(const std::wstring& name) {
    viewed_ = name;
    ReloadSnapshot();
    list_->SetSelection({});
    list_->SetOffset(0);
    Changed();
    ScrollToCurrent();
    for (size_t i = 0; i < chips_.size(); i++) chips_[i]->SetSelected(chipNames_[i] == viewed_);
    for (size_t i = 0; i < chips_.size(); i++) if (chipNames_[i] == viewed_) EnsureChipVisible((int)i);
}

void PlaylistPanel::OnQueueChanged() {
    if (!IsEffectivelyVisible()) { stale_ = true; return; }
    if (!Live()) return;
    RefreshPlayed();
    Changed();
}

void PlaylistPanel::OnCurrentChanged() {
    if (!IsEffectivelyVisible()) { stale_ = true; return; }
    if (Live()) { Invalidate(); ScrollToCurrent(); }
}

void PlaylistPanel::OnCurrentPlaylistChanged() {
    viewed_ = ctx_.CurrentPlaylistName();   // the panel follows what plays
    if (!IsEffectivelyVisible()) { stale_ = true; return; }
    Refresh();
}

void PlaylistPanel::OnPlaylistRenamed(const std::wstring& from, const std::wstring& to) {
    if (viewed_ == from) viewed_ = to;
    if (!IsEffectivelyVisible()) { stale_ = true; return; }
    Reload();
}

void PlaylistPanel::OnPlaylistsChanged() {
    if (!IsEffectivelyVisible()) { stale_ = true; return; }
    RebuildChips();
    if (!Live()) { ReloadSnapshot(); Changed(); }
}

void PlaylistPanel::OnPlaylistDeleted(const std::wstring& name) {
    if (viewed_ == name) viewed_ = ctx_.CurrentPlaylistName() == name ? std::wstring() : ctx_.CurrentPlaylistName();
    if (!IsEffectivelyVisible()) { stale_ = true; return; }
    Reload();
}

void PlaylistPanel::Changed() {
    UpdateFilter();
    list_->SetReorderEnabled(Editable());
    UpdateSummary();
    UpdateButtons();
    Invalidate();
}

void PlaylistPanel::UpdateFilter() {
    if (filter_.empty()) list_->SetFilter(nullptr);
    else list_->SetFilter([this](int row) { const auto& it = Items(); return row >= 0 && row < (int)it.size() && plp::FilterMatches(it[(size_t)row], filter_); });
    list_->SourceChanged();
    list_->SetEmptyText(!filter_.empty() && !Items().empty() ? Tr(L"No matches", L"검색 결과가 없습니다")
                                                              : Tr(L"This playlist is empty. Add files or drop media here.", L"재생목록이 비어 있습니다. 파일을 추가하거나 여기에 끌어다 놓으세요."));
}

void PlaylistPanel::UpdateSummary() {
    const auto& it = Items();
    summary_ = plp::SummaryText((int)it.size(), plp::TotalKnownDuration(it));
}

void PlaylistPanel::UpdateButtons() {
    const bool edit = Editable();
    sort_->SetEnabled(edit);
    ctx_.OnPanelSelectionChanged();
}

void PlaylistPanel::ScrollToCurrent() {
    int row = CurrentRow();
    if (row < 0) return;
    int v = list_->ViewRowOf(row);
    if (v >= 0) list_->ScrollIntoViewRow(v);
}

std::vector<int> PlaylistPanel::SelectedRows() const { return list_->Selection(); }
void PlaylistPanel::SelectRows(std::vector<int> rows) { list_->SetSelection(std::move(rows)); UpdateButtons(); }

float PlaylistPanel::Px() const { return 1.f / std::max(0.25f, ctx_.Scale()); }

void PlaylistPanel::RebuildChips() {
    for (TextButton* c : chips_) strip_->RemoveChild(c);
    chips_.clear(); chipNames_.clear(); chipReadOnly_.clear();
    std::vector<std::pair<std::wstring, bool>> entries{{std::wstring(), false}};
    for (const auto& info : ctx_.Svc().playlists.List()) entries.emplace_back(info.name, info.isReference);
    // a viewed playlist that no longer exists falls back to the current one
    bool found = false;
    for (const auto& e : entries) found |= e.first == viewed_;
    if (!found) viewed_ = ctx_.CurrentPlaylistName();
    for (const auto& e : entries) {
        auto* chip = strip_->Add<ChipButton>(plp::ChipLabel(e.first));
        chip->drag = &strip_->drag;
        chip->SetChipStyle(true);
        chip->SetFontSize(13);
        chip->SetSelected(e.first == viewed_);
        chip->SetToolTip(e.second ? plp::ChipLabel(e.first) + Tr(L" (read-only)", L" (읽기 전용)") : plp::ChipLabel(e.first));
        if (e.second) chip->SetLeadGlyph(G::Str(G::Link));   // read-only reference playlist
        const int index = (int)chips_.size();
        chip->onInvoke = [this, index] { if (index < (int)chipNames_.size() && chipNames_[(size_t)index] != viewed_) SetViewed(chipNames_[(size_t)index]); };
        chip->onRightClick = [this, index] { ShowChipMenu(index); };
        chips_.push_back(chip);
        chipNames_.push_back(e.first);
        chipReadOnly_.push_back(e.second);
    }
    LayoutChips();
    for (size_t i = 0; i < chips_.size(); i++) if (chipNames_[i] == viewed_) EnsureChipVisible((int)i);
}

void PlaylistPanel::LayoutChips() {
    const RectF s = strip_->Bounds();
    float total = 0;
    std::vector<float> w;
    for (TextButton* c : chips_) { w.push_back(c->DesiredWidth(ctx_.Measurer())); total += w.back() + 4; }
    total = std::max(0.f, total - 4);
    chipMax_ = std::max(0.f, total - s.w());
    chipScroll_ = std::clamp(chipScroll_, 0.f, chipMax_);
    float x = s.l - chipScroll_;
    for (size_t i = 0; i < chips_.size(); i++) {
        chips_[i]->SetBounds({x, s.t, x + w[i], s.t + kChipH});
        x += w[i] + 4;
    }
    Invalidate();
}

void PlaylistPanel::EnsureChipVisible(int index) {
    if (index < 0 || index >= (int)chips_.size()) return;
    const RectF s = strip_->Bounds();
    if (s.w() <= 0) { chipReveal_ = true; return; }   // not laid out yet: OnLayout reveals the viewed chip
    const RectF c = chips_[(size_t)index]->Bounds();
    if (c.l < s.l) chipScroll_ -= s.l - c.l;
    else if (c.r > s.r) chipScroll_ += c.r - s.r;
    LayoutChips();
}

/// Hover arrow click: the first chip cut off on that side becomes the edge chip (about one viewport per click).
void PlaylistPanel::ScrollChipsPage(int dir) {
    const RectF s = strip_->Bounds();
    if (dir > 0) {
        for (TextButton* c : chips_) {
            const RectF b = c->Bounds();
            if (b.r <= s.r + 0.5f) continue;
            chipScroll_ += b.l - s.l > 0.5f ? b.l - s.l : b.r - s.r;
            break;
        }
    } else {
        for (size_t i = chips_.size(); i-- > 0;) {
            const RectF b = chips_[i]->Bounds();
            if (b.l >= s.l - 0.5f) continue;
            chipScroll_ += b.r - s.r < -0.5f ? b.r - s.r : b.l - s.l;
            break;
        }
    }
    LayoutChips();
}

bool PlaylistPanel::Perform(PanelAction a) {
    switch (a) {
    case PanelAction::None: return false;
    case PanelAction::Play: {
        auto sel = SelectedRows();
        int fv = list_->FocusedRow();
        PlayRow(fv >= 0 ? list_->SourceRowOf(fv) : (sel.empty() ? -1 : sel.front()));
        return true;
    }
    case PanelAction::SelectAll: {
        std::vector<int> all;
        for (int v = 0; v < list_->ViewCount(); v++) all.push_back(list_->SourceRowOf(v));
        SelectRows(std::move(all));
        return true;
    }
    case PanelAction::InvertSelection: {
        std::vector<int> visible, cur = SelectedRows(), out;
        for (int v = 0; v < list_->ViewCount(); v++) visible.push_back(list_->SourceRowOf(v));
        for (int r : visible) if (!std::binary_search(cur.begin(), cur.end(), r)) out.push_back(r);
        SelectRows(std::move(out));
        return true;
    }
    case PanelAction::RemoveSelected: RemoveRows(SelectedRows()); return true;
    case PanelAction::RemoveUnselected: {
        if (SelectedRows().empty()) return true;
        RemoveRows(plp::Complement((int)Items().size(), SelectedRows()));
        return true;
    }
    case PanelAction::RemoveAbove: RemoveRows(plp::RowsAbove(SelectedRows())); return true;
    case PanelAction::RemoveMissing: RemoveMissing(); return true;
    case PanelAction::MoveTop: Move(wvp::MoveOp::Top); return true;
    case PanelAction::MoveUp: Move(wvp::MoveOp::Up); return true;
    case PanelAction::MoveDown: Move(wvp::MoveOp::Down); return true;
    case PanelAction::MoveBottom: Move(wvp::MoveOp::Bottom); return true;
    case PanelAction::AddFiles: case PanelAction::AddFolder: case PanelAction::AddUrl: AddSource(a); return true;
    case PanelAction::NewAlbum: NewAlbum(); return true;
    case PanelAction::RenameAlbum: RenameAlbum(viewed_); return true;
    case PanelAction::DeleteAlbum: DeleteAlbum(viewed_); return true;
    case PanelAction::FocusSearch: FocusSearch(); return true;
    default: SortBy(a); return true;
    }
}

bool PlaylistPanel::OnKeyDown(int vk, unsigned mods) {
    pui::UiHost* host = Host();
    const pui::Widget* f = host ? host->Focused() : nullptr;
    if (f == search_) {   // typing: Esc (box already empty) / Enter hand the keyboard back to the player; Ctrl accelerators still work
        if (mods == 0 && (vk == pui::key::Escape || vk == pui::key::Enter)) { host->SetFocus(nullptr); return true; }
        PanelAction a = plp::ActionForKey(vk, mods, false);
        if (a == PanelAction::None) return false;
        Perform(a);
        return true;
    }
    if (!active_) return false;
    PanelAction a = plp::ActionForKey(vk, mods, true);
    if (a != PanelAction::None) {
        Perform(a);
        return true;   // consumed even when the action was a no-op: panel accelerators never leak into the app key table
    }
    switch (vk) {   // list keys; anything else (Left/Right, Space, F-keys ...) falls through to the player shortcuts
    case pui::key::Up: case pui::key::Down: case pui::key::Home: case pui::key::End: case pui::key::PageUp: case pui::key::PageDown:
    case pui::key::Enter: case pui::key::Delete:
        return !(mods & (pui::ModCtrl | pui::ModAlt)) && list_->OnKeyDown(vk, mods);
    default: return false;
    }
}

void PlaylistPanel::FocusSearch() {
    active_ = true;
    search_->Focus();
    search_->SelectAll();
}

void PlaylistPanel::PlayRow(int row) {
    if (row < 0 || row >= (int)Items().size()) return;
    if (Live()) ctx_.Controller()->PlayAt(row);
    else PlayPlaylistEntry(ctx_, viewed_, row, false);
}

void PlaylistPanel::RemoveRows(std::vector<int> rows) {
    if (!Editable() || rows.empty()) return;
    const int n = (int)Items().size();
    std::sort(rows.begin(), rows.end());
    rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
    rows.erase(std::remove_if(rows.begin(), rows.end(), [n](int r) { return r < 0 || r >= n; }), rows.end());
    if (rows.empty()) return;
    list_->SetSelection({});
    if (Live()) {
        ctx_.Controller()->RemoveMany(rows);   // QueueChanged -> OnQueueChanged
        return;
    }
    std::vector<wvp::MediaItem> next = items_;
    for (auto it = rows.rbegin(); it != rows.rend(); ++it) next.erase(next.begin() + *it);
    Commit(std::move(next));
}

void PlaylistPanel::RemoveMissing() {
    std::vector<int> missing;
    const auto& items = Items();
    for (int i = 0; i < (int)items.size(); i++) {
        if (!IsLocalPath(items[(size_t)i].url)) continue;
        std::error_code ec;
        if (!std::filesystem::exists(std::filesystem::path(items[(size_t)i].url), ec) && !ec) missing.push_back(i);
    }
    RemoveRows(std::move(missing));
}

void PlaylistPanel::Move(wvp::MoveOp op) {
    if (!Editable()) return;
    std::vector<int> sel = SelectedRows();
    if (sel.empty()) return;
    if (Live()) {
        SelectRows(ctx_.Controller()->MoveSelection(sel, op));   // QueueChanged refreshed the list already
        return;
    }
    std::vector<int> moved;
    std::vector<int> order = plp::MovePermutation((int)items_.size(), sel, op, &moved);
    ApplyOrder(order, sel);
}

void PlaylistPanel::ApplyOrder(const std::vector<int>& order, const std::vector<int>& keepSelected) {
    const int n = (int)Items().size();
    if ((int)order.size() != n) return;
    bool identity = true;
    for (int i = 0; i < n; i++) identity &= order[(size_t)i] == i;
    if (identity) return;
    if (Live()) {
        if (!ctx_.Controller()->Reorder(order)) return;
    } else {
        std::vector<wvp::MediaItem> next;
        next.reserve(items_.size());
        for (int old : order) next.push_back(items_[(size_t)old]);
        Commit(std::move(next));
    }
    SelectRows(plp::RemapSelection(order, keepSelected));
}

void PlaylistPanel::SortBy(PanelAction a) {
    if (!Editable()) return;
    const int n = (int)Items().size();
    if (n < 2) return;
    std::vector<int> order;
    if (a == PanelAction::Reverse) order = wvp::ReverseOrder((size_t)n);
    else {
        wvp::SortKey key = wvp::SortKey::FileName;
        switch (a) {
        case PanelAction::SortTitle: key = wvp::SortKey::Title; break;
        case PanelAction::SortExtension: key = wvp::SortKey::Extension; break;
        case PanelAction::SortSize: key = wvp::SortKey::Size; break;
        case PanelAction::SortDuration: key = wvp::SortKey::Duration; break;
        case PanelAction::SortDate: key = wvp::SortKey::Date; break;
        case PanelAction::Shuffle: key = wvp::SortKey::Random; break;
        case PanelAction::SortFolder: key = wvp::SortKey::Folder; break;
        default: break;   // Sort / SortFileName: file name ascending
        }
        order = wvp::SortOrder(Items(), key, false, {}, std::random_device{}());
    }
    ApplyOrder(order, SelectedRows());
}

void PlaylistPanel::Commit(std::vector<wvp::MediaItem> items) {
    if (!ctx_.SavePlaylistItems(viewed_, items)) { ReloadSnapshot(); Changed(); return; }   // SavePlaylistItems reports the error
    items_ = std::move(items);
    Changed();
}

void PlaylistPanel::AddSource(PanelAction a) {
    if (!Editable()) return;
    auto alive = alive_;
    auto done = [this, alive] { if (alive && *alive) AfterAdd(); };
    switch (a) {
    case PanelAction::AddFiles: AddFilesToPlaylist(ctx_, viewed_, done); break;
    case PanelAction::AddFolder: AddFolderToPlaylist(ctx_, viewed_, done); break;
    default: AddUrlToPlaylist(ctx_, viewed_, done); break;
    }
}

void PlaylistPanel::AfterAdd() {
    if (!Live()) ReloadSnapshot();
    Changed();
}

bool PlaylistPanel::DropPaths(const std::vector<std::wstring>& paths, pui::PointF dip) {
    if (!Editable() || paths.empty()) return false;
    // insertion row (source index) under the drop point; no filter -> the drop lands there, otherwise it is appended
    int before = -1;
    if (!list_->IsFiltered()) {
        const int n = (int)Items().size();
        int v = list_->RowAt(dip);
        if (v >= 0) {
            before = v;
            if (dip.y > list_->RowRect(v).cy()) before = std::min(n, v + 1);
        }
    }
    const int oldCount = (int)Items().size();
    auto alive = alive_;
    DropPathsToPlaylist(ctx_, paths, viewed_, [this, alive, before, oldCount] {
        if (!alive || !*alive) return;
        if (!Live()) ReloadSnapshot();
        const int n = (int)Items().size();
        if (before >= 0 && before < oldCount && n > oldCount) {   // move the appended block to the drop row
            std::vector<int> block;
            for (int i = oldCount; i < n; i++) block.push_back(i);
            ApplyOrder(pui::vl::ReorderOrder(n, block, before), {});
        }
        Changed();
    });
    return true;
}

void PlaylistPanel::NewAlbum() {
    auto name = lists::PromptPlaylistName(ctx_, Tr(L"New playlist", L"새 재생목록"), Tr(L"Playlist name", L"재생목록 이름"),
                                          ctx_.Svc().playlists.CreateUniqueName(Tr(L"New playlist", L"새 재생목록")), Tr(L"Create", L"만들기"));
    if (!name) return;
    if (!ctx_.Svc().playlists.Save(*name, {})) {
        ctx_.ShowInfoBar(InfoSeverity::Error, Tr(L"Could not create the playlist", L"재생목록을 만들 수 없습니다"), *name);
        return;
    }
    viewed_ = *name;
    Reload();
}

void PlaylistPanel::RenameAlbum(const std::wstring& name) {
    if (name.empty()) return;
    auto to = lists::PromptPlaylistName(ctx_, Tr(L"Rename playlist", L"재생목록 이름 바꾸기"), Tr(L"Playlist name", L"재생목록 이름"), name,
                                        Tr(L"Rename", L"이름 바꾸기"), name);
    if (!to || *to == name) return;
    std::wstring err;
    if (!ctx_.Svc().playlists.Rename(name, *to, &err)) {
        ctx_.ShowInfoBar(InfoSeverity::Error, Tr(L"Could not rename the playlist", L"이름을 바꿀 수 없습니다"),
                         err.empty() ? Tr(L"The playlist could not be renamed.", L"재생목록 이름을 바꿀 수 없습니다.") : err);
        return;
    }
    ctx_.PlaylistRenamed(name, *to);   // App forwards to every panel: viewed follows, chips refresh
}

void PlaylistPanel::AlbumAdded(const std::wstring& name) {
    ctx_.PlaylistsChanged();   // every panel instance rebuilds its chips
    viewed_ = name;
    Reload();
}

void PlaylistPanel::RefreshAlbum(const std::wstring& name) {
    if (!ctx_.Svc().playlists.IsReference(name)) return;
    const std::weak_ptr<bool> weak = alive_;
    RefreshPlaylistReference(ctx_, name, [this, weak] {
        auto a = weak.lock();
        if (!a || !*a) return;
        ctx_.PlaylistsChanged();   // item counts / cached items changed
        Reload();
    });
}

void PlaylistPanel::EditableCopyOfAlbum(const std::wstring& name) {
    const std::weak_ptr<bool> weak = alive_;
    ImportReferenceAsEditableCopy(ctx_, name, [this, weak](const std::wstring& copy) { if (auto a = weak.lock(); a && *a) AlbumAdded(copy); });
}

void PlaylistPanel::SaveDefaultAsAlbum() {
    auto items = ctx_.PlaylistItems(L"");
    if (items.empty()) return;
    auto name = lists::PromptPlaylistName(ctx_, Tr(L"Save as playlist", L"재생목록으로 저장"), Tr(L"Playlist name", L"재생목록 이름"),
                                          ctx_.Svc().playlists.CreateUniqueName(Tr(L"New playlist", L"새 재생목록")), Tr(L"Save", L"저장"));
    if (!name) return;
    int start = 0;
    if (ctx_.IsCurrentPlaylist(L""))
        if (auto* ctl = ctx_.Controller(); ctl && ctl->CurrentIndex() >= 0) start = ctl->CurrentIndex();
    if (!lists::NameUnnamedPlaylist(ctx_, *name, items, start)) return;
    ctx_.ShowToast(TrW(L"Saved playlist " + *name, L"재생목록 " + *name + L" 저장함"));
    AlbumAdded(*name);
}

void PlaylistPanel::DeleteAlbum(const std::wstring& name) {
    if (name.empty() || !Host()) return;
    lists::ShowConfirmDialog(*Host(), Tr(L"Delete this playlist?", L"이 앨범을 지울까요?"),
                             std::wstring(L"\"") + name + L"\"", Tr(L"Delete", L"삭제"), [ctx = &ctx_, name] {
        if (!ctx->Svc().playlists.Delete(name)) {
            ctx->ShowInfoBar(InfoSeverity::Error, Tr(L"Could not delete the playlist", L"재생목록을 지울 수 없습니다"), name);
            return;
        }
        ctx->PlaylistDeleted(name);   // App forwards to every panel
    });
}

pui::MenuItemDesc PlaylistPanel::Item(const wchar_t* en, const wchar_t* ko, PanelAction a, bool enabled) {
    auto d = pui::MenuItemDesc::Item(Tr(en, ko), [this, a] { Perform(a); }, plp::AccelText(a));
    if (!enabled || !Editable()) d.Disabled();
    return d;
}

void PlaylistPanel::ShowMenuFor(TextButton* anchor, std::vector<pui::MenuItemDesc> items) {
    nativemenu::ShowBelow(std::move(items), anchor->Bounds(), true, {[anchor] { anchor->SetMenuOpen(true); }, [anchor] { anchor->SetMenuOpen(false); }});
}

void PlaylistPanel::ShowMenuForButton(GlyphButton* anchor, std::vector<pui::MenuItemDesc> items) {
    nativemenu::ShowBelow(std::move(items), anchor->Bounds(), true, {[anchor] { anchor->SetMenuOpen(true); }, [anchor] { anchor->SetMenuOpen(false); }});
}

void PlaylistPanel::ShowPlusMenu() {
    if (!Host()) return;
    const std::weak_ptr<bool> weak = alive_;
    auto added = [this, weak](const std::wstring& name) { if (auto a = weak.lock(); a && *a) AlbumAdded(name); };
    std::vector<pui::MenuItemDesc> m;
    m.push_back(pui::MenuItemDesc::Item(Tr(L"New playlist…", L"새 재생목록…"), [this] { NewAlbum(); }, plp::AccelText(PanelAction::NewAlbum), G::Add));
    m.push_back(pui::MenuItemDesc::Item(Tr(L"Import playlist file…", L"재생목록 파일 가져오기…"), [this, added] { ImportPlaylist(ctx_, added); }, {}, G::OpenFile));
    m.push_back(pui::MenuItemDesc::Item(Tr(L"Import from URL…", L"URL에서 가져오기…"), [this, added] { AddPlaylistUrlFromPrompt(ctx_, added); }, {}, G::OpenUrl));
    m.push_back(pui::MenuItemDesc::Separator());
    m.push_back(pui::MenuItemDesc::Item(Tr(L"Open playlists folder", L"재생목록 폴더 열기"), [this] {
        const std::wstring dir = ctx_.Svc().playlists.Dir();
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (!OsDialogsBlocked()) ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }, {}, G::OpenFolder));
    ShowMenuForButton(plus_, std::move(m));
}

void PlaylistPanel::ShowSortMenu() {
    ShowMenuForButton(sort_, {Item(L"Sort", L"정렬", PanelAction::Sort), Item(L"Reverse order", L"역순 정렬", PanelAction::Reverse),
                              pui::MenuItemDesc::Separator(),
                              Item(L"By title", L"제목", PanelAction::SortTitle), Item(L"By file name", L"파일명", PanelAction::SortFileName),
                              Item(L"By extension", L"확장자", PanelAction::SortExtension), Item(L"By size", L"크기", PanelAction::SortSize),
                              Item(L"By duration", L"재생 시간", PanelAction::SortDuration), Item(L"By date", L"날짜", PanelAction::SortDate),
                              Item(L"Shuffle order", L"임의 정렬", PanelAction::Shuffle), Item(L"By folder", L"폴더별", PanelAction::SortFolder)});
}

/// Playlist menu (chip right click, or empty list space): playlist-level items for the viewed playlist.
std::vector<pui::MenuItemDesc> PlaylistPanel::BuildPlaylistMenu() {
    const std::wstring name = viewed_;
    const bool unnamed = name.empty(), reference = !unnamed && ctx_.Svc().playlists.IsReference(name);
    std::vector<pui::MenuItemDesc> m;
    m.push_back(pui::MenuItemDesc::Item(Tr(L"Play playlist", L"재생목록 재생"), [this, name] { PlayPlaylistEntry(ctx_, name, 0, true); }, {}, G::Play));
    auto add = pui::MenuItemDesc::Sub(Tr(L"Add", L"추가"),
                                      {Item(L"Files…", L"파일…", PanelAction::AddFiles), Item(L"Folder…", L"폴더…", PanelAction::AddFolder),
                                       Item(L"URL…", L"주소…", PanelAction::AddUrl)}, G::Add);
    if (!Editable()) add.Disabled();
    m.push_back(std::move(add));
    if (!ctx_.IsCurrentPlaylist(name))
        m.push_back(pui::MenuItemDesc::Item(Tr(L"Add to current playlist", L"현재 재생목록에 추가"), [this, name] {
            auto items = ctx_.PlaylistItems(name);
            if (items.empty()) { ctx_.ShowToast(Tr(L"This playlist is empty", L"재생목록이 비어 있습니다")); return; }
            const std::wstring count = lists::ItemCountText((int)items.size());
            ctx_.AppendToCurrentPlaylist(std::move(items));
            ctx_.ShowToast(TrW(L"Added " + count + L" to the current playlist", L"현재 재생목록에 " + count + L" 추가함"));
        }, {}, G::Add));
    m.push_back(pui::MenuItemDesc::Separator());
    if (unnamed) {
        auto save = pui::MenuItemDesc::Item(Tr(L"Save as playlist…", L"재생목록으로 저장…"), [this] { SaveDefaultAsAlbum(); }, {}, G::Save);
        if (ctx_.PlaylistItems(L"").empty()) save.Disabled();
        m.push_back(std::move(save));
    } else {
        m.push_back(pui::MenuItemDesc::Item(Tr(L"Rename…", L"이름 바꾸기…"), [this, name] { RenameAlbum(name); }, plp::AccelText(PanelAction::RenameAlbum), G::Rename));
        m.push_back(pui::MenuItemDesc::Item(Tr(L"Delete playlist", L"재생목록 삭제"), [this, name] { DeleteAlbum(name); }, plp::AccelText(PanelAction::DeleteAlbum), G::Delete));
    }
    if (reference) {
        m.push_back(pui::MenuItemDesc::Separator());
        m.push_back(pui::MenuItemDesc::Item(Tr(L"Refresh", L"새로 고침"), [this, name] { RefreshAlbum(name); }, {}, G::Refresh));
        m.push_back(pui::MenuItemDesc::Item(Tr(L"Make editable copy", L"편집 가능한 사본 만들기"), [this, name] { EditableCopyOfAlbum(name); }, {}, G::Copy));
    }
    return m;
}

/// Item menu (row right click; the row is selected first): acts on the selection of the viewed playlist.
std::vector<pui::MenuItemDesc> PlaylistPanel::BuildItemMenu(int row) {
    const std::vector<int> rows = SelectedRows();
    std::vector<pui::MenuItemDesc> m;
    m.push_back(pui::MenuItemDesc::Item(Tr(L"Play", L"재생"), [this, row] { PlayRow(row); }, L"Enter", G::Play));
    auto pick = [this, rows] {
        std::vector<wvp::MediaItem> out;
        for (int r : rows) if (r >= 0 && r < (int)Items().size()) out.push_back(Items()[(size_t)r]);
        return out;
    };
    m.push_back(pui::MenuItemDesc::Sub(Tr(L"Copy", L"복사"), lists::AddToPlaylistItems(ctx_, pick, viewed_), G::Copy));
    auto above = Item(L"Delete items above", L"위쪽 항목 삭제", PanelAction::RemoveAbove, !rows.empty() && rows.front() > 0);
    auto del = pui::MenuItemDesc::Sub(Tr(L"Delete", L"삭제"),
                                      {Item(L"Delete", L"삭제", PanelAction::RemoveSelected),
                                       Item(L"Delete others", L"다른 항목 삭제", PanelAction::RemoveUnselected),
                                       Item(L"Delete missing files", L"없는 파일 삭제", PanelAction::RemoveMissing),
                                       std::move(above)}, G::Delete);
    if (!Editable()) del.Disabled();
    m.push_back(std::move(del));
    m.push_back(pui::MenuItemDesc::Separator());
    m.push_back(Item(L"Move to top", L"맨 위로", PanelAction::MoveTop, !rows.empty()));
    m.push_back(Item(L"Move to bottom", L"맨 아래로", PanelAction::MoveBottom, !rows.empty()));
    return m;
}

void PlaylistPanel::ShowChipMenu(int index) {
    if (index < 0 || index >= (int)chips_.size() || !Host()) return;
    if (chipNames_[(size_t)index] != viewed_) SetViewed(chipNames_[(size_t)index]);   // the menu acts on the chip's playlist
    ShowMenuFor(chips_[(size_t)index], BuildPlaylistMenu());
}

void PlaylistPanel::ShowRowMenu(int row, pui::PointF pt) {
    if (row < 0 || row >= (int)Items().size()) {   // empty list space: the playlist menu
        nativemenu::ShowAtPoint(BuildPlaylistMenu(), pt);
        return;
    }
    auto rows = SelectedRows();
    if (std::find(rows.begin(), rows.end(), row) == rows.end()) SelectRows({row});
    nativemenu::ShowAtPoint(BuildItemMenu(row), pt);
}

void PlaylistPanel::OnLayout() {
    const RectF in = Bounds().Deflate(1, 1, 1, 1);
    float y = in.t;
    tab_->SetBounds({in.l + kPad, y + kPad, in.l + kPad + tab_->DesiredWidth(ctx_.Measurer()), y + kHeaderH});
    y += kHeaderH;
    plus_->SetBounds({in.r - kPad - kChipH, y + kPad, in.r - kPad, y + kPad + kChipH});
    strip_->SetBounds({in.l + kPad, y + kPad, plus_->Bounds().l - 4, y + kPad + kChipH});
    strip_->drag.slop = (float)GetSystemMetrics(SM_CXDRAG) / std::max(0.25f, ctx_.Scale());
    const RectF sb = strip_->Bounds();
    arrowL_->SetBounds({sb.l, sb.t, sb.l + kArrowW, sb.b});
    arrowR_->SetBounds({sb.r - kArrowW, sb.t, sb.r, sb.b});
    LayoutChips();
    if (chipReveal_ && sb.w() > 0) {
        chipReveal_ = false;
        for (size_t i = 0; i < chips_.size(); i++) if (chipNames_[i] == viewed_) EnsureChipVisible((int)i);
    }
    y += kChipRowH;
    sort_->SetBounds({in.r - kPad - kSortW, y, in.r - kPad, y + pui::TextBox::kHeight});
    search_->SetBounds({in.l + kPad, y, sort_->Bounds().l - 4, y + pui::TextBox::kHeight});
    y += kSearchRowH;
    dividerY_ = pui::SnapDip(y - 1, ctx_.Scale());
    const float footerTop = std::max(y + kRowH, in.b - kFooterH);
    list_->SetBounds({in.l + 6, y + 4, in.r - 6, footerTop});
    footerRect_ = {in.l, pui::SnapDip(footerTop, ctx_.Scale()), in.r, in.b};
    summaryRect_ = {in.l + kPad, footerTop + 1 + kFooterPad, in.r - kPad, footerTop + 1 + kFooterPad + kSummaryRowH};

}

void PlaylistPanel::Paint(pui::IRenderer& r, const pui::Theme& t) {
    const RectF b = Bounds();
    if (overlay_) r.FillRoundRect(b, t.overlayRadius, t.flyoutSurface);   // popup over the video: opaque base
    r.FillRoundRect(b, t.overlayRadius, t.panelFill);
    const float px = Px();
    r.StrokeRoundRect(b.Deflate(px / 2, px / 2, px / 2, px / 2), t.overlayRadius, t.panelStroke, px);
    r.FillRect({b.l + px, dividerY_, b.r - px, dividerY_ + px}, t.panelStroke);
    r.FillRect({b.l + px, footerRect_.t, b.r - px, footerRect_.t + px}, t.panelStroke);
    pui::TextStyle ts;
    ts.size = 12;
    ts.align = pui::TextAlign::Trailing;
    r.Text(summary_, summaryRect_, ts, t.textTertiary);
}

void PlaylistPanel::PaintRow(pui::IRenderer& r, const pui::Theme& t, const pui::RowPaintArgs& a) {
    const auto& items = Items();
    if (a.row < 0 || a.row >= (int)items.size()) return;
    const wvp::MediaItem& item = items[(size_t)a.row];
    const plp::RowStyle style = plp::RowStyleFor(a.current, played_.count(item.url) != 0);
    const bool played = style == plp::RowStyle::Played;
    const RectF box = a.rect.Deflate(0, kRowGap / 2, 0, kRowGap / 2);   // 40 DIP row + 2 DIP gap
    if (a.pressed) r.FillRoundRect(box, t.controlRadius, t.subtleTertiary);
    else if (a.selected) r.FillRoundRect(box, t.controlRadius, WithAlpha(t.accent, a.current ? 0x38 : 0x2A));
    else if (a.current) r.FillRoundRect(box, t.controlRadius, t.ctlSecondary);
    else if (a.hover) r.FillRoundRect(box, t.controlRadius, t.subtleSecondary);
    if (a.current) r.FillRoundRect({box.l, box.cy() - 8, box.l + 3, box.cy() + 8}, 1.5f, t.accent);

    float x = box.l + kRowPadX;
    pui::TextStyle idx;
    idx.size = 12;
    idx.ellipsis = false;
    if (a.current) {
        pui::TextStyle gs;
        gs.font = pui::FontKind::Icon; gs.size = 12; gs.ellipsis = false;
        r.Text(G::Str(G::Play), {x, box.t, x + kIndexW, box.b}, gs, t.accent);
    } else {
        r.Text(std::to_wstring(a.row + 1), {x, box.t, x + kIndexW, box.b}, idx, played ? t.textDisabled : t.textTertiary);
    }
    x += kIndexW + kRowPadX;

    pui::TextStyle dur;
    dur.size = 12;
    dur.align = pui::TextAlign::Trailing;
    dur.ellipsis = false;
    std::wstring durText = item.duration && *item.duration > 0 ? FormatTime(*item.duration) : std::wstring();
    float durW = durText.empty() ? 0.f : std::ceil(r.MeasureText(durText, dur).w);
    float right = box.r - kRowPadX;
    if (!durText.empty()) {
        r.Text(durText, {right - durW, box.t, right, box.b}, dur, played ? t.textDisabled : t.textSecondary);
        right -= durW + kRowPadX;
    }
    pui::TextStyle title;
    title.size = 14;
    title.weight = a.current ? 600 : 400;
    title.ellipsis = true;
    const std::wstring titleText = item.DisplayTitle();
    const RectF titleRect{x, box.t, std::max(x, right), box.b};
    const pui::Color titleColor = a.current ? t.accent : played ? t.textTertiary : t.textPrimary;
    float scroll = -1.f;
    if (a.hover && !a.dragging) {
        pui::TextStyle plain = title;
        plain.ellipsis = false;
        const float textW = std::ceil(r.MeasureText(titleText, plain).w);
        if (textW - titleRect.w() > 0.5f) {
            a.animating = true;   // keep ticking while the title overflows
            scroll = plp::MarqueeOffset(a.hoverMs, textW, titleRect.w());
            if (scroll >= 0) {
                const float px = Px();
                scroll = std::round(scroll / px) * px;   // whole device pixels: no shimmer
                r.PushClip(titleRect);
                r.Text(titleText, {titleRect.l - scroll, box.t, titleRect.l - scroll + textW + 2 * px, box.b}, plain, titleColor);
                r.PopClip();
            }
        }
    }
    if (scroll < 0) r.Text(titleText, titleRect, title, titleColor);
    if (a.focused) r.StrokeRoundRect(box.Deflate(1, 1, 1, 1), t.controlRadius, t.focusStrokeOuter, 1.5f);
}

}
