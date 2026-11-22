// TextBlock and OrderList (see controls.h). Threading: UI thread only.
#include "controls.h"
#include "view.h"

#include <algorithm>
#include <cmath>

namespace moonsr::app::ui {

namespace {
constexpr float kPadL = 16.0f;
constexpr float kBtn = 32.0f;
constexpr float kBtnRight = 16.0f;
constexpr float kPillH = 22.0f;
}  // namespace

// ---------------------------------------------------------------------------------------------------------------------
// TextBlock
// ---------------------------------------------------------------------------------------------------------------------

void TextBlock::SetText(const std::wstring& t) {
    if (text == t) return;
    text = t;
    if (view) view->RequestLayout();
    Invalidate();
}

float TextBlock::HeightForWidth(const Gfx& g, float width) const {
    const float th = g.MeasureHeight(text, Font::Body, width - 32.0f);
    return 16.0f + (title.empty() ? 0.0f : 28.0f) + th + 16.0f;
}

void TextBlock::Draw(Canvas& c, const Palette& p) {
    c.FillRound(bounds, metric::kCardRadius, p.cardFill);
    c.StrokeRound(bounds, metric::kCardRadius, p.cardStroke);
    float top = bounds.top + 16.0f;
    if (!title.empty()) {
        c.Text(title, Rect{bounds.left + 16.0f, top - 4.0f, bounds.right - 16.0f, top + 22.0f}, Font::BodyStrong, p.text);
        top += 28.0f;
    }
    c.Text(text, Rect{bounds.left + 16.0f, top, bounds.right - 16.0f, bounds.bottom - 8.0f}, Font::Body, p.text,
           HAlign::Left, true);
}

// ---------------------------------------------------------------------------------------------------------------------
// OrderList
// ---------------------------------------------------------------------------------------------------------------------

void OrderList::SetBadge(int id, const std::wstring& badge, bool on, const std::wstring& note) {
    for (Entry& e : entries) {
        if (e.id != id) continue;
        if (e.badge == badge && e.badgeOn == on && e.note == note) return;
        e.badge = badge;
        e.badgeOn = on;
        e.note = note;
        Invalidate();
        return;
    }
}

bool OrderList::MoveEntry(int from, int to) {
    const int n = static_cast<int>(entries.size());
    if (from < 0 || from >= n || to < 0 || to >= n || from == to) return false;
    std::vector<int> oldIds;
    for (const Entry& e : entries) oldIds.push_back(e.id);
    Entry moved = entries[static_cast<size_t>(from)];
    entries.erase(entries.begin() + from);
    entries.insert(entries.begin() + to, moved);
    anim_.clear();
    for (int i = 0; i < n; ++i) {
        const int id = entries[static_cast<size_t>(i)].id;
        int oldIdx = i;
        for (int k = 0; k < n; ++k)
            if (oldIds[static_cast<size_t>(k)] == id) oldIdx = k;
        if (oldIdx != i) anim_.emplace_back(id, static_cast<float>(oldIdx - i) * kRowH);
    }
    sel_ = to;
    if (view) view->RequestAnimation();
    Invalidate();
    if (onChange) onChange();
    return true;
}

bool OrderList::Tick(float dt) {
    if (anim_.empty()) return false;
    const float step = dt / 0.14f * kRowH;
    for (auto& a : anim_) {
        if (std::fabs(a.second) <= step) a.second = 0.0f;
        else a.second -= std::copysign(step, a.second);
    }
    anim_.erase(std::remove_if(anim_.begin(), anim_.end(), [](const auto& a) { return a.second == 0.0f; }), anim_.end());
    return !anim_.empty();
}

Rect OrderList::UpRect(int row) const {
    const float cy = bounds.top + (static_cast<float>(row) + 0.5f) * kRowH;
    const float r = bounds.right - kBtnRight - kBtn - 8.0f;
    return Rect{r - kBtn, cy - kBtn * 0.5f, r, cy + kBtn * 0.5f};
}

Rect OrderList::DownRect(int row) const {
    const float cy = bounds.top + (static_cast<float>(row) + 0.5f) * kRowH;
    return Rect{bounds.right - kBtnRight - kBtn, cy - kBtn * 0.5f, bounds.right - kBtnRight, cy + kBtn * 0.5f};
}

OrderList::Hit OrderList::HitAt(float x, float y, int* row) const {
    // x, y are local to the control; convert to page space for the rect tests.
    const float px = bounds.left + x, py = bounds.top + y;
    const int n = static_cast<int>(entries.size());
    const int r = static_cast<int>(std::floor(y / kRowH));
    if (row) *row = r;
    if (r < 0 || r >= n || x < 0 || x >= bounds.right - bounds.left) return Hit::None;
    if (r > 0 && RectHas(UpRect(r), px, py)) return Hit::Up;
    if (r < n - 1 && RectHas(DownRect(r), px, py)) return Hit::Down;
    return Hit::Row;
}

float OrderList::ComboWidth() const { return (bounds.right - bounds.left) >= 860.0f ? 210.0f : 160.0f; }

Rect OrderList::ComboRect(float rowTop) const {
    const float cy = rowTop + kRowH * 0.5f;
    const float right = bounds.right - kBtnRight - kBtn - 8.0f - kBtn - kComboGap;
    return Rect{right - ComboWidth(), cy - metric::kControlHeight * 0.5f, right, cy + metric::kControlHeight * 0.5f};
}

void OrderList::SyncCombos() {
    for (size_t i = 0; i < entries.size(); ++i)
        if (entries[i].combo) entries[i].combo->bounds = ComboRect(bounds.top + static_cast<float>(i) * kRowH);
}

int OrderList::DropIndex() const {
    const int n = static_cast<int>(entries.size());
    return std::clamp(static_cast<int>(std::floor(dragY_ / kRowH)), 0, std::max(0, n - 1));
}

void OrderList::OnHoverMove(float x, float y) {
    int row = -1;
    const Hit h = HitAt(x - bounds.left, y - bounds.top, &row);
    if (h != hoverHit_ || row != hoverRow_) {
        hoverHit_ = h;
        hoverRow_ = row;
        Invalidate();
    }
}

void OrderList::OnPress(float x, float y) {
    int row = -1;
    pressHit_ = HitAt(x - bounds.left, y - bounds.top, &row);
    pressRow_ = row;
    pressY_ = dragY_ = y - bounds.top;
    dragging_ = false;
    cancelled_ = false;
    if (row >= 0 && row < static_cast<int>(entries.size())) sel_ = row;
    Invalidate();
}

void OrderList::OnDrag(float x, float y) {
    (void)x;
    dragY_ = y - bounds.top;
    if (!cancelled_ && pressHit_ == Hit::Row && !dragging_ && std::fabs(dragY_ - pressY_) > 4.0f) dragging_ = true;
    Invalidate();
}

void OrderList::OnRelease(float x, float y, bool inside) {
    (void)inside;
    const bool wasCancelled = cancelled_;
    const bool wasDragging = dragging_;
    const Hit pressedHit = pressHit_;
    const int pressedRow = pressRow_;
    dragging_ = false;
    cancelled_ = false;
    pressHit_ = Hit::None;
    pressRow_ = -1;
    if (wasCancelled) {
        Invalidate();
        return;
    }
    if (wasDragging) {
        dragY_ = y - bounds.top;
        MoveEntry(pressedRow, DropIndex());
    } else if (pressedHit == Hit::Up || pressedHit == Hit::Down) {
        int row = -1;
        if (HitAt(x - bounds.left, y - bounds.top, &row) == pressedHit && row == pressedRow)
            MoveEntry(pressedRow, pressedHit == Hit::Up ? pressedRow - 1 : pressedRow + 1);
    }
    Invalidate();
}

bool OrderList::OnEscape() {
    if (!dragging_) return false;
    dragging_ = false;
    cancelled_ = true;
    Invalidate();
    return true;
}

bool OrderList::OnKey(UINT vk) {
    const int n = static_cast<int>(entries.size());
    int to = sel_;
    if (vk == VK_UP) to = sel_ - 1;
    else if (vk == VK_DOWN) to = sel_ + 1;
    else if (vk == VK_HOME) to = 0;
    else if (vk == VK_END) to = n - 1;
    else return false;
    if (to < 0 || to >= n || to == sel_) return false;
    sel_ = to;
    Invalidate();
    return true;
}

bool OrderList::OnAltKey(UINT vk) {
    if (vk == VK_UP) {
        MoveEntry(sel_, sel_ - 1);
        return true;
    }
    if (vk == VK_DOWN) {
        MoveEntry(sel_, sel_ + 1);
        return true;
    }
    return false;
}

void OrderList::Draw(Canvas& c, const Palette& p) {
    const Gfx& g = c.gfx();
    c.FillRound(bounds, metric::kCardRadius, p.cardFill);
    c.StrokeRound(bounds, metric::kCardRadius, p.cardStroke);
    const int n = static_cast<int>(entries.size());
    SyncCombos();
    const bool focusRow = focused;
    const int dragFrom = dragging_ ? pressRow_ : -1;

    auto pill = [&](const std::wstring& text, float rightEdge, float cy, const D2D1_COLOR_F& fill, const D2D1_COLOR_F& tc, bool outline = false) {
        const float w = g.MeasureWidth(text, Font::Caption) + 18.0f;
        const Rect r{rightEdge - w, cy - kPillH * 0.5f, rightEdge, cy + kPillH * 0.5f};
        c.FillRound(r, kPillH * 0.5f, fill);
        if (outline) c.StrokeRound(r, kPillH * 0.5f, p.ctrlStroke);
        c.Text(text, r, Font::Caption, tc, HAlign::Center);
        return r.left;
    };

    auto drawRow = [&](int i, float top, bool lifted) {
        const Entry& e = entries[static_cast<size_t>(i)];
        const Rect row{bounds.left + 1.0f, top, bounds.right - 1.0f, top + kRowH};
        const float cy = top + kRowH * 0.5f;
        if (lifted) {
            c.FillRound(Inflate(row, -3.0f), 6.0f, p.popupFill);
            c.StrokeRound(Inflate(row, -3.0f), 6.0f, p.accent, 2.0f);
        } else if (focusRow && i == sel_) {
            c.FillRound(Inflate(row, -3.0f), 6.0f, p.subtleSelected);
        } else if (hover && hoverHit_ != Hit::None && hoverRow_ == i) {
            c.FillRound(Inflate(row, -3.0f), 6.0f, p.subtleHover);
        }
        // Drag handle: two columns of three dots.
        const float hx = bounds.left + kPadL + 6.0f;
        for (int dx = -1; dx <= 1; dx += 2)
            for (int dy = -1; dy <= 1; ++dy) c.FillCircle(hx + static_cast<float>(dx) * 3.5f, cy + static_cast<float>(dy) * 7.0f, 1.6f, p.textSecondary);
        // Position number.
        const float nx = bounds.left + 62.0f;
        if (p.radius == 0.0f) c.FillRect(Rect{nx - 12.0f, cy - 12.0f, nx + 12.0f, cy + 12.0f}, p.accent);
        else c.FillCircle(nx, cy, 13.0f, p.accent);
        c.Text(std::to_wstring(i + 1), Rect{nx - 13.0f, cy - 13.0f, nx + 13.0f, cy + 13.0f}, Font::BodyStrong, p.textOnAccent,
               HAlign::Center);
        float tx = bounds.left + 92.0f;
        if (g.hasIcons() && e.icon != 0) {
            c.Icon(e.icon, Rect{tx, cy - 12.0f, tx + 24.0f, cy + 12.0f}, Font::Icon, p.text);
            tx += 24.0f + 14.0f;
        }
        // Buttons.
        const bool canUp = i > 0, canDown = i < n - 1;
        for (int b = 0; b < 2; ++b) {
            const bool up = b == 0;
            const bool can = up ? canUp : canDown;
            const Rect r = up ? UpRect(i) : DownRect(i);
            const Rect rr{r.left, cy - kBtn * 0.5f, r.right, cy + kBtn * 0.5f};
            const Hit me = up ? Hit::Up : Hit::Down;
            const bool hv = can && hover && hoverHit_ == me && hoverRow_ == i;
            const bool pr = can && pressed && pressHit_ == me && pressRow_ == i;
            const D2D1_COLOR_F fill = !can ? D2D1_COLOR_F{0, 0, 0, 0} : pr ? p.ctrlPressed : hv ? p.ctrlHover : p.ctrlFill;
            if (fill.a > 0.0f) {
                c.FillRound(rr, metric::kControlRadius, fill);
                c.StrokeRound(rr, metric::kControlRadius, p.ctrlStroke);
            }
            c.Chevron((rr.left + rr.right) * 0.5f, cy, 5.0f, can ? p.text : p.textDisabled, up);
        }
        // The stage's GPU combo box sits left of the buttons; it is drawn here so it follows the row (animation, drag).
        float edge = UpRect(i).left - 12.0f;
        if (e.combo) {
            const Rect rest = e.combo->bounds;
            e.combo->bounds = ComboRect(top);
            if (e.combo->visible) e.combo->Draw(c, p);
            e.combo->bounds = rest;
            edge = ComboRect(top).left - kComboGap;
        }
        // Badges, right to left, before the combo box.
        const float textMin = tx + 150.0f;
        if (!e.badge.empty()) {
            const float w = g.MeasureWidth(e.badge, Font::Caption) + 18.0f;
            if (edge - w > textMin) {
                edge = pill(e.badge, edge, cy, e.badgeOn ? p.success.fill : p.ctrlFill, e.badgeOn ? p.text : p.textSecondary, !e.badgeOn) - 8.0f;
            }
        }
        if (!e.note.empty()) {
            const float w = g.MeasureWidth(e.note, Font::Caption) + 18.0f;
            if (edge - w > textMin) edge = pill(e.note, edge, cy, p.warning.fill, p.text) - 8.0f;
        }
        const float right = edge - 4.0f;
        const float descH = std::min(34.0f, g.MeasureHeight(e.desc, Font::Caption, std::max(40.0f, right - tx)));
        const float by = cy - (20.0f + descH) * 0.5f;
        c.Text(e.name, Rect{tx, by, right, by + 20.0f}, Font::BodyStrong, p.text);
        c.Text(e.desc, Rect{tx, by + 20.0f, right, by + 20.0f + descH}, Font::Caption, p.textSecondary, HAlign::Left, true);
        if (focused && focusVisible && i == sel_ && !lifted) DrawFocus(c, p, row, 6.0f, -3.0f);
    };

    for (int i = 0; i < n; ++i) {
        if (i > 0) c.Line(bounds.left + 1.0f, bounds.top + static_cast<float>(i) * kRowH, bounds.right - 1.0f,
                          bounds.top + static_cast<float>(i) * kRowH, p.divider, 1.0f);
    }
    for (int i = 0; i < n; ++i) {
        if (i == dragFrom) {
            // Ghost of the dragged row's slot.
            c.FillRound(Rect{bounds.left + 4.0f, bounds.top + static_cast<float>(i) * kRowH + 3.0f, bounds.right - 4.0f,
                             bounds.top + static_cast<float>(i + 1) * kRowH - 3.0f},
                        6.0f, p.subtlePressed);
            continue;
        }
        float off = 0.0f;
        for (const auto& a : anim_)
            if (a.first == entries[static_cast<size_t>(i)].id) off = a.second;
        drawRow(i, bounds.top + static_cast<float>(i) * kRowH + off, false);
    }
    if (dragging_ && dragFrom >= 0 && dragFrom < n) {
        const float top = std::clamp(bounds.top + dragY_ - kRowH * 0.5f, bounds.top - 8.0f,
                                     bounds.bottom - kRowH + 8.0f);
        drawRow(dragFrom, top, true);
        // The insertion indicator is drawn last so the lifted row never hides it.
        const int to = DropIndex();
        const float lineY = bounds.top + (to > dragFrom ? static_cast<float>(to + 1) : static_cast<float>(to)) * kRowH;
        const float ly = (to == dragFrom) ? -1.0f : lineY;
        if (ly >= 0.0f) {
            c.FillRound(Rect{bounds.left + 8.0f, ly - 1.5f, bounds.right - 8.0f, ly + 1.5f}, 1.5f, p.accent);
            c.FillCircle(bounds.left + 8.0f, ly, 4.0f, p.accent);
            c.FillCircle(bounds.right - 8.0f, ly, 4.0f, p.accent);
        }
    }
}

}  // namespace moonsr::app::ui
