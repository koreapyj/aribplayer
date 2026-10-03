#include "pui/VirtualList.h"

namespace pui {


namespace vl {

std::vector<ColumnSpan> LayoutColumns(const std::vector<ListColumn>& cols, float totalWidth, float gap) {
    std::vector<ColumnSpan> out(cols.size());
    if (cols.empty()) return out;
    float fixed = 0, weights = 0;
    for (auto& c : cols) {
        if (c.width > 0) fixed += c.width;
        else weights += c.fraction > 0 ? c.fraction : 1.f;
    }
    float remaining = std::max(0.f, totalWidth - fixed - gap * (float)(cols.size() - 1));
    float x = 0;
    for (size_t i = 0; i < cols.size(); i++) {
        float w = cols[i].width > 0 ? cols[i].width : remaining * (cols[i].fraction > 0 ? cols[i].fraction : 1.f) / weights;
        w = std::max(w, cols[i].minWidth);
        out[i] = {x, w};
        x += w + gap;
    }
    return out;
}

int InsertionIndexForY(float contentY, float rowHeight, int count) {
    if (count <= 0 || rowHeight <= 0) return 0;
    int idx = (int)std::floor((contentY + rowHeight * 0.5f) / rowHeight);
    return std::clamp(idx, 0, count);
}

float AutoScrollVelocity(float y, float top, float bottom, float zone, float maxSpeed) {
    zone = std::min(zone, std::max(0.f, (bottom - top) * 0.5f));
    if (zone <= 0) return 0;
    if (y < top + zone) return -maxSpeed * std::clamp((top + zone - y) / zone, 0.f, 1.f);
    if (y > bottom - zone) return maxSpeed * std::clamp((y - (bottom - zone)) / zone, 0.f, 1.f);
    return 0;
}

int PageTarget(int focused, bool down, int firstFull, int lastFull, int pageRows, int count) {
    if (count <= 0) return -1;
    lastFull = std::clamp(lastFull, 0, count - 1);
    firstFull = std::clamp(firstFull, 0, count - 1);
    pageRows = std::max(1, pageRows);
    if (focused < 0) return down ? lastFull : 0;
    if (down) return focused < lastFull ? lastFull : std::min(count - 1, focused + pageRows);
    return focused > firstFull ? firstFull : std::max(0, focused - pageRows);
}

static std::vector<int> Sanitize(int count, std::vector<int> rows) {
    rows.erase(std::remove_if(rows.begin(), rows.end(), [&](int r) { return r < 0 || (count >= 0 && r >= count); }), rows.end());
    std::sort(rows.begin(), rows.end());
    rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
    return rows;
}

std::vector<int> ReorderOrder(int count, const std::vector<int>& rowsIn, int insertBefore) {
    std::vector<int> rows = Sanitize(count, rowsIn);
    insertBefore = std::clamp(insertBefore, 0, count);
    int nBefore = (int)(std::lower_bound(rows.begin(), rows.end(), insertBefore) - rows.begin());
    std::vector<int> rest;
    for (int i = 0; i < count; i++)
        if (!std::binary_search(rows.begin(), rows.end(), i)) rest.push_back(i);
    int pos = std::clamp(insertBefore - nBefore, 0, (int)rest.size());
    std::vector<int> out(rest.begin(), rest.begin() + pos);
    out.insert(out.end(), rows.begin(), rows.end());
    out.insert(out.end(), rest.begin() + pos, rest.end());
    return out;
}

std::vector<int> MovedRowsNewIndices(const std::vector<int>& rowsIn, int insertBefore) {
    std::vector<int> rows = Sanitize(-1, rowsIn);
    insertBefore = std::max(0, insertBefore);
    int nBefore = (int)(std::lower_bound(rows.begin(), rows.end(), insertBefore) - rows.begin());
    std::vector<int> out;
    for (int k = 0; k < (int)rows.size(); k++) out.push_back(insertBefore - nBefore + k);
    return out;
}

bool IsNoOpReorder(int count, const std::vector<int>& rows, int insertBefore) {
    auto order = ReorderOrder(count, rows, insertBefore);
    for (int i = 0; i < (int)order.size(); i++)
        if (order[(size_t)i] != i) return false;
    return true;
}

void RealizedRange(float offset, float viewH, float rowH, int count, int overscan, int& first, int& last) {
    if (count <= 0 || viewH <= 0 || rowH <= 0) { first = 0; last = -1; return; }
    first = std::max(0, (int)std::floor(offset / rowH) - overscan);
    last = std::min(count - 1, (int)std::ceil((offset + viewH) / rowH) - 1 + overscan);
}

}


VirtualListRow::VirtualListRow(VirtualList* list) : Widget(Role::ListItem), list_(list) {}

bool VirtualListRow::IsSelected() const { return list_->IsRowSelected(row_); }
void VirtualListRow::SelectItem() { list_->RowSelectItem(row_); }
void VirtualListRow::AddToSelection() { list_->RowAddToSelection(row_); }
void VirtualListRow::RemoveFromSelection() { list_->RowRemoveFromSelection(row_); }
void VirtualListRow::Invoke() { list_->RowInvoke(row_); }
Widget* VirtualListRow::SelectionContainer() const { return list_; }

void VirtualListRow::RestartHover() {
    if (UiHost* h = Host()) hoverStartMs_ = h->Now();
}

void VirtualListRow::OnPointerEnter() {
    RestartHover();
    animating_ = true;   // until a paint shows whether the painter animates
    if (UiHost* h = Host()) h->StartAnimating(this);
    Invalidate();
}

void VirtualListRow::OnPointerLeave() {
    animating_ = false;
    Invalidate();
}

bool VirtualListRow::OnTick(uint64_t) {
    if (!IsHovered() || !IsEffectivelyVisible()) return false;
    if (animating_) Invalidate();
    return animating_;
}

void VirtualListRow::Paint(IRenderer& r, const Theme& t) {
    RectF rr = Bounds();
    RectF vp = list_->Viewport();
    if (row_ < 0 || !rr.Intersects(vp)) return;
    const IListSource* src = list_->SourceForPaint();
    int srow = list_->SourceRowOf(row_);
    r.PushClip(vp);
    bool dragged = list_->IsRowDragged(row_);
    if (dragged) r.PushOpacity(0.5f);
    if (list_->painter_) {
        RowPaintArgs a;
        a.rect = rr;
        a.row = srow;
        a.viewRow = row_;
        a.hover = IsHovered() && !list_->IsDragging();
        a.pressed = list_->IsRowPressed(row_);
        a.selected = list_->IsRowSelected(row_);
        a.focused = row_ == list_->FocusedRow() && list_->ShowFocusVisual();
        a.current = src && src->IsCurrent(srow);
        a.dragging = dragged;
        if (a.hover) {
            UiHost* h = Host();
            uint64_t now = h ? h->Now() : 0;
            a.hoverMs = now > hoverStartMs_ ? now - hoverStartMs_ : 0;
        }
        list_->painter_(r, t, a);
        animating_ = a.hover && a.animating;
        if (dragged) r.PopOpacity();
        r.PopClip();
        return;
    }
    RectF box = rr.Deflate(4, 2, 4, 2);
    bool sel = list_->IsRowSelected(row_);
    if (sel) r.FillRoundRect(box, 4, t.subtleSecondary);
    if (IsHovered() && !list_->IsDragging()) r.FillRoundRect(box, 4, t.subtleSecondary);
    if (list_->IsRowPressed(row_)) r.FillRoundRect(box, 4, t.subtleTertiary);
    if (sel) r.FillRoundRect({box.l, box.cy() - 8, box.l + 3, box.cy() + 8}, 1.5f, t.accent);
    if (row_ == list_->FocusedRow()) list_->PaintFocusVisual(r, t, box, 4);
    if (src) {
        bool cur = src->IsCurrent(srow);
        float baseX = rr.l + VirtualList::kPadX;
        if (list_->ShowGlyphs()) {
            std::wstring g = src->Glyph(srow);
            if (!g.empty()) {
                TextStyle gs;
                gs.font = FontKind::Icon;
                gs.size = 16;
                gs.align = TextAlign::Center;
                gs.ellipsis = false;
                r.Text(g, {baseX - 4, box.t, baseX + 20, box.b}, gs, cur ? t.accent : t.textSecondary);
            }
            baseX += VirtualList::kGlyphW;
        }
        for (size_t c = 0; c < list_->spans_.size(); c++) {
            const ColumnSpan& sp = list_->spans_[c];
            RectF cell{baseX + sp.x, box.t, baseX + sp.x + sp.w, box.b};
            TextStyle st;
            st.size = t.fontBody;
            st.weight = (c == 0 && cur) ? 600 : 400;
            bool right = c < list_->columns_.size() && list_->columns_[c].rightAlign;
            st.align = right ? TextAlign::Trailing : TextAlign::Leading;
            Color col = c == 0 ? (cur ? t.accent : t.textPrimary) : t.textSecondary;
            r.Text(src->Text(srow, (int)c), cell, st, col);
        }
    }
    if (dragged) r.PopOpacity();
    r.PopClip();
}


VirtualList::VirtualList() : Widget(Role::List) {
    SetFocusable(true);
    SetClipChildren(true);
    overlay_ = Add<OverlayWidget>();
    overlay_->onPaint = [this](IRenderer& r, const Theme& t) { PaintOverlay(r, t); };
}

uint64_t VirtualList::Now() const {
    UiHost* h = Host();
    return h ? h->Now() : 0;
}

void VirtualList::Animate() {
    if (UiHost* h = Host()) h->StartAnimating(this);
}

RectF VirtualList::Viewport() const {
    RectF b = Bounds();
    return {b.l, b.t + HeaderHeight(), b.r, std::max(b.t + HeaderHeight(), b.b)};
}

RectF VirtualList::RowRect(int row) const {
    if (row < 0 || row >= count_) return {};
    RectF vp = Viewport();
    float y = vp.t + (float)row * rowH_ - offset_;
    return {vp.l, y, vp.r, y + rowH_};
}

int VirtualList::RowAt(PointF pt) const {
    RectF vp = Viewport();
    if (!vp.Contains(pt) || rowH_ <= 0) return -1;
    int idx = (int)std::floor((pt.y - vp.t + offset_) / rowH_);
    return (idx < 0 || idx >= count_) ? -1 : idx;
}

int VirtualList::FirstVisibleRow() const {
    if (count_ <= 0) return -1;
    return std::clamp((int)std::floor(offset_ / rowH_), 0, count_ - 1);
}

int VirtualList::LastVisibleRow() const {
    if (count_ <= 0) return -1;
    int l = (int)std::ceil((offset_ + Viewport().h()) / rowH_) - 1;
    return std::clamp(l, FirstVisibleRow(), count_ - 1);
}

Widget* VirtualList::RowWidget(int row) const {
    for (auto* r : rows_)
        if (r->row_ == row) return r;
    return nullptr;
}


void VirtualList::SetSource(IListSource* src) {
    source_ = src;
    offset_ = 0;
    selection_.clear();
    hiddenSel_.clear();
    anchor_ = focusedRow_ = -1;
    SourceChanged();
}

void VirtualList::SetColumns(std::vector<ListColumn> cols) {
    columns_ = std::move(cols);
    RebuildColumns();
    Invalidate();
}

void VirtualList::SetRowHeight(float h) {
    rowH_ = std::max(1.f, h);
    OnLayout();
    Invalidate();
}

void VirtualList::SetSelectionMode(SelectionMode m) {
    mode_ = m;
    if (m == SelectionMode::Single && selection_.size() > 1) CommitSelection({*selection_.begin()}, false);
}

void VirtualList::SetHeaderText(std::wstring t) {
    headerText_ = std::move(t);
    OnLayout();
    Invalidate();
}

void VirtualList::SetShowGlyphs(bool s) {
    showGlyphs_ = s;
    RebuildColumns();
    Invalidate();
}

void VirtualList::RebuildColumns() {
    float total = std::max(0.f, Bounds().w() - 2 * kPadX - (showGlyphs_ ? kGlyphW : 0.f));
    if (columns_.empty()) spans_ = vl::LayoutColumns({ListColumn{}}, total, kColGap);
    else spans_ = vl::LayoutColumns(columns_, total, kColGap);
}


void VirtualList::RebuildView() {
    int n = source_ ? std::max(0, source_->Count()) : 0;
    if (!filter_) { view_.clear(); count_ = n; return; }
    view_.clear();
    for (int i = 0; i < n; i++)
        if (filter_(i)) view_.push_back(i);
    count_ = (int)view_.size();
}

int VirtualList::ViewRowOf(int sourceRow) const {
    if (!filter_) return sourceRow >= 0 && sourceRow < count_ ? sourceRow : -1;
    auto it = std::lower_bound(view_.begin(), view_.end(), sourceRow);
    return it != view_.end() && *it == sourceRow ? (int)(it - view_.begin()) : -1;
}

void VirtualList::SetFilter(std::function<bool(int)> keep) {
    if (!keep && !filter_) return;
    std::vector<int> sel = Selection();   // visible + hidden: the filter never changes what is selected
    int focusSrc = SourceRowOf(focusedRow_), anchorSrc = SourceRowOf(anchor_);
    filter_ = std::move(keep);
    if (filter_ && dragging_) CancelDrag();
    RebuildView();
    std::set<int> ns;
    hiddenSel_.clear();
    for (int s : sel) {
        if (int v = ViewRowOf(s); v >= 0) ns.insert(v);
        else hiddenSel_.insert(s);
    }
    CommitSelection(std::move(ns), false);
    focusedRow_ = ViewRowOf(focusSrc);
    anchor_ = ViewRowOf(anchorSrc);
    offset_ = 0;
    Realize();
    Invalidate();
}

void VirtualList::ClampState() {
    RebuildView();
    offset_ = std::clamp(offset_, 0.f, MaxOffset());
    std::set<int> keep;
    for (int r : selection_)
        if (r < count_) keep.insert(r);
    CommitSelection(std::move(keep), false);
    const int srcCount = source_ ? std::max(0, source_->Count()) : 0;
    for (auto it = hiddenSel_.begin(); it != hiddenSel_.end();) it = *it >= srcCount ? hiddenSel_.erase(it) : std::next(it);
    if (count_ == 0) anchor_ = focusedRow_ = -1;
    else {
        if (anchor_ >= count_) anchor_ = count_ - 1;
        if (focusedRow_ >= count_) focusedRow_ = count_ - 1;
    }
}

void VirtualList::SourceChanged() {
    ClampState();
    for (auto* r : rows_) r->RestartHover();
    Realize();
    Invalidate();
}

void VirtualList::OnLayout() {
    RebuildColumns();
    RebuildView();
    offset_ = std::clamp(offset_, 0.f, MaxOffset());
    Realize();
    if (overlay_) overlay_->SetBounds(Bounds());
}

void VirtualList::Realize() {
    RectF vp = Viewport();
    sb_.SetView(vp);
    sb_.SetMetrics(vp.h(), (float)count_ * rowH_, offset_);
    int f, l;
    vl::RealizedRange(offset_, vp.h(), rowH_, count_, kOverscan, f, l);
    std::vector<VirtualListRow*> kept;
    for (auto* r : rows_) {
        if (r->row_ >= f && r->row_ <= l) kept.push_back(r);
        else if (auto up = RemoveChild(r)) {
            r->row_ = -1;
            if (pool_.size() < 64) pool_.push_back(std::move(up));
        }
    }
    std::vector<VirtualListRow*> out;
    size_t ki = 0;
    int pos = 0;
    auto bind = [&](VirtualListRow* w, int i) {
        if (w->row_ != i) w->RestartHover();
        w->row_ = i;
        w->SetName(source_ ? source_->Text(SourceRowOf(i), 0) : std::wstring());
        w->SetBounds(RowRect(i));
    };
    for (int i = f; i <= l; i++, pos++) {
        if (ki < kept.size() && kept[ki]->row_ == i) {
            bind(kept[ki], i);
            out.push_back(kept[ki++]);
            continue;
        }
        std::unique_ptr<Widget> up;
        if (!pool_.empty()) { up = std::move(pool_.back()); pool_.pop_back(); }
        else up = std::make_unique<VirtualListRow>(this);
        auto* row = static_cast<VirtualListRow*>(up.get());
        bind(row, i);
        AddChild(std::move(up), pos);
        out.push_back(row);
    }
    rows_ = std::move(out);
}

void VirtualList::SetOffset(float o) {
    o = std::clamp(o, 0.f, MaxOffset());
    if (o == offset_) return;
    offset_ = o;
    for (auto* r : rows_) r->RestartHover();   // hover marquees start over after a scroll
    Realize();
    if (UiHost* h = Host()) { sb_.Poke(h->Now()); h->StartAnimating(this); }
    if (dragging_) UpdateInsertIndex();
    Invalidate();
}

void VirtualList::ScrollIntoViewRow(int row) {
    if (row < 0 || row >= count_) return;
    float top = (float)row * rowH_, vh = Viewport().h();
    if (top < offset_) SetOffset(top);
    else if (top + rowH_ > offset_ + vh) SetOffset(top + rowH_ - vh);
}

void VirtualList::ScrollIntoView(Widget* child) {
    if (auto* row = dynamic_cast<VirtualListRow*>(child)) ScrollIntoViewRow(row->Row());
}

double VirtualList::VerticalScrollPercent() const {
    float mo = MaxOffset();
    return mo > 0 ? (double)offset_ / mo * 100.0 : -1.0;
}

double VirtualList::VerticalViewSize() const {
    float extent = (float)count_ * rowH_;
    return extent > 0 ? std::min(100.0, (double)Viewport().h() / extent * 100.0) : 100.0;
}

void VirtualList::SetVerticalScrollPercent(double pct) {
    SetOffset((float)(std::clamp(pct, 0.0, 100.0) / 100.0 * MaxOffset()));
}


std::vector<int> VirtualList::Selection() const {
    std::vector<int> out;
    for (int v : selection_) out.push_back(SourceRowOf(v));
    out.insert(out.end(), hiddenSel_.begin(), hiddenSel_.end());   // selected rows the filter hides
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<Widget*> VirtualList::SelectedChildren() const {
    std::vector<Widget*> out;
    for (auto* r : rows_)
        if (IsRowSelected(r->row_)) out.push_back(r);
    return out;
}

bool VirtualList::CommitSelection(std::set<int> s, bool notify) {
    if (notify) hiddenSel_.clear();   // a user selection replaces the rows kept while filtered out
    if (s == selection_) return false;
    std::set<int> old = std::move(selection_);
    selection_ = std::move(s);
    for (auto* r : rows_)
        if ((old.count(r->row_) != 0) != (selection_.count(r->row_) != 0)) r->Raise(UiEvent::SelectionChanged);
    Raise(UiEvent::SelectionChanged);
    Invalidate();
    if (notify && onSelectionChanged) onSelectionChanged();
    return true;
}

void VirtualList::SetSelection(std::vector<int> rows) {
    hiddenSel_.clear();
    if (filter_) {
        for (int& r : rows) {
            const int v = ViewRowOf(r);
            if (v < 0 && source_ && r >= 0 && r < source_->Count()) hiddenSel_.insert(r);
            r = v;
        }
    }
    std::set<int> s;
    for (int r : rows)
        if (r >= 0 && r < count_) s.insert(r);
    if (mode_ == SelectionMode::Single && s.size() > 1) {
        int first = -1;
        for (int r : rows)
            if (r >= 0 && r < count_) { first = r; break; }
        s = {first};
    }
    CommitSelection(s, false);
    if (!s.empty()) {
        anchor_ = *s.begin();
        if (focusedRow_ < 0) focusedRow_ = anchor_;
    }
}

void VirtualList::SetFocusedRow(int row) {
    focusedRow_ = (row >= 0 && row < count_) ? row : -1;
    ScrollIntoViewRow(focusedRow_);
    Invalidate();
}

std::set<int> VirtualList::RangeSet(int a, int b) const {
    std::set<int> s;
    int lo = std::max(0, std::min(a, b)), hi = std::min(count_ - 1, std::max(a, b));
    for (int i = lo; i <= hi; i++) s.insert(i);
    return s;
}

void VirtualList::RowSelectItem(int row) {
    if (row < 0 || row >= count_) return;
    anchor_ = focusedRow_ = row;
    CommitSelection({row}, true);
}

void VirtualList::RowAddToSelection(int row) {
    if (row < 0 || row >= count_) return;
    if (mode_ == SelectionMode::Single) { RowSelectItem(row); return; }
    std::set<int> s = selection_;
    s.insert(row);
    CommitSelection(std::move(s), true);
}

void VirtualList::RowRemoveFromSelection(int row) {
    std::set<int> s = selection_;
    s.erase(row);
    CommitSelection(std::move(s), true);
}

void VirtualList::RowInvoke(int row) {
    if (row < 0 || row >= count_) return;
    focusedRow_ = row;
    Raise(UiEvent::Invoked);
    if (onInvoke) onInvoke(SourceRowOf(row));
}

void VirtualList::ApplyClick(int row, unsigned mods) {
    bool ctrl = (mods & ModCtrl) != 0, shift = (mods & ModShift) != 0;
    if (mode_ == SelectionMode::Single) {
        anchor_ = focusedRow_ = row;
        CommitSelection({row}, true);
    } else if (shift) {
        if (anchor_ < 0) anchor_ = focusedRow_ >= 0 ? focusedRow_ : row;
        std::set<int> s = ctrl ? selection_ : std::set<int>();
        for (int i : RangeSet(anchor_, row)) s.insert(i);
        focusedRow_ = row;
        CommitSelection(std::move(s), true);
    } else if (ctrl) {
        std::set<int> s = selection_;
        if (!s.erase(row)) s.insert(row);
        anchor_ = focusedRow_ = row;
        CommitSelection(std::move(s), true);
    } else if (selection_.count(row) && selection_.size() > 1) {
        pendingCollapse_ = row;   // collapse to this row on release unless a drag starts
        focusedRow_ = row;
        anchor_ = row;
    } else {
        anchor_ = focusedRow_ = row;
        CommitSelection({row}, true);
    }
    Invalidate();
}

void VirtualList::MoveFocusTo(int target, unsigned mods) {
    if (count_ <= 0) return;
    target = std::clamp(target, 0, count_ - 1);
    bool ctrl = (mods & ModCtrl) != 0, shift = (mods & ModShift) != 0;
    bool ext = mode_ == SelectionMode::Extended;
    if (ext && ctrl && !shift) {
        focusedRow_ = target;
    } else if (ext && shift) {
        if (anchor_ < 0) anchor_ = focusedRow_ >= 0 ? focusedRow_ : target;
        focusedRow_ = target;
        CommitSelection(RangeSet(anchor_, target), true);
    } else {
        anchor_ = focusedRow_ = target;
        CommitSelection({target}, true);
    }
    ScrollIntoViewRow(target);
    Invalidate();
}

void VirtualList::OnFocusChanged(bool focused) {
    if (focused && focusedRow_ < 0 && count_ > 0)
        focusedRow_ = selection_.empty() ? FirstVisibleRow() : *selection_.begin();
    Invalidate();
}


void VirtualList::FinishReorder(std::vector<int> rows, int insertBefore) {
    if (!onReorder) return;
    int oldFocus = focusedRow_;
    onReorder(rows, insertBefore);
    std::vector<int> nr = vl::MovedRowsNewIndices(rows, insertBefore);
    int fi = 0;
    for (size_t i = 0; i < rows.size(); i++)
        if (rows[i] == oldFocus) fi = (int)i;
    std::set<int> s;
    for (int r : nr)
        if (r < count_) s.insert(r);
    CommitSelection(std::move(s), true);
    if (!nr.empty()) {
        anchor_ = nr.front();
        focusedRow_ = std::min(nr[(size_t)fi], count_ - 1);
        ScrollIntoViewRow(focusedRow_);
    }
    Invalidate();
}

void VirtualList::KeyboardReorder(bool up) {
    std::vector<int> rows = ViewSelection();
    if (rows.empty() && focusedRow_ >= 0) rows.push_back(focusedRow_);
    if (rows.empty()) return;
    if (up) {
        if (rows.front() == 0) return;
        FinishReorder(rows, rows.front() - 1);
    } else {
        if (rows.back() >= count_ - 1) return;
        FinishReorder(rows, std::min(count_, rows.back() + 2));
    }
}

void VirtualList::UpdateInsertIndex() {
    RectF vp = Viewport();
    insertIndex_ = vl::InsertionIndexForY(dragPt_.y - vp.t + offset_, rowH_, count_);
    Invalidate();
}

void VirtualList::CancelDrag() {
    dragging_ = false;
    dragRows_.clear();
    pendingCollapse_ = -1;
    Invalidate();
}


Widget* VirtualList::HitTest(PointF p) {
    if (!Visible() || !Bounds().Contains(p)) return nullptr;
    if (sb_.HitPart(p) != ScrollBarController::Part::None || !Viewport().Contains(p)) return this;
    return Widget::HitTest(p);
}

bool VirtualList::OnPointerDown(const PointerEvent& e) {
    if (e.button == PointerButton::Right) {
        int row = RowAt(e.pt);
        if (row >= 0) {
            if (!IsRowSelected(row)) CommitSelection({row}, true), anchor_ = row;
            focusedRow_ = row;
        }
        return false;   // the host then asks OnContextMenu
    }
    if (e.button != PointerButton::Left) return false;
    float no = offset_;
    if (sb_.PointerDown(e.pt, Now(), 24.f, no)) {
        sbCapture_ = true;
        pressRow_ = -1;
        SetOffset(no);
        Animate();
        return true;
    }
    int row = RowAt(e.pt);
    pressPt_ = dragPt_ = e.pt;
    pressRow_ = row;
    pendingCollapse_ = -1;
    dragging_ = false;
    if (row < 0) return true;
    if (e.clickCount >= 2) {
        pressRow_ = -1;
        RowInvoke(row);
        return true;
    }
    ApplyClick(row, e.mods);
    return true;
}

bool VirtualList::OnPointerMove(const PointerEvent& e) {
    float no = offset_;
    if (sbCapture_) {
        if (sb_.PointerMove(e.pt, Now(), no)) SetOffset(no);
        return true;
    }
    if (pressRow_ < 0 && !dragging_) {
        bool hov = sb_.Hover();
        sb_.PointerMove(e.pt, Now(), no);
        if (hov != sb_.Hover()) Animate();
        return false;
    }
    dragPt_ = e.pt;
    if (!dragging_) {
        float dx = e.pt.x - pressPt_.x, dy = e.pt.y - pressPt_.y;
        if (reorder_ && !filter_ && IsRowSelected(pressRow_) && dx * dx + dy * dy > 16.f) {
            dragging_ = true;
            dragRows_ = ViewSelection();
            pendingCollapse_ = -1;
            lastTickMs_ = Now();
            Animate();
        } else return true;
    }
    UpdateInsertIndex();
    return true;
}

bool VirtualList::OnPointerUp(const PointerEvent& e) {
    if (e.button == PointerButton::Right) return true;   // the context menu was raised on press
    if (sbCapture_) {
        sbCapture_ = false;
        sb_.PointerUp(Now());
        Animate();
        return true;
    }
    if (dragging_) {
        std::vector<int> rows = dragRows_;
        int ib = insertIndex_;
        CancelDrag();
        pressRow_ = -1;
        if (!vl::IsNoOpReorder(count_, rows, ib)) FinishReorder(std::move(rows), ib);
        return true;
    }
    if (pendingCollapse_ >= 0) {
        int row = pendingCollapse_;
        pendingCollapse_ = -1;
        if (RowAt(e.pt) == row) {
            anchor_ = focusedRow_ = row;
            CommitSelection({row}, true);
        }
    }
    pressRow_ = -1;
    Invalidate();
    return true;
}

void VirtualList::OnPointerLeave() {
    sb_.Leave(Now());
    Animate();
}

bool VirtualList::OnPointerWheel(const PointerEvent& e) {
    if ((e.mods & ModCtrl) || e.wheelDelta == 0 || MaxOffset() <= 0) return false;
    ScrollBy(-e.wheelDelta * wheelStep_);
    return true;
}

bool VirtualList::OnTick(uint64_t now) {
    bool keep = sb_.Update(now);
    if (dragging_) {
        uint64_t dt = now > lastTickMs_ ? std::min<uint64_t>(now - lastTickMs_, 100) : 0;
        lastTickMs_ = now;
        RectF vp = Viewport();
        float v = vl::AutoScrollVelocity(dragPt_.y, vp.t, vp.b);
        if (v != 0 && dt > 0) SetOffset(offset_ + v * (float)dt / 1000.f);
        UpdateInsertIndex();
        keep = true;
    }
    return keep;
}

bool VirtualList::OnKeyDown(int vk, unsigned mods) {
    bool alt = (mods & ModAlt) != 0, ctrl = (mods & ModCtrl) != 0;
    if (dragging_) {
        if (vk == key::Escape) CancelDrag();
        return true;
    }
    if (alt) {
        if ((vk == key::Up || vk == key::Down) && reorder_ && !filter_) { KeyboardReorder(vk == key::Up); return true; }
        return false;
    }
    if (count_ <= 0) return false;
    int vh = (int)Viewport().h();
    int firstFull = std::clamp((int)std::ceil(offset_ / rowH_), 0, count_ - 1);
    int lastFull = (int)std::floor((offset_ + (float)vh) / rowH_) - 1;
    int pageRows = (int)std::floor((float)vh / rowH_);
    switch (vk) {
    case key::Up: MoveFocusTo(focusedRow_ < 0 ? 0 : focusedRow_ - 1, mods); return true;
    case key::Down: MoveFocusTo(focusedRow_ < 0 ? 0 : focusedRow_ + 1, mods); return true;
    case key::Home: MoveFocusTo(0, mods); return true;
    case key::End: MoveFocusTo(count_ - 1, mods); return true;
    case key::PageUp: MoveFocusTo(vl::PageTarget(focusedRow_, false, firstFull, lastFull, pageRows, count_), mods); return true;
    case key::PageDown: MoveFocusTo(vl::PageTarget(focusedRow_, true, firstFull, lastFull, pageRows, count_), mods); return true;
    case key::A:
        if (ctrl && mode_ == SelectionMode::Extended) {
            if (anchor_ < 0) anchor_ = 0;
            CommitSelection(RangeSet(0, count_ - 1), true);
            return true;
        }
        return false;
    case key::Space:
        if (focusedRow_ < 0) return false;
        if (mode_ == SelectionMode::Extended) {
            std::set<int> s = selection_;
            if (!s.erase(focusedRow_)) s.insert(focusedRow_);
            anchor_ = focusedRow_;
            CommitSelection(std::move(s), true);
        } else {
            anchor_ = focusedRow_;
            CommitSelection({focusedRow_}, true);
        }
        return true;
    case key::Enter:
        if (focusedRow_ < 0) return false;
        RowInvoke(focusedRow_);
        return true;
    case key::Delete:
        if (selection_.empty() || !onDelete) return false;
        {   // only what is shown: rows kept selected behind a filter are never deleted blind
            std::vector<int> vis;
            for (int v : selection_) vis.push_back(SourceRowOf(v));
            onDelete(std::move(vis));
        }
        return true;
    default: return false;
    }
}

bool VirtualList::OnContextMenu(PointF pt, bool keyboard) {
    if (!onContextMenu) return false;
    int row;
    if (keyboard) {
        row = focusedRow_;
        RectF rr = RowRect(row).Intersect(Viewport());
        pt = row >= 0 ? PointF{rr.cx(), rr.cy()} : PointF{Bounds().cx(), Bounds().cy()};
    } else {
        row = RowAt(pt);
    }
    onContextMenu(row >= 0 ? SourceRowOf(row) : -1, pt, keyboard);
    return true;
}


void VirtualList::Paint(IRenderer& r, const Theme& t) {
    RectF b = Bounds();
    if (!headerText_.empty()) {
        TextStyle st;
        st.size = t.fontCaption;
        st.weight = 600;
        r.Text(headerText_, {b.l + kPadX, b.t, b.r - kPadX, b.t + kHeaderH}, st, t.textSecondary);
    }
    if (count_ == 0 && !emptyText_.empty()) {
        TextStyle st;
        st.size = t.fontBody;
        st.align = TextAlign::Center;
        st.wrap = true;
        r.Text(emptyText_, Viewport().Deflate(kPadX, 0, kPadX, 0), st, t.textSecondary);
    }
}

void VirtualList::PaintOverlay(IRenderer& r, const Theme& t) {
    RectF vp = Viewport();
    if (dragging_) {
        float y = vp.t + (float)insertIndex_ * rowH_ - offset_;
        if (y >= vp.t - 1 && y <= vp.b + 1) r.Line({vp.l + 8, y}, {vp.r - 8, y}, t.accent, 2);
        RectF g = RectF::XYWH(vp.l + 4, dragPt_.y - rowH_ * 0.5f, vp.w() - 8, rowH_);
        r.PushOpacity(0.85f);
        r.FillRoundRect(g, 4, t.flyoutSurface);
        r.StrokeRoundRect(g, 4, t.accent, 1);
        if (source_ && !dragRows_.empty()) {
            TextStyle st;
            st.size = t.fontBody;
            r.Text(source_->Text(SourceRowOf(dragRows_.front()), 0), g.Deflate(kPadX, 0, kPadX + 28, 0), st, t.textPrimary);
            if (dragRows_.size() > 1) {
                r.FillEllipse({g.r - 20, g.cy()}, 11, 11, t.accent);
                TextStyle bs;
                bs.size = t.fontCaption;
                bs.align = TextAlign::Center;
                bs.ellipsis = false;
                r.Text(std::to_wstring(dragRows_.size()), {g.r - 31, g.t, g.r - 9, g.b}, bs, t.textOnAccent);
            }
        }
        r.PopOpacity();
    }
    sb_.Paint(r, t, Now());
}

}
