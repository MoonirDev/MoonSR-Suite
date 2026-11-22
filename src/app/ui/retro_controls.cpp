// Console ("Retro") rendering of the custom controls and the theme picker. The Windows 11 Dark look stays in controls.cpp; each
// control's Draw() hands over to the DrawRetro() here when the palette has `retro` set. Everything is flat: square corners,
// 1 px solid borders, monospace text, bracketed buttons, block sliders, inverted (accent background) hover and selection.
// Threading: UI thread only.
#include "controls.h"

#include "view.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace moonsr::app::ui {

namespace {
D2D1_COLOR_F Fade(const D2D1_COLOR_F& c, float a) { return D2D1_COLOR_F{c.r, c.g, c.b, c.a * a}; }
const wchar_t* InfoPrefix(InfoBar::Severity s) {
    switch (s) {
    case InfoBar::Severity::Success: return L"OK:";
    case InfoBar::Severity::Warning: return L"WARN:";
    case InfoBar::Severity::Error: return L"ERROR:";
    default: return L"INFO:";
    }
}
}  // namespace

void ToggleSwitch::DrawRetro(Canvas& c, const Palette& p) {
    const float cy = (bounds.top + bounds.bottom) * 0.5f;
    const std::wstring label = checked_ ? L"[ON ]" : L"[OFF]";
    const float w = c.gfx().MeasureWidth(label, Font::BodyStrong) + 10.0f;
    const Rect box{bounds.right - w, cy - 12.0f, bounds.right, cy + 12.0f};
    const float lit = Lit();  // inverted highlight; fades out (afterglow) when the pointer leaves
    if (lit > 0.0f) c.FillRect(box, Fade(checked_ ? p.accent : p.text, lit));
    const D2D1_COLOR_F col = !enabled ? p.textDisabled : lit > 0.5f ? p.textOnAccent : checked_ ? p.accent : p.textSecondary;
    c.Text(label, box, Font::BodyStrong, col, HAlign::Center);
    DrawFocus(c, p, box, 2.0f);
}

float Button::RetroPreferredWidth(const Gfx& g) const {
    return std::max(64.0f, g.MeasureWidth(L"[ " + text + L" ]", Font::BodyStrong) + 16.0f);
}

void Button::DrawRetro(Canvas& c, const Palette& p) {
    const float lit = Lit();
    if (lit > 0.0f) c.FillRect(bounds, Fade(accent ? p.accent : p.text, lit));
    const D2D1_COLOR_F tc = !enabled ? p.textDisabled : lit > 0.5f ? p.textOnAccent : accent ? p.accent : p.text;
    c.Text(L"[ " + text + L" ]", Rect{bounds.left + 4, bounds.top, bounds.right - 4, bounds.bottom}, Font::BodyStrong, tc,
           HAlign::Center);
    DrawFocus(c, p, bounds, 0.0f, 0.0f);
}

void ComboBox::DrawRetro(Canvas& c, const Palette& p) {
    const D2D1_COLOR_F fill = !enabled ? p.ctrlDisabled : open_ ? p.ctrlPressed : StateColor(p.ctrlFill, p.ctrlHover, p.ctrlPressed);
    c.FillRect(bounds, fill);
    c.StrokeRound(bounds, 0.0f, !enabled ? p.divider : (open_ || hover) ? p.accent : p.ctrlStroke);
    c.Text(selected_ >= 0 ? items_[static_cast<size_t>(selected_)].text : std::wstring(),
           Rect{bounds.left + 10, bounds.top, bounds.right - 28, bounds.bottom}, Font::Body, enabled ? p.text : p.textDisabled,
           HAlign::Left);
    c.Text(L"v", Rect{bounds.right - 26, bounds.top, bounds.right - 6, bounds.bottom}, Font::BodyStrong,
           enabled ? p.accent : p.textDisabled, HAlign::Center);
    DrawFocus(c, p, bounds, 0.0f, 2.0f);
}

void Slider::DrawRetro(Canvas& c, const Palette& p) {
    // Block track: filled cells up to the value, thin dashes after it, a tall block as the thumb.
    const float cy = (bounds.top + bounds.bottom) * 0.5f;
    const float l = TrackLeft(), r = TrackRight();
    const float t = ShownT();
    const float tx = l + (r - l) * t;
    const float cell = 8.0f;
    const D2D1_COLOR_F on = enabled ? p.accent : p.accentDisabled, off = enabled ? p.strong : p.textDisabled;
    for (float x = l; x + 6.0f <= r + 0.5f; x += cell) {
        if (x + 3.0f <= tx) c.FillRect(Rect{x, cy - 5.0f, x + 6.0f, cy + 5.0f}, on);
        else c.FillRect(Rect{x, cy - 1.0f, x + 6.0f, cy + 1.0f}, off);
    }
    const bool hot = enabled && (pressed || hover);
    c.FillRect(Rect{tx - 3.0f, cy - 10.0f, tx + 3.0f, cy + 10.0f}, !enabled ? p.textDisabled : hot ? p.text : p.accent);
    const std::wstring label = displayDivisor_ > 1 ? std::format(L"{:.2f}{}", static_cast<double>(value_) / displayDivisor_, suffix_)
                                                  : std::format(L"{}{}", value_, suffix_);
    c.Text(label, Rect{r + 14.0f, bounds.top, bounds.right, bounds.bottom}, Font::Body, enabled ? p.text : p.textDisabled,
           HAlign::Right);
    DrawFocus(c, p, Rect{tx - 7.0f, cy - 12.0f, tx + 7.0f, cy + 12.0f}, 0.0f, 0.0f);
}

void NumberBox::DrawRetro(Canvas& c, const Palette& p) {
    gfx_ = &c.gfx();
    c.FillRect(bounds, !enabled ? p.ctrlDisabled : focused ? p.ctrlHover : hover ? p.ctrlHover : p.ctrlFill);
    c.StrokeRound(bounds, 0.0f, clamped_ ? p.error.icon : focused ? p.accent : !enabled ? p.divider : p.ctrlStroke);
    const Rect inner{bounds.left + 10.0f, bounds.top, bounds.right - 10.0f, bounds.bottom};
    const float cy = (bounds.top + bounds.bottom) * 0.5f;
    if (!suffix_.empty()) c.Text(suffix_, inner, Font::Body, enabled ? p.textSecondary : p.textDisabled, HAlign::Right);
    if (focused && HasSelection()) {
        const float a = XOf(std::min(caret_, anchor_)), b = XOf(std::max(caret_, anchor_));
        c.FillRect(Rect{a, cy - 9.0f, b, cy + 9.0f}, Fade(p.accent, 0.45f));
    }
    c.Text(text_, inner, Font::Body, enabled ? p.text : p.textDisabled, HAlign::Left);
    if (focused && !HasSelection() && enabled && CursorOn()) {
        // Block caret (blinks with visual effects on): one character cell, translucent so the character stays readable.
        const float x = XOf(caret_);
        const float cw = std::max(7.0f, c.gfx().MeasureWidth(L"0", Font::Body));
        c.FillRect(Rect{x, cy - 9.0f, x + cw, cy + 9.0f}, Fade(p.accent, 0.6f));
    }
    DrawFocus(c, p, bounds, 0.0f, 2.0f);
}

float InfoBar::RetroMeasure(const Gfx& g, float width) const {
    const float prefixW = g.MeasureWidth(L"ERROR:", Font::BodyStrong);
    const float textH = g.MeasureHeight(message_, Font::Body, width - 12.0f - prefixW - 10.0f - 12.0f);
    return std::max(40.0f, textH + 22.0f);
}

void InfoBar::DrawRetro(Canvas& c, const Palette& p) {
    const InfoColors& col = severity_ == Severity::Success   ? p.success
                            : severity_ == Severity::Warning ? p.warning
                            : severity_ == Severity::Error   ? p.error
                                                             : p.info;
    c.FillRect(bounds, col.fill);
    c.StrokeRound(bounds, 0.0f, col.icon);
    const float prefixW = c.gfx().MeasureWidth(L"ERROR:", Font::BodyStrong);
    c.Text(InfoPrefix(severity_), Rect{bounds.left + 12.0f, bounds.top + 11.0f, bounds.left + 12.0f + prefixW + 4.0f, bounds.top + 31.0f},
           Font::BodyStrong, col.icon, HAlign::Left);
    c.Text(message_, Rect{bounds.left + 12.0f + prefixW + 10.0f, bounds.top + 11.0f, bounds.right - 12.0f, bounds.bottom - 8.0f},
           Font::Body, p.text, HAlign::Left, true);
}

void Tile::DrawRetro(Canvas& c, const Palette& p) {
    c.FillRect(bounds, p.cardFill);
    c.StrokeRound(bounds, 0.0f, p.cardStroke);
    const float x = bounds.left + 12.0f;
    const float w = RW(bounds) - 24.0f;
    c.Text(label, Rect{x, bounds.top + 8.0f, bounds.right - 8.0f, bounds.top + 28.0f}, Font::Caption, p.textSecondary);
    Font f = Font::Value;
    const Gfx& g = c.gfx();
    float vw = g.MeasureWidth(value_, f);
    if (vw > w) {
        f = Font::ValueSmall;
        vw = g.MeasureWidth(value_, f);
    }
    const Rect vr{x, bounds.top + 30.0f, x + std::min(vw + 2.0f, w), bounds.bottom - 8.0f};
    c.Text(value_, vr, f, p.accent, HAlign::Left);
    if (!note_.empty()) {
        const float nx = vr.right + 8.0f;
        if (nx < bounds.right - 12.0f) c.Text(note_, Rect{nx, vr.top, bounds.right - 8.0f, vr.bottom}, Font::Caption, p.textSecondary);
    }
}

float HeroBanner::RetroHeight() { return 132.0f; }

void HeroBanner::DrawRetro(Canvas& c, const Palette& p) {
    // A console window: title strip, banner text and a prompt line.
    c.FillRect(bounds, p.cardFill);
    c.StrokeRound(bounds, 0.0f, p.cardStroke);
    const Rect strip{bounds.left, bounds.top, bounds.right, bounds.top + 24.0f};
    c.FillRect(strip, p.accent);
    c.Text(L"C:\\MoonSR\\moonsr.exe", Rect{bounds.left + 10.0f, strip.top, bounds.right - 10.0f, strip.bottom}, Font::BodyStrong,
           p.textOnAccent, HAlign::Left);
    const float x = bounds.left + 16.0f;
    c.Text(L"MoonSR Suite", Rect{x, bounds.top + 32.0f, bounds.right - 16.0f, bounds.top + 70.0f}, Font::Title, p.accent);
    c.Text(L"Game upscaling and more.", Rect{x, bounds.top + 70.0f, bounds.right - 16.0f, bounds.top + 92.0f}, Font::Body,
           p.textSecondary);
    const std::wstring prompt = L"C:\\MoonSR> ";
    const float pw = c.gfx().MeasureWidth(prompt, Font::Body);
    c.Text(prompt, Rect{x, bounds.top + 96.0f, x + pw + 4.0f, bounds.top + 120.0f}, Font::Body, p.text);
    if (CursorOn()) c.FillRect(Rect{x + pw, bounds.top + 101.0f, x + pw + 8.0f, bounds.top + 115.0f}, p.accent);
}

void NavItem::DrawRetro(Canvas& c, const Palette& p) {
    // The selection block; when the selection moves, the old block fades out (phosphor afterglow, selT).
    const float sel = selected ? 1.0f : selT;
    if (sel < 1.0f) {
        const D2D1_COLOR_F h = StateColor(D2D1_COLOR_F{0, 0, 0, 0}, p.subtleHover, p.subtlePressed);
        if (h.a > 0.0f) c.FillRect(bounds, h);
    }
    if (sel > 0.0f) c.FillRect(bounds, Fade(p.accent, sel));
    const D2D1_COLOR_F tc = sel > 0.5f ? p.textOnAccent : p.text;
    if (compact) {
        c.Text(std::wstring(1, label.empty() ? L'?' : label[0]), bounds, Font::BodyStrong, tc, HAlign::Center);
    } else {
        c.Text((selected ? L"> " : L"  ") + label, Rect{bounds.left + 10.0f, bounds.top, bounds.right - 6.0f, bounds.bottom},
               selected ? Font::BodyStrong : Font::Body, tc, HAlign::Left);
    }
    DrawFocus(c, p, bounds, 0.0f, -1.0f);
}

// ---------------------------------------------------------------------------------------------------------------------
// ThemePicker
// ---------------------------------------------------------------------------------------------------------------------

namespace {
constexpr float kGap = 8.0f;
constexpr float kCardH = 108.0f;
constexpr float kPreviewW = 168.0f;
constexpr float kPreviewH = 84.0f;
constexpr float kVisibleCards = 2.5f;   // the scroll region shows about two and a half cards
constexpr float kBarGutter = 16.0f;     // room for the scrollbar right of the cards
constexpr float kWheelStep = 64.0f;     // DIPs per wheel notch
}  // namespace

float ThemePicker::ContentHeight() const {
    const float n = static_cast<float>(std::max<size_t>(1, cards_.size()));
    return n * kCardH + (n - 1.0f) * kGap;
}

float ThemePicker::HeightForWidth(const Gfx&, float) const {
    return std::min(ContentHeight(), kVisibleCards * kCardH + 2.0f * kGap);
}

float ThemePicker::MaxScroll() const { return std::max(0.0f, ContentHeight() - RH(bounds)); }

Rect ThemePicker::CardRect(int i) const {
    const float t = bounds.top + static_cast<float>(i) * (kCardH + kGap) - scroll_;
    return Rect{bounds.left, t, bounds.right - (Scrollable() ? kBarGutter : 0.0f), t + kCardH};
}

Rect ThemePicker::RemoveRect(int i) const {
    const Rect c = CardRect(i);
    return Rect{c.right - 112.0f, c.top + 10.0f, c.right - 10.0f, c.top + 38.0f};
}

Rect ThemePicker::TrackRect() const {
    return Rect{bounds.right - kBarGutter + 4.0f, bounds.top + 2.0f, bounds.right - 2.0f, bounds.bottom - 2.0f};
}

Rect ThemePicker::ThumbRect() const {
    const Rect tr = TrackRect();
    const float th = std::max(28.0f, RH(tr) * RH(bounds) / ContentHeight());
    const float ms = MaxScroll();
    const float ty = tr.top + (RH(tr) - th) * (ms > 0.0f ? scroll_ / ms : 0.0f);
    return Rect{tr.left, ty, tr.right, ty + th};
}

int ThemePicker::CardAt(float x, float y) const {
    if (y < bounds.top || y >= bounds.bottom) return -1;  // outside the scroll region (cards scrolled out of view)
    for (int i = 0; i < static_cast<int>(cards_.size()); ++i)
        if (RectHas(CardRect(i), x, y)) return i;
    return -1;
}

void ThemePicker::ScrollTo(float v, bool animate) {
    v = std::clamp(v, 0.0f, MaxScroll());
    scrollTarget_ = v;
    if (animate && Effects()) {
        view->RequestAnimation();
    } else {
        scroll_ = v;
    }
    Invalidate();
}

void ThemePicker::EnsureVisible(int i, bool animate) {
    if (i < 0) return;
    if (RH(bounds) <= 0.0f) {
        ensurePending_ = true;
        return;
    }
    const float top = static_cast<float>(i) * (kCardH + kGap), bottom = top + kCardH;
    if (top < scrollTarget_) ScrollTo(top, animate);
    else if (bottom > scrollTarget_ + RH(bounds)) ScrollTo(bottom - RH(bounds), animate);
}

void ThemePicker::SetCards(std::vector<Card> cards) {
    cards_ = std::move(cards);
    cardHover_.assign(cards_.size(), 0.0f);
    hoverCard_ = pressCard_ = -1;
    if (SelectedIndex() < 0) sel_ = L"dark";
    if (view) view->RequestLayout();
    Invalidate();
}

int ThemePicker::SelectedIndex() const {
    for (int i = 0; i < static_cast<int>(cards_.size()); ++i)
        if (cards_[static_cast<size_t>(i)].key == sel_) return i;
    return -1;
}

void ThemePicker::SetSelected(const std::wstring& key) {
    sel_ = key;
    EnsureVisible(SelectedIndex(), false);
    Invalidate();
}

void ThemePicker::Select(int i, bool notify) {
    if (i < 0 || i >= static_cast<int>(cards_.size())) return;
    EnsureVisible(i, true);
    if (cards_[static_cast<size_t>(i)].key == sel_) return;
    sel_ = cards_[static_cast<size_t>(i)].key;
    Invalidate();
    if (notify && onChange) onChange();
}

void ThemePicker::OnPress(float x, float y) {
    dragThumb_ = false;
    if (Scrollable() && x >= bounds.right - kBarGutter) {
        const Rect th = ThumbRect();
        if (y >= th.top && y < th.bottom) {
            dragThumb_ = true;
            dragOff_ = y - th.top;
        } else {  // track click: one page towards the click
            ScrollTo(scrollTarget_ + (y < th.top ? -1.0f : 1.0f) * RH(bounds) * 0.85f, true);
        }
        pressCard_ = -1;
        return;
    }
    pressCard_ = CardAt(x, y);
    pressRemove_ = pressCard_ >= 0 && !cards_[static_cast<size_t>(pressCard_)].builtin && RectHas(RemoveRect(pressCard_), x, y);
}

void ThemePicker::OnDrag(float, float y) {
    if (!dragThumb_) return;
    const Rect tr = TrackRect(), th = ThumbRect();
    const float range = std::max(1.0f, RH(tr) - RH(th));
    ScrollTo((y - dragOff_ - tr.top) / range * MaxScroll(), false);
}

void ThemePicker::OnRelease(float x, float y, bool) {
    if (dragThumb_) {
        dragThumb_ = false;
        Invalidate();
        return;
    }
    const int i = CardAt(x, y);
    if (i >= 0 && i == pressCard_ && enabled) {
        if (pressRemove_) {
            if (RectHas(RemoveRect(i), x, y) && onRemove) onRemove(cards_[static_cast<size_t>(i)].key);
        } else {
            Select(i, true);
        }
    }
    pressCard_ = -1;
    pressRemove_ = false;
}

void ThemePicker::OnHoverMove(float x, float y) {
    const bool bar = Scrollable() && x >= bounds.right - kBarGutter && y >= bounds.top && y < bounds.bottom;
    const int i = bar ? -1 : CardAt(x, y);
    const bool r = i >= 0 && !cards_[static_cast<size_t>(i)].builtin && RectHas(RemoveRect(i), x, y);
    if (i != hoverCard_ || r != hoverRemove_ || bar != hoverBar_) {
        hoverCard_ = i;
        hoverRemove_ = r;
        hoverBar_ = bar;
        Invalidate();
    }
}

bool ThemePicker::OnWheel(float delta) {
    if (!Scrollable()) return false;
    const float v = std::clamp(scrollTarget_ - delta * kWheelStep, 0.0f, MaxScroll());
    if (v == scrollTarget_) return false;  // at the end: let the page scroll
    ScrollTo(v, true);
    return true;
}

bool ThemePicker::OnKey(UINT vk) {
    const int i = SelectedIndex();
    if (vk == VK_LEFT || vk == VK_UP) { Select(i - 1, true); return true; }
    if (vk == VK_RIGHT || vk == VK_DOWN) { Select(i + 1, true); return true; }
    if (vk == VK_HOME) { Select(0, true); return true; }
    if (vk == VK_END) { Select(static_cast<int>(cards_.size()) - 1, true); return true; }
    if (vk == VK_DELETE && i >= 0 && !cards_[static_cast<size_t>(i)].builtin && onRemove) {
        onRemove(cards_[static_cast<size_t>(i)].key);
        return true;
    }
    return false;
}

void ThemePicker::Activate() {}

bool ThemePicker::AnimPending() const {
    if (scroll_ != scrollTarget_) return true;
    for (size_t i = 0; i < cardHover_.size(); ++i)
        if (cardHover_[i] != (static_cast<int>(i) == hoverCard_ ? 1.0f : 0.0f)) return true;
    return Control::AnimPending();
}

bool ThemePicker::Tick(float dt) {
    const bool anim = Effects();
    bool again = false;
    if (scroll_ != scrollTarget_) {
        if (!anim) {
            scroll_ = scrollTarget_;
        } else {
            scroll_ += (scrollTarget_ - scroll_) * std::min(1.0f, dt / 0.07f);
            if (std::fabs(scroll_ - scrollTarget_) < 0.5f) scroll_ = scrollTarget_;
            again |= scroll_ != scrollTarget_;
        }
    }
    for (size_t i = 0; i < cardHover_.size(); ++i) {
        const float target = static_cast<int>(i) == hoverCard_ ? 1.0f : 0.0f;
        float& v = cardHover_[i];
        if (v == target) continue;
        if (!anim) v = target;
        else v = target > v ? std::min(target, v + dt / 0.083f) : std::max(target, v - dt / 0.2f);
        again |= v != target;
    }
    return again;
}

void ThemePicker::DrawMiniature(Canvas& c, const Rect& r, const Palette& mp) {
    const float wasCap = c.radiusCap(), wasBw = c.borderW();
    const bool wasGlow = c.phosphor();
    const D2D1_COLOR_F wasA = c.glowA(), wasB = c.glowB();
    c.SetPhosphor(false, mp.accent, mp.accent);  // no glow inside miniatures
    c.SetStyle(mp.radius, 1.0f);                 // corner style of the previewed theme
    c.PushClip(r);
    if (mp.retro) c.FillRect(r, mp.bg);
    else c.FillGradient(r, 0.0f, {{0.0f, mp.bgA}, {0.5f, mp.bgB}, {1.0f, mp.bgC}}, r.left, r.top, r.right, r.bottom);
    // Navigation pane: the logo header (mark + wordmark in that theme's logo colours), then three items, the first selected.
    const float navW = RW(r) * 0.30f;
    c.FillRect(Rect{r.left + navW, r.top, r.left + navW + 1.0f, r.bottom}, mp.divider);
    const LogoColors lc = LogoColorsFor(mp);
    c.LogoMark(Rect{r.left + 3.0f, r.top + 4.0f, r.left + 16.0f, r.top + 14.0f}, lc, false);
    const float ww = c.Wordmark(r.left + 17.0f, r.top + 3.5f, 7.0f, mp.text, lc.srA, lc.srB, nullptr, mp.retro ? 1 : 0);
    if (mp.promptHeaders) c.FillRect(Rect{r.left + 17.5f + ww, r.top + 6.0f, r.left + 20.5f + ww, r.top + 12.5f}, mp.accent);
    for (int i = 0; i < 3; ++i) {
        const Rect it{r.left + 5.0f, r.top + 22.0f + static_cast<float>(i) * 18.0f, r.left + navW - 5.0f, r.top + 36.0f + static_cast<float>(i) * 18.0f};
        if (i == 0) {
            c.FillRound(it, 3.0f, mp.retro ? mp.accent : mp.subtleSelected);
            if (!mp.retro) c.FillRound(Rect{it.left, it.top + 3.0f, it.left + 2.0f, it.bottom - 3.0f}, 1.0f, mp.accent);
        }
        const float tw = (RW(it) - 14.0f) * (i == 1 ? 0.7f : 0.9f);
        c.FillRect(Rect{it.left + 6.0f, (it.top + it.bottom) * 0.5f - 1.0f, it.left + 6.0f + tw, (it.top + it.bottom) * 0.5f + 1.0f},
                   i == 0 && mp.retro ? mp.textOnAccent : mp.text);
    }
    // Header and a card with a toggle row and a slider row.
    const float cx = r.left + navW + 10.0f;
    c.FillRect(Rect{cx, r.top + 8.0f, cx + 44.0f, r.top + 12.0f}, mp.promptHeaders ? mp.accent : mp.text);
    const Rect card{cx, r.top + 22.0f, r.right - 10.0f, r.bottom - 10.0f};
    c.FillRound(card, 4.0f, mp.cardFill);
    c.StrokeRound(card, 4.0f, mp.cardStroke);
    const float row1 = card.top + RH(card) * 0.28f, row2 = card.top + RH(card) * 0.72f;
    c.FillRect(Rect{card.left + 8.0f, row1 - 1.0f, card.left + 34.0f, row1 + 1.0f}, mp.textSecondary);
    c.Line(card.left + 1.0f, (row1 + row2) * 0.5f, card.right - 1.0f, (row1 + row2) * 0.5f, mp.divider, 1.0f);
    const float tr = card.right - 8.0f;
    if (mp.retro) {
        const Rect tb{tr - 28.0f, row1 - 5.0f, tr, row1 + 5.0f};
        c.StrokeRound(tb, 0.0f, mp.accent);
        c.FillRect(Rect{tb.left + 3.0f, tb.top + 3.0f, tb.left + 12.0f, tb.bottom - 3.0f}, mp.accent);
    } else {
        const Rect tb{tr - 22.0f, row1 - 6.0f, tr, row1 + 6.0f};
        c.FillRound(tb, 6.0f, mp.accent);
        c.FillCircle(tb.right - 6.0f, row1, 3.0f, mp.textOnAccent);
    }
    const float sl = card.left + 8.0f, sr = card.right - 8.0f, sx = sl + (sr - sl) * 0.6f;
    if (mp.retro) {
        for (float x = sl; x + 5.0f <= sr; x += 7.0f) {
            if (x + 2.0f <= sx) c.FillRect(Rect{x, row2 - 4.0f, x + 5.0f, row2 + 4.0f}, mp.accent);
            else c.FillRect(Rect{x, row2 - 1.0f, x + 5.0f, row2 + 1.0f}, mp.strong);
        }
        c.FillRect(Rect{sx - 2.0f, row2 - 7.0f, sx + 2.0f, row2 + 7.0f}, mp.accent);
    } else {
        c.FillRound(Rect{sl, row2 - 2.0f, sr, row2 + 2.0f}, 2.0f, Fade(mp.strong, 0.6f));
        c.FillRound(Rect{sl, row2 - 2.0f, sx, row2 + 2.0f}, 2.0f, mp.accent);
        c.FillCircle(sx, row2, 6.0f, mp.thumb);
        c.FillCircle(sx, row2, 4.0f, mp.accent);
    }
    if (mp.scanlines) c.Scanlines(r, 0.12f);
    c.PopClip();
    c.StrokeRound(r, 4.0f, mp.cardStroke);
    c.SetStyle(wasCap, wasBw);
    c.SetPhosphor(wasGlow, wasA, wasB);
}

void ThemePicker::DrawScrollbar(Canvas& c, const Palette& p) {
    if (!Scrollable()) return;
    const Rect tr = TrackRect(), th = ThumbRect();
    const bool hot = hoverBar_ || dragThumb_;
    if (p.retro) {
        // Console: an outlined track with a dotted fill and a solid block thumb.
        c.StrokeRound(tr, 0.0f, hot ? p.accent : p.ctrlStroke);
        for (float y = tr.top + 4.0f; y + 2.0f <= tr.bottom - 3.0f; y += 4.0f)
            c.FillRect(Rect{(tr.left + tr.right) * 0.5f - 1.0f, y, (tr.left + tr.right) * 0.5f + 1.0f, y + 2.0f}, p.divider);
        c.FillRect(Inflate(th, -2.0f), dragThumb_ ? p.accentHover : p.accent);
        return;
    }
    // Fluent: a thin rounded thumb that widens on hover, over a faint track.
    if (hot) c.FillRound(tr, RW(tr) * 0.5f, p.ctrlFill);
    const float w = hot ? 6.0f : 4.0f;
    const float cx = (tr.left + tr.right) * 0.5f;
    const Rect t{cx - w * 0.5f, th.top + 2.0f, cx + w * 0.5f, th.bottom - 2.0f};
    const D2D1_COLOR_F col = dragThumb_ ? p.textSecondary : Fade(p.strong, hot ? 1.0f : 0.8f);
    c.FillRound(t, w * 0.5f, col);
}

void ThemePicker::Draw(Canvas& c, const Palette& p) {
    if (cardHover_.size() != cards_.size()) cardHover_.assign(cards_.size(), 0.0f);
    if (scroll_ > MaxScroll()) scroll_ = scrollTarget_ = MaxScroll();  // after a resize or a removed theme
    if (ensurePending_ && RH(bounds) > 0.0f) {
        ensurePending_ = false;
        EnsureVisible(SelectedIndex(), false);
    }
    c.PushClip(bounds);
    for (int i = 0; i < static_cast<int>(cards_.size()); ++i) {
        const Card& cd = cards_[static_cast<size_t>(i)];
        const Rect card = CardRect(i);
        if (card.bottom < bounds.top || card.top > bounds.bottom) continue;
        const bool sel = cd.key == sel_;
        const float hv = sel ? 0.0f : cardHover_[static_cast<size_t>(i)];
        c.FillRound(card, metric::kCardRadius, BlendOver(p.cardFill, p.ctrlHover, hv));
        c.StrokeRound(card, metric::kCardRadius, sel ? p.accent : p.cardStroke, sel ? 2.0f : 1.0f);
        const Rect prev{card.left + 12.0f, card.top + 12.0f, card.left + 12.0f + kPreviewW, card.top + 12.0f + kPreviewH};
        DrawMiniature(c, prev, cd.palette);
        const float tx = prev.right + 16.0f;
        const float ty = card.top + (kCardH - 64.0f) * 0.5f;
        const float mw = 30.0f;
        if (p.retro) {
            c.Text(sel ? L"[x]" : L"[ ]", Rect{tx, ty, tx + mw, ty + 22.0f}, Font::BodyStrong, sel ? p.accent : p.textSecondary);
        } else {
            const float mx = tx + 8.0f, my = ty + 11.0f;
            if (sel) {
                c.FillCircle(mx, my, 8.0f, p.accent);
                c.Check(mx, my - 0.5f, 9.0f, p.textOnAccent);
            } else {
                c.StrokeCircle(mx, my, 8.0f, p.strong, 1.0f);
            }
        }
        const bool removable = !cd.builtin;
        const float textRight = card.right - 12.0f - (removable ? 100.0f : 0.0f);
        c.Text(cd.name, Rect{tx + mw, ty, textRight, ty + 22.0f}, Font::BodyStrong, p.text);
        c.Text(cd.desc, Rect{tx + mw, ty + 24.0f, card.right - 12.0f, ty + 64.0f}, Font::Caption, p.textSecondary, HAlign::Left, true);
        if (removable) {
            const Rect rr = RemoveRect(i);
            const bool hot = hoverCard_ == i && hoverRemove_;
            if (hot) c.FillRect(rr, p.retro ? p.text : p.ctrlHover);
            c.Text(p.retro ? L"[ Remove ]" : L"Remove", rr, p.retro ? Font::BodyStrong : Font::Body,
                   hot && p.retro ? p.textOnAccent : p.error.icon, HAlign::Center);
        }
        if (focused && focusVisible && sel) DrawFocus(c, p, card, metric::kCardRadius, -3.0f);
    }
    c.PopClip();
    DrawScrollbar(c, p);
}

}  // namespace moonsr::app::ui
