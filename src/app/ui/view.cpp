#include <windows.h>
#include <timeapi.h>
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
    pal_ = MakePalette(ThemeId::Dark);
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
    if (hwnd_ && animating_) { KillTimer(hwnd_, kAnimTimerId); timeEndPeriod(1); }
    if (hwnd_ && blinking_) KillTimer(hwnd_, kBlinkTimerId);
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

void View::SetTheme(const Palette& palette) {
    pal_ = palette;
    // Fonts, icon glyphs and the corner / border style follow the theme; the layout depends on text metrics.
    gfx_.SetStyle(pal_.font, pal_.retro, pal_.radius, pal_.borderWidth);
    CloseDropdown();
    popup_.SetPalette(pal_);
    layoutDirty_ = true;
    UpdateBlinkTimer();
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

void View::SetEffects(bool on) {
    if (effects_ == on) return;
    effects_ = on;
    if (!on) {
        pageT_ = 1.0f;
        SnapStates();
    }
    blinkOn_ = true;
    UpdateBlinkTimer();
    Invalidate();
}

void View::SetShown(bool shown) {
    if (shown_ == shown) return;
    shown_ = shown;
    if (!shown) {
        if (animating_ && hwnd_) { KillTimer(hwnd_, kAnimTimerId); timeEndPeriod(1); }
        animating_ = false;
        pageT_ = 1.0f;
        SnapStates();
    }
    blinkOn_ = true;
    UpdateBlinkTimer();
    if (shown) Invalidate();
}

bool View::NeedsBlink() const { return effects_ && shown_ && hwnd_ && (pal_.promptHeaders || pal_.retro); }

void View::UpdateBlinkTimer() {
    if (!hwnd_) return;
    const bool want = NeedsBlink();
    if (want && !blinking_) {
        blinking_ = SetTimer(hwnd_, kBlinkTimerId, kBlinkMs, nullptr) != 0;
    } else if (!want && blinking_) {
        KillTimer(hwnd_, kBlinkTimerId);
        blinking_ = false;
        blinkOn_ = true;
    }
}

void View::OnBlinkTimer() {
    if (!NeedsBlink()) {
        UpdateBlinkTimer();
        return;
    }
    blinkOn_ = !blinkOn_;
    Invalidate();
}

void View::ResetBlink() {
    if (!blinking_) return;
    blinkOn_ = true;
    SetTimer(hwnd_, kBlinkTimerId, kBlinkMs, nullptr);  // restarts the period
    Invalidate();
}

void View::SnapStates() {
    for (auto& p : pages_) p->Tick(0.0f, false, false);
    for (auto& n : nav_) {
        n->StepState(0.0f, false, false);
        n->Tick(0.0f);
    }
}

bool View::AnyAnimPending() const {
    if (pageT_ < 1.0f) return true;
    for (const auto& n : nav_)
        if (n->AnimPending()) return true;
    return cur_ < static_cast<int>(pages_.size()) && pages_[static_cast<size_t>(cur_)]->AnimPending();
}

void View::StartAnimTimer() {
    if (!hwnd_ || animating_ || !shown_) return;
    lastTick_ = GetTickCount64();
    // 10 ms frames; a 1 ms system timer period is held only while animating so the timer can run that fast.
    timeBeginPeriod(1);
    animating_ = SetTimer(hwnd_, kAnimTimerId, 10, nullptr) != 0;
    if (!animating_) timeEndPeriod(1);
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
    if (!effects_ || !shown_) {
        SnapStates();
        Invalidate();
        return;
    }
    StartAnimTimer();
    Invalidate();
}

void View::OnAnimTimer() {
    const ULONGLONG now = GetTickCount64();
    const float dt = std::min(0.1f, static_cast<float>(now - lastTick_) / 1000.0f);
    lastTick_ = now;
    bool again = false;
    const bool anim = effects_ && shown_;
    const bool glow = Afterglow();
    for (auto& p : pages_) again |= p->Tick(dt, anim, glow);
    for (auto& n : nav_) {
        again |= n->StepState(dt, anim, glow);
        again |= n->Tick(dt);
    }
    if (pageT_ < 1.0f) {
        pageT_ = anim ? std::min(1.0f, pageT_ + dt / 0.20f) : 1.0f;
        again |= pageT_ < 1.0f;
    }
    if (!again) {
        animating_ = false;
        { KillTimer(hwnd_, kAnimTimerId); timeEndPeriod(1); }
    }
    Invalidate();
}

void View::TestWheel(float x, float y, float notches) {
    mouseX_ = x;
    mouseY_ = y;
    OnMouseWheel(static_cast<int>(notches * WHEEL_DELTA), 0, 0);
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
    if (!effects_) SnapStates();
    if (canvas_.Begin(hwnd_, dpi_, mica_)) {
        const float w = ClientW(), h = ClientH();
        if (mica_) {
            // Translucent system backdrop: a tint over Mica, or the console background at backdropAlpha over Acrylic.
            canvas_.Clear(D2D1_COLOR_F{0, 0, 0, 0});
            canvas_.FillRect(Rect{0, 0, w, h}, pal_.opaqueBg ? D2D1_COLOR_F{pal_.bg.r, pal_.bg.g, pal_.bg.b, pal_.backdropAlpha} : pal_.micaTint);
        } else if (pal_.opaqueBg) {
            canvas_.Clear(pal_.bg);
        } else {
            canvas_.Clear(pal_.bg);
            canvas_.FillGradient(Rect{0, 0, w, h}, 0.0f, {{0.0f, pal_.bgA}, {0.5f, pal_.bgB}, {1.0f, pal_.bgC}}, 0.0f, 0.0f, w, h);
        }
        // Phosphor glow on accent text and blocks (console themes with glow, effects on).
        canvas_.SetPhosphor(effects_ && pal_.retro && pal_.glow, pal_.accent, pal_.accentHover);
        DrawNav(w, h);
        // Page switch (Fluent themes): the new page fades in and slides up a little.
        const float e = 1.0f - (1.0f - pageT_) * (1.0f - pageT_) * (1.0f - pageT_);  // ease-out cubic
        const bool fading = pageT_ < 1.0f;
        if (fading) canvas_.PushOpacity(Rect{navW_, 0, w, h}, e);
        DrawHeader(w);
        canvas_.PushClip(Rect{navW_, metric::kHeaderHeight, w, h});
        canvas_.SetOffset(contentLeft_, metric::kHeaderHeight - scroll_ + (fading ? (1.0f - e) * 16.0f : 0.0f));
        pages_[static_cast<size_t>(cur_)]->Draw(canvas_, pal_, gfx_);
        canvas_.SetOffset(0, 0);
        canvas_.PopClip();
        if (fading) canvas_.PopOpacity();
        DrawScrollbar(w, h);
        if (dialog_) dialog_->Draw(canvas_, pal_, w, h);
        if (effects_ && pal_.scanlines) canvas_.Scanlines(Rect{0, 0, w, h}, 0.13f);
        canvas_.SetPhosphor(false, pal_.accent, pal_.accent);
        if (!canvas_.End()) Invalidate();
    }
    EndPaint(hwnd_, &ps);
    // Something is still on its way to its target (a hover fade after a mouse move, for example): keep animating.
    if (effects_ && !animating_ && AnyAnimPending()) StartAnimTimer();
}

void View::DrawNav(float, float h) {
    // Brand header: the logo mark and the "MoonSR" wordmark ("Moon" in the text colour, "SR" in the logo gradient), both
    // vector, so they are sharp at every DPI. Console themes use the phosphor colours, a monospace wordmark and a cursor.
    const LogoColors lc = LogoColorsFor(pal_);
    const float markH = 34.0f, markW = markH * 228.0f / 176.0f, markTop = 18.0f, cy = markTop + markH * 0.5f;
    const float lx = compact_ ? (navW_ - markW) * 0.5f : 12.0f;
    canvas_.LogoMark(Rect{lx, markTop, lx + markW, markTop + markH}, lc, effects_ && pal_.glow);
    if (!compact_) {
        const float size = pal_.retro ? 21.0f : 23.0f;
        const float wx = lx + markW + 8.0f;
        const float wy = cy - size * (pal_.retro ? 0.68f : 0.67f);
        float lh = 0.0f;
        const float ww = canvas_.Wordmark(wx, wy, size, pal_.text, lc.srA, lc.srB, &lh);
        if (pal_.retro && CursorOn())  // block cursor after the wordmark (underscore style), blinking with effects on
            canvas_.FillRect(Rect{wx + ww + 3.0f, wy + lh * 0.74f, wx + ww + 3.0f + size * 0.55f, wy + lh * 0.74f + 3.5f}, pal_.accent);
    }
    if (pal_.promptHeaders) canvas_.Line(navW_ - 0.5f, 0.0f, navW_ - 0.5f, ClientH(), pal_.divider, 1.0f);
    for (auto& n : nav_) n->Draw(canvas_, pal_);
    if (!navStatus_.empty()) {
        const float y = h - 40.0f;
        const D2D1_COLOR_F dot = navActive_ ? pal_.success.icon : pal_.strong;
        if (pal_.promptHeaders) {
            const std::wstring prompt = compact_ ? L">" : L"C:\\MoonSR> ";
            const float pw = gfx_.MeasureWidth(prompt, Font::Caption);
            canvas_.Text(prompt, Rect{compact_ ? 0.0f : 12.0f, y, navW_, y + 40.0f}, Font::Caption, pal_.textSecondary,
                         compact_ ? HAlign::Center : HAlign::Left);
            if (!compact_) {
                canvas_.Text(navStatus_, Rect{12.0f + pw, y, navW_ - 8.0f, y + 40.0f}, Font::Caption, navActive_ ? pal_.accent : pal_.text);
                const float cx = 12.0f + pw + gfx_.MeasureWidth(navStatus_, Font::Caption) + 3.0f;
                if (CursorOn() && cx + 8.0f < navW_ - 6.0f) canvas_.FillRect(Rect{cx, y + 13.0f, cx + 7.0f, y + 27.0f}, pal_.accent);
            }
        } else if (compact_) {
            canvas_.FillCircle(navW_ * 0.5f, y + 20.0f, 4.0f, dot);
        } else {
            canvas_.FillCircle(24.0f, y + 20.0f, 4.0f, dot);
            canvas_.Text(navStatus_, Rect{40.0f, y, navW_ - 8.0f, y + 40.0f}, Font::Caption, pal_.textSecondary);
        }
    }
}

void View::DrawHeader(float w) {
    const std::wstring& title = pages_[static_cast<size_t>(cur_)]->title();
    if (pal_.promptHeaders) {
        // "> Title" followed by a block cursor.
        const std::wstring t = L"> " + title;
        canvas_.Text(t, Rect{contentLeft_, 14.0f, w - 16.0f, 62.0f}, Font::Title, pal_.accent);
        const float tw = gfx_.MeasureWidth(t, Font::Title);
        if (CursorOn() && contentLeft_ + tw + 24.0f < w) canvas_.FillRect(Rect{contentLeft_ + tw + 6.0f, 22.0f, contentLeft_ + tw + 18.0f, 46.0f}, pal_.accent);
        return;
    }
    canvas_.Text(title, Rect{contentLeft_, 14.0f, w - 16.0f, 62.0f}, Font::Title, pal_.text);
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
    if (pal_.retro) canvas_.FillRect(t, scrollDrag_ ? pal_.accentHover : scrollHover_ ? pal_.accent : pal_.strong);
    else canvas_.FillRound(t, RW(t) * 0.5f, D2D1_COLOR_F{pal_.strong.r, pal_.strong.g, pal_.strong.b, pal_.strong.a * 0.8f});
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
    // A scrollable control under the pointer (the theme cards) scrolls first; at its end the page scrolls.
    if (mouseX_ >= 0) {
        if (Control* c = HitTest(mouseX_, mouseY_, nullptr); c && c->OnWheel(static_cast<float>(delta) / WHEEL_DELTA)) {
            OnMouseMove(static_cast<int>(mouseX_ * Scale()), static_cast<int>(mouseY_ * Scale()));
            return;
        }
    }
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
        Control* old = focus_;
        old->focused = false;
        old->focusVisible = false;
        old->OnFocusChanged(false, keyboard);
    }
    focus_ = c;
    if (c) {
        c->focused = true;
        c->focusVisible = keyboard;
        if (keyboard) ScrollIntoView(c);
        c->OnFocusChanged(true, keyboard);
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

bool View::OnChar(wchar_t ch) {
    if (dialog_ || popup_.IsOpen() || !focus_ || !focus_->enabled) return false;
    return focus_->OnChar(ch);
}

void View::OnWindowFocusChanged(bool active) {
    if (!active && focus_) focus_->OnFocusChanged(false, false);
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
        if (effects_ && shown_ && !pal_.retro) {  // Fluent page switch: fade and slide in
            pageT_ = 0.0f;
            StartAnimTimer();
        }
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
