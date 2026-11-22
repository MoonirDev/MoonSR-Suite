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
    const bool invert = enabled && (pressed || hover);
    if (invert) c.FillRect(box, checked_ ? p.accent : p.text);
    const D2D1_COLOR_F col = !enabled ? p.textDisabled : invert ? p.textOnAccent : checked_ ? p.accent : p.textSecondary;
    c.Text(label, box, Font::BodyStrong, col, HAlign::Center);
    DrawFocus(c, p, box, 2.0f);
}

float Button::RetroPreferredWidth(const Gfx& g) const {
    return std::max(64.0f, g.MeasureWidth(L"[ " + text + L" ]", Font::BodyStrong) + 16.0f);
}

void Button::DrawRetro(Canvas& c, const Palette& p) {
    const bool invert = enabled && (pressed || hover);
    if (invert) c.FillRect(bounds, accent ? p.accent : p.text);
    const D2D1_COLOR_F tc = !enabled ? p.textDisabled : invert ? p.textOnAccent : accent ? p.accent : p.text;
    c.Text(L"[ " + text + L" ]", Rect{bounds.left + 4, bounds.top, bounds.right - 4, bounds.bottom}, Font::BodyStrong, tc,
           HAlign::Center);
    DrawFocus(c, p, bounds, 0.0f, 0.0f);
}

void ComboBox::DrawRetro(Canvas& c, const Palette& p) {
    const D2D1_COLOR_F fill = !enabled ? p.ctrlDisabled : (pressed || open_) ? p.ctrlPressed : hover ? p.ctrlHover : p.ctrlFill;
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
    const float t = max_ > min_ ? static_cast<float>(value_ - min_) / static_cast<float>(max_ - min_) : 0.0f;
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
    if (focused && !HasSelection() && enabled) {
        // Block caret: one character cell, translucent so the character under it stays readable.
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
    c.Text(L"Game upscaling and frame generation.", Rect{x, bounds.top + 70.0f, bounds.right - 16.0f, bounds.top + 92.0f}, Font::Body,
           p.textSecondary);
    const std::wstring prompt = L"C:\\MoonSR> ";
    const float pw = c.gfx().MeasureWidth(prompt, Font::Body);
    c.Text(prompt, Rect{x, bounds.top + 96.0f, x + pw + 4.0f, bounds.top + 120.0f}, Font::Body, p.text);
    c.FillRect(Rect{x + pw, bounds.top + 101.0f, x + pw + 8.0f, bounds.top + 115.0f}, p.accent);
}

void NavItem::DrawRetro(Canvas& c, const Palette& p) {
    if (selected) c.FillRect(bounds, p.accent);
    else if (pressed) c.FillRect(bounds, p.subtlePressed);
    else if (hover) c.FillRect(bounds, p.subtleHover);
    const D2D1_COLOR_F tc = selected ? p.textOnAccent : p.text;
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
}  // namespace

float ThemePicker::HeightForWidth(const Gfx&, float) const {
    const float n = static_cast<float>(std::max<size_t>(1, cards_.size()));
    return n * kCardH + (n - 1.0f) * kGap;
}

Rect ThemePicker::CardRect(int i) const {
    const float t = bounds.top + static_cast<float>(i) * (kCardH + kGap);
    return Rect{bounds.left, t, bounds.right, t + kCardH};
}

Rect ThemePicker::RemoveRect(int i) const {
    const Rect c = CardRect(i);
    return Rect{c.right - 112.0f, c.top + 10.0f, c.right - 10.0f, c.top + 38.0f};
}

int ThemePicker::CardAt(float x, float y) const {
    for (int i = 0; i < static_cast<int>(cards_.size()); ++i)
        if (RectHas(CardRect(i), x, y)) return i;
    return -1;
}

void ThemePicker::SetCards(std::vector<Card> cards) {
    cards_ = std::move(cards);
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
    Invalidate();
}

void ThemePicker::Select(int i, bool notify) {
    if (i < 0 || i >= static_cast<int>(cards_.size()) || cards_[static_cast<size_t>(i)].key == sel_) return;
    sel_ = cards_[static_cast<size_t>(i)].key;
    Invalidate();
    if (notify && onChange) onChange();
}

void ThemePicker::OnPress(float x, float y) {
    pressCard_ = CardAt(x, y);
    pressRemove_ = pressCard_ >= 0 && !cards_[static_cast<size_t>(pressCard_)].builtin && RectHas(RemoveRect(pressCard_), x, y);
}
void ThemePicker::OnRelease(float x, float y, bool) {
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
    const int i = CardAt(x, y);
    const bool r = i >= 0 && !cards_[static_cast<size_t>(i)].builtin && RectHas(RemoveRect(i), x, y);
    if (i != hoverCard_ || r != hoverRemove_) {
        hoverCard_ = i;
        hoverRemove_ = r;
        Invalidate();
    }
}
bool ThemePicker::OnKey(UINT vk) {
    const int i = SelectedIndex();
    if (vk == VK_LEFT || vk == VK_UP) { Select(i - 1, true); return true; }
    if (vk == VK_RIGHT || vk == VK_DOWN) { Select(i + 1, true); return true; }
    if (vk == VK_DELETE && i >= 0 && !cards_[static_cast<size_t>(i)].builtin && onRemove) {
        onRemove(cards_[static_cast<size_t>(i)].key);
        return true;
    }
    return false;
}
void ThemePicker::Activate() {}

void ThemePicker::DrawMiniature(Canvas& c, const Rect& r, const Palette& mp) {
    const float wasCap = c.radiusCap(), wasBw = c.borderW();
    c.SetStyle(mp.radius, 1.0f);  // corner style of the previewed theme
    c.PushClip(r);
    if (mp.retro) c.FillRect(r, mp.bg);
    else c.FillGradient(r, 0.0f, {{0.0f, mp.bgA}, {0.5f, mp.bgB}, {1.0f, mp.bgC}}, r.left, r.top, r.right, r.bottom);
    // Navigation pane with three items, the first selected.
    const float navW = RW(r) * 0.27f;
    c.FillRect(Rect{r.left + navW, r.top, r.left + navW + 1.0f, r.bottom}, mp.divider);
    for (int i = 0; i < 3; ++i) {
        const Rect it{r.left + 6.0f, r.top + 22.0f + static_cast<float>(i) * 18.0f, r.left + navW - 6.0f, r.top + 36.0f + static_cast<float>(i) * 18.0f};
        if (i == 0) {
            c.FillRound(it, 3.0f, mp.retro ? mp.accent : mp.subtleSelected);
            if (!mp.retro) c.FillRound(Rect{it.left, it.top + 3.0f, it.left + 2.0f, it.bottom - 3.0f}, 1.0f, mp.accent);
        }
        const float tw = (RW(it) - 14.0f) * (i == 1 ? 0.7f : 0.9f);
        c.FillRect(Rect{it.left + 6.0f, (it.top + it.bottom) * 0.5f - 1.0f, it.left + 6.0f + tw, (it.top + it.bottom) * 0.5f + 1.0f},
                   i == 0 && mp.retro ? mp.textOnAccent : mp.text);
    }
    c.FillRect(Rect{r.left + 8.0f, r.top + 6.0f, r.left + navW - 8.0f, r.top + 12.0f}, mp.retro ? mp.accent : mp.brandSR);
    // Header and a card with a toggle row and a slider row.
    const float cx = r.left + navW + 10.0f;
    c.FillRect(Rect{cx, r.top + 8.0f, cx + 44.0f, r.top + 12.0f}, mp.text);
    const Rect card{cx, r.top + 22.0f, r.right - 10.0f, r.bottom - 10.0f};
    c.FillRound(card, 4.0f, mp.cardFill);
    c.StrokeRound(card, 4.0f, mp.cardStroke);
    const float row1 = card.top + RH(card) * 0.28f, row2 = card.top + RH(card) * 0.72f;
    c.FillRect(Rect{card.left + 8.0f, row1 - 1.0f, card.left + 44.0f, row1 + 1.0f}, mp.textSecondary);
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
    c.PopClip();
    c.StrokeRound(r, 4.0f, mp.cardStroke);
    c.SetStyle(wasCap, wasBw);
}

void ThemePicker::Draw(Canvas& c, const Palette& p) {
    for (int i = 0; i < static_cast<int>(cards_.size()); ++i) {
        const Card& cd = cards_[static_cast<size_t>(i)];
        const Rect card = CardRect(i);
        const bool sel = cd.key == sel_;
        c.FillRound(card, metric::kCardRadius, hoverCard_ == i && !sel ? p.ctrlHover : p.cardFill);
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
}

}  // namespace moonsr::app::ui
