#pragma once
// Only the rows intersecting the viewport (+ overscan) exist as child widgets (Role::ListItem); they are recycled.
#include <functional>
#include <set>
#include <string>
#include <vector>

#include "ScrollViewer.h"

namespace pui {

struct IListSource {
    virtual ~IListSource() = default;
    virtual int Count() const = 0;
    virtual std::wstring Text(int row, int column) const = 0;
    virtual std::wstring Glyph(int row) const { (void)row; return {}; }
    /// e.g. the now-playing item: bold + accent.
    virtual bool IsCurrent(int row) const { (void)row; return false; }
};

/// width > 0: fixed DIP width. Otherwise `fraction` is the weight (<= 0 counts as 1) of the space left after fixed columns.
struct ListColumn {
    std::wstring header;
    float width = 0;
    float fraction = 0;
    bool rightAlign = false;
    float minWidth = 0;
};

enum class SelectionMode { Single, Extended };

struct ColumnSpan { float x = 0, w = 0; };

namespace vl {
std::vector<ColumnSpan> LayoutColumns(const std::vector<ListColumn>& cols, float totalWidth, float gap);
/// Insertion boundary (0..count) nearest to a content-space y (y measured from the first row's top).
int InsertionIndexForY(float contentY, float rowHeight, int count);
/// Signed auto-scroll speed (DIP/s; negative = up) for a pointer y near the viewport edges.
float AutoScrollVelocity(float y, float top, float bottom, float zone = 32.f, float maxSpeed = 900.f);
/// Focus target for PageUp/PageDown: first moves to the edge row of the page, then a full page.
int PageTarget(int focused, bool down, int firstFull, int lastFull, int pageRows, int count);
/// Rows (sorted, unique, valid) are removed and inserted as one block before original index `insertBefore`;
/// returns the new order as a list of old indices.
std::vector<int> ReorderOrder(int count, const std::vector<int>& rows, int insertBefore);
/// New indices of the moved rows after the move described above.
std::vector<int> MovedRowsNewIndices(const std::vector<int>& rows, int insertBefore);
bool IsNoOpReorder(int count, const std::vector<int>& rows, int insertBefore);
/// Inclusive realized range; last < first when empty.
void RealizedRange(float offset, float viewH, float rowH, int count, int overscan, int& first, int& last);
}

class VirtualList;

/// Arguments for a custom row painter (VirtualList::SetRowPainter). `rect` is the full row slot (absolute DIPs).
/// `row` is the SOURCE row (use it with IListSource); `viewRow` is the position in the (possibly filtered) list.
struct RowPaintArgs {
    RectF rect{};
    int row = -1, viewRow = -1;
    bool hover = false, pressed = false, selected = false, focused = false, current = false, dragging = false;
    /// Milliseconds since the hover began (0 when not hovered). A painter that animates by it sets `animating` so the row
    /// keeps ticking/repainting; the row stops ticking when it stays false.
    uint64_t hoverMs = 0;
    mutable bool animating = false;
};

/// One realized row (child of the list). Exposed so tests/UIA can inspect it.
class VirtualListRow : public Widget {
public:
    explicit VirtualListRow(VirtualList* list);
    int Row() const { return row_; }
    void Paint(IRenderer& r, const Theme& t) override;
    void OnPointerEnter() override;
    void OnPointerLeave() override;
    bool OnTick(uint64_t nowMs) override;
    bool SupportsInvoke() const override { return true; }
    void Invoke() override;
    bool SupportsSelectionItem() const override { return true; }
    bool IsSelected() const override;
    void SelectItem() override;
    void AddToSelection() override;
    void RemoveFromSelection() override;
    Widget* SelectionContainer() const override;

private:
    friend class VirtualList;
    void RestartHover();
    VirtualList* list_;
    int row_ = -1;
    uint64_t hoverStartMs_ = 0;
    bool animating_ = false;
};

class VirtualList : public Widget {
public:
    VirtualList();

    void SetSource(IListSource* src);
    IListSource* Source() const { return source_; }
    void SetColumns(std::vector<ListColumn> cols);
    void SetRowHeight(float h);
    void SetSelectionMode(SelectionMode m);
    void SetReorderEnabled(bool e) { reorder_ = e; }
    void SetHeaderText(std::wstring t);
    void SetEmptyText(std::wstring t) { emptyText_ = std::move(t); Invalidate(); }
    void SetShowGlyphs(bool s);
    void SetWheelStep(float s) { wheelStep_ = s; }
    /// Opt-in custom row look (draw through IRenderer only). Clipped to the viewport; unset = default look.
    void SetRowPainter(std::function<void(IRenderer&, const Theme&, const RowPaintArgs&)> p) { painter_ = std::move(p); Invalidate(); }
    /// Shows only source rows for which `keep(sourceRow)` is true (nullptr clears). Selection, invoke, delete and context menu
    /// report SOURCE rows; drag-reorder is disabled while filtered. Re-evaluated on SourceChanged().
    void SetFilter(std::function<bool(int sourceRow)> keep);
    bool IsFiltered() const { return (bool)filter_; }
    int SourceRowOf(int viewRow) const { return viewRow >= 0 && viewRow < count_ ? (filter_ ? view_[(size_t)viewRow] : viewRow) : -1; }
    int ViewRowOf(int sourceRow) const;                 // -1 when hidden by the filter
    int ViewCount() const { return count_; }

    /// Count and/or row content changed: clamps selection/focus/offset, re-realizes and refreshes row names.
    void SourceChanged();

    void SetSelection(std::vector<int> rows);          // SOURCE rows; programmatic: raises UI events, does not call onSelectionChanged
    std::vector<int> Selection() const;                 // SOURCE rows, sorted ascending
    bool IsRowSelected(int row) const { return selection_.count(row) != 0; }
    int FocusedRow() const { return focusedRow_; }
    void SetFocusedRow(int row);                        // moves the focus row only (keeps it visible)

    RectF Viewport() const;                             // list bounds minus header
    RectF RowRect(int row) const;                       // absolute rect (may be outside the viewport); empty for invalid rows
    int RowAt(PointF pt) const;                         // -1 outside the viewport / past the last row
    int FirstVisibleRow() const;                        // -1 when empty
    int LastVisibleRow() const;
    int RealizedRowCount() const { return (int)rows_.size(); }
    Widget* RowWidget(int row) const;                   // realized row widget or nullptr
    float Offset() const { return offset_; }
    float MaxOffset() const { return std::max(0.f, (float)count_ * rowH_ - Viewport().h()); }
    void SetOffset(float o);
    void ScrollBy(float dy) { SetOffset(offset_ + dy); }
    void ScrollIntoViewRow(int row);
    bool IsDragging() const { return dragging_; }
    int DropIndex() const { return dragging_ ? insertIndex_ : -1; }
    const ScrollBarController& ScrollBar() const { return sb_; }

    std::function<void(int row)> onInvoke;
    std::function<void(std::vector<int> rows)> onDelete;
    std::function<void()> onSelectionChanged;           // user-driven changes only
    std::function<void(std::vector<int> rows, int insertBefore)> onReorder;   // rows ascending, original indices
    std::function<void(int row, PointF pt, bool keyboard)> onContextMenu;     // row -1 = empty area

    void OnLayout() override;
    void Paint(IRenderer& r, const Theme& t) override;
    Widget* HitTest(PointF p) override;
    bool OnPointerDown(const PointerEvent& e) override;
    bool OnPointerMove(const PointerEvent& e) override;
    bool OnPointerUp(const PointerEvent& e) override;
    bool OnPointerWheel(const PointerEvent& e) override;
    void OnPointerLeave() override;
    bool OnKeyDown(int vk, unsigned mods) override;
    bool OnContextMenu(PointF pt, bool keyboard) override;
    void OnFocusChanged(bool focused) override;
    bool OnTick(uint64_t nowMs) override;
    bool SupportsSelection() const override { return true; }
    bool CanSelectMultiple() const override { return mode_ == SelectionMode::Extended; }
    std::vector<Widget*> SelectedChildren() const override;
    bool SupportsScroll() const override { return true; }
    double VerticalScrollPercent() const override;
    double VerticalViewSize() const override;
    void SetVerticalScrollPercent(double pct) override;
    void ScrollIntoView(Widget* child) override;

    void RowSelectItem(int row);
    void RowAddToSelection(int row);
    void RowRemoveFromSelection(int row);
    void RowInvoke(int row);
    bool IsRowPressed(int row) const { return pressRow_ == row && !dragging_ && !sbCapture_; }
    bool IsRowDragged(int row) const { return dragging_ && std::find(dragRows_.begin(), dragRows_.end(), row) != dragRows_.end(); }
    const IListSource* SourceForPaint() const { return source_; }
    bool ShowGlyphs() const { return showGlyphs_; }
    static constexpr float kPadX = 16, kColGap = 12, kGlyphW = 36, kHeaderH = 32;

private:
    uint64_t Now() const;
    void Animate();
    void Realize();
    void Rebind();
    void RebuildColumns();
    void RebuildView();
    std::vector<int> ViewSelection() const { return std::vector<int>(selection_.begin(), selection_.end()); }
    void ClampState();
    bool CommitSelection(std::set<int> s, bool notify);
    std::set<int> RangeSet(int a, int b) const;
    void ApplyClick(int row, unsigned mods);
    void MoveFocusTo(int target, unsigned mods);
    void KeyboardReorder(bool up);
    void FinishReorder(std::vector<int> rows, int insertBefore);
    void UpdateInsertIndex();
    void CancelDrag();
    void PaintOverlay(IRenderer& r, const Theme& t);
    float HeaderHeight() const { return headerText_.empty() ? 0.f : kHeaderH; }

    IListSource* source_ = nullptr;
    std::vector<ListColumn> columns_;
    std::vector<ColumnSpan> spans_;
    float rowH_ = 48, offset_ = 0, wheelStep_ = 48;
    static constexpr int kOverscan = 2;
    int count_ = 0;
    SelectionMode mode_ = SelectionMode::Single;
    bool reorder_ = false, showGlyphs_ = false;
    std::function<void(IRenderer&, const Theme&, const RowPaintArgs&)> painter_;
    std::function<bool(int)> filter_;
    std::vector<int> view_;                             // view row -> source row while filtered
    std::wstring headerText_, emptyText_;

    std::vector<VirtualListRow*> rows_;                 // realized, ascending row index
    std::vector<std::unique_ptr<Widget>> pool_;         // recycled row widgets
    OverlayWidget* overlay_ = nullptr;

    std::set<int> selection_;
    std::set<int> hiddenSel_;   // SOURCE rows selected but hidden by the filter (kept, reported by Selection())
    int anchor_ = -1, focusedRow_ = -1;

    int pressRow_ = -1, pendingCollapse_ = -1;
    PointF pressPt_{}, dragPt_{};
    bool sbCapture_ = false, dragging_ = false;
    std::vector<int> dragRows_;
    int insertIndex_ = 0;
    uint64_t lastTickMs_ = 0;

    ScrollBarController sb_;
    friend class VirtualListRow;
};

}
