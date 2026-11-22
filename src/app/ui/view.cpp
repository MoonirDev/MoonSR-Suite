#include "view.h"

#include "core/log.h"

#include <algorithm>
#include <cmath>

namespace moonsr::app::ui {

namespace {
constexpr float kNavTop = 70.0f;
constexpr float kNavItemH = 40.0f;
constexpr float kNavItemGap = 2.0f;
constexpr float kScrollbarZone = 16.0f;
}  // namespace

View::View(Gfx& gfx) : gfx_(gfx), canvas_(gfx), popup_(gfx) {
    pal_ = MakePalette(SystemUsesDarkApps());
    popup_.onClosed = [this](int idx) {
        ComboBox* c = popupCombo_;
        popupCombo_ = nullptr;
        if (c) {
            c->SetOpen(false);
            if (idx >= 0) c->SetSelected(idx, true);
        }
        Invalidate();
    };
}

View::~View() {
    popup_.onClosed = nullptr;
    if (hwnd_ && animating_) KillTimer(hwnd_, kAnimTimerId);
}

void View::Attach(HWND hwnd, UINT dpi) {
    hwnd_ = hwnd;
    dpi_ = dpi;
}

Page* View::AddPage(const std::wstring& title, wchar_t navIcon, const std::wstring& navLabel) {
    pages_.push_back(std::make_unique<Page>(*this, title));
    auto item = std::make_unique<NavItem>();
    item->view = this;
    item->icon = navIcon;
    item->label = navLabel;
    item->index = static_cast<int>(nav_.size());
    item->selected = nav_.empty();
    nav_.push_back(std::move(item));
    layoutDirty_ = true;
    return pages_.back().get();
}

void View::SetTheme(bool dark) {
    pal_ = MakePalette(dark);
    popup_.SetPalette(pal_);
    Invalidate();
}

void View::SetDpi(UINT dpi) {
    if (dpi == dpi_) return;
    CloseDropdown();
    dpi_ = dpi;
    layoutDirty_ = true;
    Invalidate();
}

void View::SetMica(bool on) {
    mica_ = on;
    Invalidate();
}

void View::SetNavStatus(const std::wstring& text, bool active) {
    if (navStatus_ == text && navActive_ == active) return;
    navStatus_ = text;
    navActive_ = active;
    Invalidate();
}

void View::Invalidate() {
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void View::RequestLayout() {
    layoutDirty_ = true;
    Invalidate();
}

void View::RequestAnimation() {
    if (!hwnd_) return;
    lastTick_ = GetTickCount64();
    if (!animating_) {
        animating_ = true;
        SetTimer(hwnd_, kAnimTimerId, 16, nullptr);
    }
    Invalidate();
}

void View::OnAnimTimer() {
    const ULONGLONG now = GetTickCount64();
    const float dt = std::min(0.1f, static_cast<float>(now - lastTick_) / 1000.0f);
    lastTick_ = now;
    bool again = false;
    for (auto& p : pages_) again |= p->Tick(dt);
    for (auto& n : nav_) again |= n->Tick(dt);
    if (!again) {
        animating_ = false;
        KillTimer(hwnd_, kAnimTimerId);
    }
    Invalidate();
}

float View::ClientW() const {
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    return static_cast<float>(rc.right) / Scale();
}

float View::ClientH() const {
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    return static_cast<float>(rc.bottom) / Scale();
}

float View::MaxScroll() const { return std::max(0.0f, pageH_ - (ClientH() - metric::kHeaderHeight)); }

void View::SetScroll(float v) {
    v = std::clamp(v, 0.0f, MaxScroll());
    if (v == scroll_) return;
    scroll_ = v;
    Invalidate();
}

void View::Layout() {
    layoutDirty_ = false;
    const float w = ClientW(), h = ClientH();
    compact_ = w < metric::kNavCompactBelow;
    navW_ = compact_ ? metric::kNavCompactWidth : metric::kNavWidth;
    float y = kNavTop;
    for (auto& n : nav_) {
        n->compact = compact_;
        n->bounds = Rect{6.0f, y, navW_ - 6.0f, y + kNavItemH};
        y += kNavItemH + kNavItemGap;
    }
    const float avail = w - navW_;
    const float padX = avail < 640.0f ? metric::kPadXNarrow : metric::kPadX;
    contentLeft_ = navW_ + padX;
    contentW_ = std::max(200.0f, std::min(metric::kContentMaxWidth, avail - 2.0f * padX));
    if (cur_ >= 0 && cur_ < static_cast<int>(pages_.size())) pageH_ = pages_[static_cast<size_t>(cur_)]->Layout(gfx_, contentW_);
    scroll_ = std::clamp(scroll_, 0.0f, std::max(0.0f, pageH_ - (h - metric::kHeaderHeight)));
}

void View::OnSize() {
    CloseDropdown();
    layoutDirty_ = true;
    Invalidate();
}

// ---------------------------------------------------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------------------------------------------------

void View::OnPaint() {
    PAINTSTRUCT ps;
    BeginPaint(hwnd_, &ps);
    if (pages_.empty()) {
        EndPaint(hwnd_, &ps);
        return;
    }
    if (layoutDirty_) Layout();
    if (canvas_.Begin(hwnd_, dpi_, mica_)) {
        const float w = ClientW(), h = ClientH();
        if (mica_) {
            canvas_.Clear(D2D1_COLOR_F{0, 0, 0, 0});
            canvas_.FillRect(Rect{0, 0, w, h}, pal_.micaTint);  // brand tint over the Mica backdrop
        } else {
            canvas_.Clear(pal_.bg);
            canvas_.FillGradient(Rect{0, 0, w, h}, 0.0f, {{0.0f, pal_.bgA}, {0.5f, pal_.bgB}, {1.0f, pal_.bgC}}, 0.0f, 0.0f, w, h);
        }
        DrawNav(w, h);
        DrawHeader(w);
        canvas_.PushClip(Rect{navW_, metric::kHeaderHeight, w, h});
        canvas_.SetOffset(contentLeft_, metric::kHeaderHeight - scroll_);
        pages_[static_cast<size_t>(cur_)]->Draw(canvas_, pal_, gfx_);
        canvas_.SetOffset(0, 0);
        canvas_.PopClip();
        DrawScrollbar(w, h);
        if (dialog_) dialog_->Draw(canvas_, pal_, w, h);
        if (!canvas_.End()) Invalidate();
    }
    EndPaint(hwnd_, &ps);
}

void View::DrawNav(float, float h) {
    // Brand: the logo mark and the wordmark (vector, so it is sharp at every DPI).
    const float lx = compact_ ? (navW_ - 30.0f) * 0.5f : 16.0f;
    canvas_.LogoMark(Rect{lx, 17.0f, lx + 30.0f, 47.0f}, false, !pal_.dark);
    if (!compact_) canvas_.Wordmark(lx + 30.0f + 10.0f, 14.0f, 24.0f, pal_.text, pal_.dark ? Rgb(0xCFE9FF) : pal_.text, pal_.brandSR);
    for (auto& n : nav_) n->Draw(canvas_, pal_);
    if (!navStatus_.empty()) {
        const float y = h - 40.0f;
        const D2D1_COLOR_F dot = navActive_ ? pal_.success.icon : pal_.strong;
        if (compact_) {
            canvas_.FillCircle(navW_ * 0.5f, y + 20.0f, 4.0f, dot);
        } else {
            canvas_.FillCircle(24.0f, y + 20.0f, 4.0f, dot);
            canvas_.Text(navStatus_, Rect{40.0f, y, navW_ - 8.0f, y + 40.0f}, Font::Caption, pal_.textSecondary);
        }
    }
}

void View::DrawHeader(float w) {
    canvas_.Text(pages_[static_cast<size_t>(cur_)]->title(), Rect{contentLeft_, 14.0f, w - 16.0f, 62.0f}, Font::Title, pal_.text);
}

Rect View::ScrollThumb() const {
    const float h = ClientH(), w = ClientW();
    const float viewH = h - metric::kHeaderHeight;
    const float ms = MaxScroll();
    if (ms <= 0.0f) return Rect{0, 0, 0, 0};
    const float th = std::max(36.0f, viewH * viewH / pageH_);
    const float ty = metric::kHeaderHeight + (viewH - th) * (scroll_ / ms);
    const float tw = (scrollHover_ || scrollDrag_) ? 6.0f : 3.0f;
    return Rect{w - 4.0f - tw, ty + 2.0f, w - 4.0f, ty + th - 2.0f};
}

void View::DrawScrollbar(float, float) {
    const Rect t = ScrollThumb();
    if (RW(t) <= 0.0f) return;
    canvas_.FillRound(t, RW(t) * 0.5f, D2D1_COLOR_F{pal_.strong.r, pal_.strong.g, pal_.strong.b, pal_.strong.a * 0.8f});
}

// ---------------------------------------------------------------------------------------------------------------------
// Mouse
// ---------------------------------------------------------------------------------------------------------------------

Control* View::HitTest(float x, float y, bool* inNav) const {
    if (inNav) *inNav = false;
    if (x < navW_) {
        for (auto& n : nav_)
            if (n->visible && n->enabled && RectHas(n->bounds, x, y)) {
                if (inNav) *inNav = true;
                return n.get();
            }
        return nullptr;
    }
    if (y < metric::kHeaderHeight || cur_ >= static_cast<int>(pages_.size())) return nullptr;
    return pages_[static_cast<size_t>(cur_)]->HitTest(x - contentLeft_, y - metric::kHeaderHeight + scroll_);
}

void View::ToLocal(const Control*, bool inNav, float x, float y, float* lx, float* ly) const {
    if (inNav) {
        *lx = x;
        *ly = y;
    } else {
        *lx = x - contentLeft_;
        *ly = y - metric::kHeaderHeight + scroll_;
    }
}

void View::OnMouseMove(int px, int py) {
    const float x = static_cast<float>(px) / Scale(), y = static_cast<float>(py) / Scale();
    mouseX_ = x;
    mouseY_ = y;
    if (!trackingMouse_) {
        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd_, 0};
        TrackMouseEvent(&tme);
        trackingMouse_ = true;
    }
    if (dialog_) {
        dialog_->MouseMove(x, y);
        Invalidate();
        return;
    }
    if (scrollDrag_) {
        const float viewH = ClientH() - metric::kHeaderHeight;
        const float th = std::max(36.0f, viewH * viewH / pageH_);
        const float t = (y - dragOffset_ - metric::kHeaderHeight) / std::max(1.0f, viewH - th);
        SetScroll(t * MaxScroll());
        return;
    }
    if (pressed_) {
        float lx, ly;
        ToLocal(pressed_, pressedInNav_, x, y, &lx, &ly);
        const bool inside = RectHas(pressed_->bounds, lx, ly);
        if (pressed_->pressed != inside) {
            pressed_->pressed = inside;
            Invalidate();
        }
        pressed_->OnDrag(lx, ly);
        return;
    }
    const Rect thumb = ScrollThumb();
    const bool sh = RW(thumb) > 0.0f && x >= ClientW() - kScrollbarZone && y >= metric::kHeaderHeight;
    if (sh != scrollHover_) {
        scrollHover_ = sh;
        Invalidate();
    }
    Control* h = scrollHover_ ? nullptr : HitTest(x, y, nullptr);
    if (h != hover_) {
        if (hover_) hover_->hover = false;
        hover_ = h;
        if (hover_) hover_->hover = true;
        Invalidate();
    }
    if (hover_ && !pressedInNav_) {
        float lx, ly;
        ToLocal(hover_, false, x, y, &lx, &ly);
        hover_->OnHoverMove(lx, ly);
    }
}

void View::OnMouseLeave() {
    trackingMouse_ = false;
    mouseX_ = mouseY_ = -1;
    if (hover_ && !pressed_) {
        hover_->hover = false;
        hover_ = nullptr;
        Invalidate();
    }
    if (scrollHover_ && !scrollDrag_) {
        scrollHover_ = false;
        Invalidate();
    }
}

void View::OnMouseDown(int px, int py) {
    const float x = static_cast<float>(px) / Scale(), y = static_cast<float>(py) / Scale();
    if (dialog_) {
        dialog_->MouseDown(x, y);
        Invalidate();
        return;
    }
    if (popup_.IsOpen()) {
        CloseDropdown();
        return;
    }
    if (scrollHover_) {
        const Rect t = ScrollThumb();
        if (y >= t.top && y <= t.bottom) {
            dragOffset_ = y - t.top;
        } else {
            dragOffset_ = (t.bottom - t.top) * 0.5f;
            const float viewH = ClientH() - metric::kHeaderHeight;
            const float th = t.bottom - t.top;
            SetScroll((y - dragOffset_ - metric::kHeaderHeight) / std::max(1.0f, viewH - th) * MaxScroll());
        }
        scrollDrag_ = true;
        SetCapture(hwnd_);
        return;
    }
    bool inNav = false;
    Control* c = HitTest(x, y, &inNav);
    if (!c) return;
    if (c->focusable) FocusControl(c, false);
    pressed_ = c;
    pressedInNav_ = inNav;
    c->pressed = true;
    float lx, ly;
    ToLocal(c, inNav, x, y, &lx, &ly);
    c->OnPress(lx, ly);
    SetCapture(hwnd_);
    Invalidate();
}

void View::OnMouseUp(int px, int py) {
    const float x = static_cast<float>(px) / Scale(), y = static_cast<float>(py) / Scale();
    if (dialog_) {
        dialog_->MouseUp(x, y);
        FinishDialog();
        Invalidate();
        return;
    }
    if (scrollDrag_) {
        scrollDrag_ = false;
        ReleaseCapture();
        Invalidate();
        return;
    }
    if (!pressed_) return;
    Control* c = pressed_;
    const bool inNav = pressedInNav_;
    pressed_ = nullptr;
    c->pressed = false;
    ReleaseCapture();
    float lx, ly;
    ToLocal(c, inNav, x, y, &lx, &ly);
    const bool inside = RectHas(c->bounds, lx, ly);
    c->OnRelease(lx, ly, inside);
    Invalidate();
}

void View::OnMouseWheel(int delta, int px, int py) {
    if (dialog_) return;
    if (popup_.IsOpen()) {
        popup_.OnWheel(delta);
        return;
    }
    (void)px;
    (void)py;
    SetScroll(scroll_ - static_cast<float>(delta) / 120.0f * 56.0f);
    if (mouseX_ >= 0) OnMouseMove(static_cast<int>(mouseX_ * Scale()), static_cast<int>(mouseY_ * Scale()));
}

// ---------------------------------------------------------------------------------------------------------------------
// Focus and keyboard
// ---------------------------------------------------------------------------------------------------------------------

std::vector<Control*> View::FocusChain() const {
    std::vector<Control*> chain;
    for (auto& n : nav_) chain.push_back(n.get());
    if (cur_ < static_cast<int>(pages_.size()))
        for (Control* c : pages_[static_cast<size_t>(cur_)]->focusOrder())
            if (c->visible && c->enabled && c->focusable) chain.push_back(c);
    return chain;
}

void View::FocusControl(Control* c, bool keyboard) {
    if (focus_ == c) {
        if (c) c->focusVisible = keyboard;
        Invalidate();
        return;
    }
    if (focus_) {
        focus_->focused = false;
        focus_->focusVisible = false;
    }
    focus_ = c;
    if (c) {
        c->focused = true;
        c->focusVisible = keyboard;
        if (keyboard) ScrollIntoView(c);
    }
    Invalidate();
}

void View::MoveFocus(int dir) {
    const std::vector<Control*> chain = FocusChain();
    if (chain.empty()) return;
    int idx = -1;
    for (size_t i = 0; i < chain.size(); ++i)
        if (chain[i] == focus_) idx = static_cast<int>(i);
    const int n = static_cast<int>(chain.size());
    if (idx < 0) idx = dir > 0 ? -1 : 0;
    idx = ((idx + dir) % n + n) % n;
    FocusControl(chain[static_cast<size_t>(idx)], true);
}

void View::ScrollIntoView(const Control* c) {
    for (auto& n : nav_)
        if (n.get() == c) return;
    const float top = c->bounds.top - 16.0f, bottom = c->bounds.bottom + 16.0f;
    const float viewH = ClientH() - metric::kHeaderHeight;
    if (top < scroll_) SetScroll(top);
    else if (bottom > scroll_ + viewH) SetScroll(bottom - viewH);
}

void View::ShowDialog(ModalDialog::Spec spec) {
    CloseDropdown();
    if (pressed_) {
        pressed_->pressed = false;
        pressed_ = nullptr;
    }
    dialog_ = std::make_unique<ModalDialog>(std::move(spec));
    Invalidate();
}

void View::FinishDialog() {
    if (!dialog_ || dialog_->result() == ModalDialog::Result::None) return;
    const ModalDialog::Result r = dialog_->result();
    ModalDialog::Spec spec = dialog_->TakeSpec();
    dialog_.reset();
    Invalidate();
    // The callbacks may open another dialog.
    if (r == ModalDialog::Result::Primary) {
        if (spec.onPrimary) spec.onPrimary();
    } else if (spec.onSecondary) {
        spec.onSecondary();
    }
}

bool View::OnEscape() {
    if (dialog_) {
        dialog_->Key(VK_ESCAPE, false);
        FinishDialog();
        return true;
    }
    if (pressed_ && pressed_->OnEscape()) {
        Invalidate();
        return true;
    }
    return false;
}

bool View::OnAltKey(UINT vk) {
    if (dialog_ || !focus_ || !focus_->enabled) return false;
    if (focus_->OnAltKey(vk)) {
        focus_->focusVisible = true;
        ScrollIntoView(focus_);
        return true;
    }
    return false;
}

bool View::OnKeyDown(UINT vk) {
    if (dialog_) {
        dialog_->Key(vk, (GetKeyState(VK_SHIFT) & 0x8000) != 0);
        FinishDialog();
        Invalidate();
        return true;
    }
    if (popup_.IsOpen()) {
        if (popup_.OnKey(vk)) return true;
    }
    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    if (vk == VK_TAB) {
        MoveFocus(shift ? -1 : 1);
        return true;
    }
    Control* f = focus_;
    if (f && f->enabled) {
        if (vk == VK_RETURN) {
            if (f->UsesEnter()) {
                f->focusVisible = true;
                f->Activate();
                return true;
            }
        } else if (vk == VK_SPACE) {
            f->Activate();
            f->focusVisible = true;
            return true;
        } else if (f->OnKey(vk)) {
            f->focusVisible = true;
            return true;
        }
    }
    if (vk == VK_RETURN) {
        if (cur_ == defaultActionPage && onDefaultAction) {
            onDefaultAction();
            return true;
        }
        return false;
    }
    switch (vk) {
    case VK_DOWN: SetScroll(scroll_ + 48.0f); return true;
    case VK_UP: SetScroll(scroll_ - 48.0f); return true;
    case VK_NEXT: SetScroll(scroll_ + (ClientH() - metric::kHeaderHeight) * 0.9f); return true;
    case VK_PRIOR: SetScroll(scroll_ - (ClientH() - metric::kHeaderHeight) * 0.9f); return true;
    default: return false;
    }
}

void View::OnWindowDeactivated() {
    CloseDropdown();
    if (pressed_) {
        pressed_->OnEscape();
        pressed_->pressed = false;
        pressed_ = nullptr;
    }
    scrollDrag_ = false;
}

void View::OnControlEnabledChanged(Control* c) {
    if (hover_ == c && !c->enabled) hover_ = nullptr;
    if (pressed_ == c && !c->enabled) pressed_ = nullptr;
}

// ---------------------------------------------------------------------------------------------------------------------
// Navigation and dropdowns
// ---------------------------------------------------------------------------------------------------------------------

void View::Navigate(int index) {
    if (index < 0 || index >= static_cast<int>(pages_.size())) return;
    CloseDropdown();
    if (index != cur_) {
        if (hover_ && hover_ != nav_[static_cast<size_t>(index)].get()) {
            hover_->hover = false;
            hover_ = nullptr;
        }
        cur_ = index;
        scroll_ = 0.0f;
        for (auto& n : nav_) n->selected = (n->index == cur_);
        layoutDirty_ = true;
        if (onPageChanged) onPageChanged(cur_);
    }
    Invalidate();
}

void View::MoveNav(int delta) {
    const int n = static_cast<int>(nav_.size());
    const int idx = std::clamp(cur_ + delta, 0, n - 1);
    Navigate(idx);
    FocusControl(nav_[static_cast<size_t>(idx)].get(), true);
}

void View::OpenDropdown(ComboBox* combo) {
    if (!combo || !hwnd_) return;
    CloseDropdown();
    if (combo->Count() == 0) return;
    // The combo's bounds are in page space; find out whether it is on the current page.
    float ox = contentLeft_, oy = metric::kHeaderHeight - scroll_;
    const float s = Scale();
    POINT origin{0, 0};
    ClientToScreen(hwnd_, &origin);
    RECT anchor{origin.x + static_cast<LONG>(std::lround((ox + combo->bounds.left) * s)),
                origin.y + static_cast<LONG>(std::lround((oy + combo->bounds.top) * s)),
                origin.x + static_cast<LONG>(std::lround((ox + combo->bounds.right) * s)),
                origin.y + static_cast<LONG>(std::lround((oy + combo->bounds.bottom) * s))};
    std::vector<std::wstring> items;
    for (const auto& it : combo->items()) items.push_back(it.text);
    popupCombo_ = combo;
    combo->SetOpen(true);
    if (!popup_.Open(hwnd_, items, combo->Selected(), anchor, RW(combo->bounds), dpi_, pal_)) {
        popupCombo_ = nullptr;
        combo->SetOpen(false);
    }
}

void View::CloseDropdown() {
    if (popup_.IsOpen()) popup_.Close(false);
}

ComboBox* View::FirstComboOnPage() const {
    if (cur_ >= static_cast<int>(pages_.size())) return nullptr;
    for (Control* c : pages_[static_cast<size_t>(cur_)]->focusOrder())
        if (auto* cb = dynamic_cast<ComboBox*>(c))
            if (cb->enabled) return cb;
    return nullptr;
}

}  // namespace moonsr::app::ui
