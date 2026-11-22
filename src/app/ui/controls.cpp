#include "controls.h"

#include "view.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace moonsr::app::ui {

namespace {
D2D1_COLOR_F Fade(const D2D1_COLOR_F& c, float a) { return D2D1_COLOR_F{c.r, c.g, c.b, c.a * a}; }
}  // namespace

// ---------------------------------------------------------------------------------------------------------------------
// Control
// ---------------------------------------------------------------------------------------------------------------------

void Control::Invalidate() {
    if (view) view->Invalidate();
}

void Control::SetEnabled(bool e) {
    if (enabled == e) return;
    enabled = e;
    if (!e) {
        hover = false;
        pressed = false;
    }
    if (view) view->OnControlEnabledChanged(this);
    Invalidate();
}

void Control::SetVisible(bool v) {
    if (visible == v) return;
    visible = v;
    if (view) view->RequestLayout();
    Invalidate();
}

void Control::DrawFocus(Canvas& c, const Palette& p, const Rect& r, float radius, float outset) const {
    if (!focused || !focusVisible) return;
    c.StrokeRound(Inflate(r, outset), radius + outset, p.focus, 2.0f);
}

// ---------------------------------------------------------------------------------------------------------------------
// ToggleSwitch
// ---------------------------------------------------------------------------------------------------------------------

void ToggleSwitch::SetChecked(bool v, bool animate) {
    if (checked_ == v) {
        if (!animate) t_ = v ? 1.0f : 0.0f;
        return;
    }
    checked_ = v;
    if (animate && view) view->RequestAnimation();
    else t_ = v ? 1.0f : 0.0f;
    Invalidate();
}

void ToggleSwitch::Toggle() {
    if (!enabled) return;
    SetChecked(!checked_, true);
    if (onChange) onChange();
}

void ToggleSwitch::OnRelease(float, float, bool inside) {
    if (inside) Toggle();
}

void ToggleSwitch::Activate() { Toggle(); }

bool ToggleSwitch::Tick(float dt) {
    const float target = checked_ ? 1.0f : 0.0f;
    if (t_ == target) return false;
    const float step = dt / 0.14f;
    t_ = t_ < target ? std::min(target, t_ + step) : std::max(target, t_ - step);
    return t_ != target;
}

void ToggleSwitch::Draw(Canvas& c, const Palette& p) {
    const float cy = (bounds.top + bounds.bottom) * 0.5f;
    const Rect sw{bounds.right - 40.0f, cy - 10.0f, bounds.right, cy + 10.0f};
    const std::wstring label = checked_ ? L"On" : L"Off";
    c.Text(label, Rect{bounds.left, bounds.top, sw.left - 12.0f, bounds.bottom}, Font::Body,
           enabled ? p.text : p.textDisabled, HAlign::Right);

    const D2D1_COLOR_F onFill = !enabled ? p.accentDisabled : pressed ? p.accentPressed : hover ? p.accentHover : p.accent;
    const D2D1_COLOR_F offFill = !enabled ? p.ctrlDisabled : pressed ? p.ctrlPressed : hover ? p.ctrlHover : p.ctrlFill;
    c.FillRound(sw, 10.0f, t_ >= 1.0f ? onFill : t_ <= 0.0f ? offFill : Mix(offFill, onFill, t_));
    if (t_ < 1.0f) c.StrokeRound(sw, 10.0f, Fade(enabled ? p.strong : p.textDisabled, 1.0f - t_), 1.0f);

    const float r = pressed ? 7.5f : hover ? 7.0f : 6.0f;
    const float kx = sw.left + 10.0f + (sw.right - sw.left - 20.0f) * t_;
    const D2D1_COLOR_F offKnob = enabled ? p.strong : p.textDisabled;
    const D2D1_COLOR_F onKnob = enabled ? p.textOnAccent : p.textDisabled;
    c.FillCircle(kx, cy, r, Mix(offKnob, onKnob, t_));
    DrawFocus(c, p, sw, 10.0f);
}

// ---------------------------------------------------------------------------------------------------------------------
// Button / LinkButton
// ---------------------------------------------------------------------------------------------------------------------

void Button::SetText(const std::wstring& t) {
    if (text == t) return;
    text = t;
    Invalidate();
}

float Button::PreferredWidth(const Gfx& g) const { return std::max(88.0f, g.MeasureWidth(text, Font::Body) + 36.0f); }

void Button::Draw(Canvas& c, const Palette& p) {
    D2D1_COLOR_F fill, textColor;
    if (accent) {
        fill = !enabled ? p.accentDisabled : pressed ? p.accentPressed : hover ? p.accentHover : p.accent;
        textColor = enabled ? p.textOnAccent : p.textDisabled;
    } else {
        fill = !enabled ? p.ctrlDisabled : pressed ? p.ctrlPressed : hover ? p.ctrlHover : p.ctrlFill;
        textColor = !enabled ? p.textDisabled : pressed ? p.textSecondary : p.text;
    }
    c.FillRound(bounds, metric::kControlRadius, fill);
    if (!accent) c.StrokeRound(bounds, metric::kControlRadius, p.ctrlStroke);
    c.Text(text, Rect{bounds.left + 8, bounds.top, bounds.right - 8, bounds.bottom}, accent ? Font::BodyStrong : Font::Body,
           textColor, HAlign::Center);
    DrawFocus(c, p, bounds, metric::kControlRadius);
}

void Button::OnRelease(float, float, bool inside) {
    if (inside && enabled && onChange) onChange();
}

void Button::Activate() {
    if (enabled && onChange) onChange();
}

float LinkButton::PreferredWidth(const Gfx& g) const { return g.MeasureWidth(text, Font::Body) + 8.0f; }

void LinkButton::Draw(Canvas& c, const Palette& p) {
    const D2D1_COLOR_F col = !enabled ? p.textDisabled : pressed ? p.textSecondary : p.accent;
    c.Text(text, bounds, Font::Body, col, HAlign::Left);
    if (hover || (focused && focusVisible)) {
        const float w = c.gfx().MeasureWidth(text, Font::Body);
        const float cy = (bounds.top + bounds.bottom) * 0.5f + 9.0f;
        c.Line(bounds.left, cy, bounds.left + w, cy, col, 1.0f);
    }
    DrawFocus(c, p, bounds, 4.0f);
}

void LinkButton::OnRelease(float, float, bool inside) {
    if (inside && enabled && onChange) onChange();
}

void LinkButton::Activate() {
    if (enabled && onChange) onChange();
}

// ---------------------------------------------------------------------------------------------------------------------
// ComboBox
// ---------------------------------------------------------------------------------------------------------------------

void ComboBox::Clear() {
    items_.clear();
    selected_ = -1;
    Invalidate();
}

int ComboBox::AddItem(const std::wstring& text, long long data) {
    items_.push_back(Item{text, data});
    if (selected_ < 0) selected_ = 0;
    Invalidate();
    return static_cast<int>(items_.size()) - 1;
}

void ComboBox::SetSelected(int index, bool notify) {
    if (index < 0 || index >= Count()) index = Count() > 0 ? 0 : -1;
    if (index == selected_) return;
    selected_ = index;
    Invalidate();
    if (notify && onChange) onChange();
}

long long ComboBox::ItemData(int index) const {
    return index >= 0 && index < Count() ? items_[static_cast<size_t>(index)].data : -1;
}

void ComboBox::Activate() {
    if (enabled && view) view->OpenDropdown(this);
}

void ComboBox::OnRelease(float, float, bool inside) {
    if (inside) Activate();
}

bool ComboBox::OnKey(UINT vk) {
    if (!enabled || Count() == 0) return false;
    switch (vk) {
    case VK_UP:
    case VK_LEFT:
        SetSelected(std::max(0, selected_ - 1), true);
        return true;
    case VK_DOWN:
    case VK_RIGHT:
        SetSelected(std::min(Count() - 1, selected_ + 1), true);
        return true;
    case VK_HOME:
        SetSelected(0, true);
        return true;
    case VK_END:
        SetSelected(Count() - 1, true);
        return true;
    case VK_F4:
        Activate();
        return true;
    default:
        return false;
    }
}

void ComboBox::Draw(Canvas& c, const Palette& p) {
    const D2D1_COLOR_F fill = !enabled ? p.ctrlDisabled : (pressed || open_) ? p.ctrlPressed : hover ? p.ctrlHover : p.ctrlFill;
    c.FillRound(bounds, metric::kControlRadius, fill);
    c.StrokeRound(bounds, metric::kControlRadius, p.ctrlStroke);
    const D2D1_COLOR_F tc = enabled ? p.text : p.textDisabled;
    c.Text(selected_ >= 0 ? items_[static_cast<size_t>(selected_)].text : std::wstring(),
           Rect{bounds.left + 12, bounds.top, bounds.right - 34, bounds.bottom}, Font::Body, tc, HAlign::Left);
    c.Chevron(bounds.right - 18.0f, (bounds.top + bounds.bottom) * 0.5f, 4.5f, enabled ? p.textSecondary : p.textDisabled);
    DrawFocus(c, p, bounds, metric::kControlRadius);
}

// ---------------------------------------------------------------------------------------------------------------------
// Slider
// ---------------------------------------------------------------------------------------------------------------------

void Slider::Setup(int minV, int maxV, int step, const wchar_t* suffix) {
    min_ = minV;
    max_ = maxV;
    step_ = std::max(1, step);
    suffix_ = suffix ? suffix : L"";
    value_ = std::clamp(value_, min_, max_);
}

void Slider::SetValue(int v, bool notify) {
    v = std::clamp(min_ + ((v - min_ + step_ / 2) / step_) * step_, min_, max_);
    if (v == value_) return;
    value_ = v;
    Invalidate();
    if (notify && onChange) onChange();
}

void Slider::SetFromX(float x) {
    const float l = TrackLeft(), r = TrackRight();
    const float t = r > l ? std::clamp((x - l) / (r - l), 0.0f, 1.0f) : 0.0f;
    SetValue(min_ + static_cast<int>(std::lround(t * static_cast<float>(max_ - min_))), true);
}

void Slider::OnPress(float x, float) { SetFromX(x); }
void Slider::OnDrag(float x, float) { SetFromX(x); }

bool Slider::OnKey(UINT vk) {
    if (!enabled) return false;
    switch (vk) {
    case VK_LEFT:
    case VK_DOWN:
        SetValue(value_ - step_, true);
        return true;
    case VK_RIGHT:
    case VK_UP:
        SetValue(value_ + step_, true);
        return true;
    case VK_PRIOR:
        SetValue(value_ + step_ * 4, true);
        return true;
    case VK_NEXT:
        SetValue(value_ - step_ * 4, true);
        return true;
    case VK_HOME:
        SetValue(min_, true);
        return true;
    case VK_END:
        SetValue(max_, true);
        return true;
    default:
        return false;
    }
}

void Slider::Draw(Canvas& c, const Palette& p) {
    const float cy = (bounds.top + bounds.bottom) * 0.5f;
    const float l = TrackLeft(), r = TrackRight();
    const float t = max_ > min_ ? static_cast<float>(value_ - min_) / static_cast<float>(max_ - min_) : 0.0f;
    const float tx = l + (r - l) * t;
    c.FillRound(Rect{l, cy - 2.0f, r, cy + 2.0f}, 2.0f, enabled ? Fade(p.strong, 0.6f) : p.textDisabled);
    if (tx > l) c.FillRound(Rect{l, cy - 2.0f, tx, cy + 2.0f}, 2.0f, enabled ? p.accent : p.accentDisabled);
    c.FillCircle(tx, cy, 10.0f, enabled ? p.thumb : p.ctrlDisabled);
    c.StrokeCircle(tx, cy, 10.0f, p.ctrlStroke);
    const float dot = !enabled ? 5.0f : pressed ? 5.0f : hover ? 7.0f : 6.0f;
    c.FillCircle(tx, cy, dot, enabled ? p.accent : p.accentDisabled);
    const std::wstring label = displayDivisor_ > 1 ? std::format(L"{:.2f}{}", static_cast<double>(value_) / displayDivisor_, suffix_)
                                                  : std::format(L"{}{}", value_, suffix_);
    c.Text(label, Rect{r + 14.0f, bounds.top, bounds.right, bounds.bottom}, Font::Body,
           enabled ? p.textSecondary : p.textDisabled, HAlign::Right);
    DrawFocus(c, p, Rect{tx - 10.0f, cy - 10.0f, tx + 10.0f, cy + 10.0f}, 10.0f);
}

// ---------------------------------------------------------------------------------------------------------------------
// InfoBar
// ---------------------------------------------------------------------------------------------------------------------

void InfoBar::Set(Severity s, const std::wstring& message) {
    if (severity_ == s && message_ == message) return;
    const bool relayout = message_ != message;
    severity_ = s;
    message_ = message;
    if (view && relayout) view->RequestLayout();
    Invalidate();
}

float InfoBar::Measure(const Gfx& g, float width) const {
    const float textH = g.MeasureHeight(message_, Font::Body, width - 16.0f - 20.0f - 16.0f - 16.0f);
    return std::max(48.0f, textH + 24.0f);
}

void InfoBar::Draw(Canvas& c, const Palette& p) {
    const InfoColors& col = severity_ == Severity::Success   ? p.success
                            : severity_ == Severity::Warning ? p.warning
                            : severity_ == Severity::Error   ? p.error
                                                             : p.info;
    c.FillRound(bounds, metric::kCardRadius, col.fill);
    c.StrokeRound(bounds, metric::kCardRadius, p.cardStroke);
    const float cy = bounds.top + (RH(bounds) <= 52.0f ? RH(bounds) * 0.5f : 24.0f);
    const float cx = bounds.left + 16.0f + 8.0f;
    c.FillCircle(cx, cy, 9.0f, col.icon);
    const wchar_t* glyph = severity_ == Severity::Success ? L"" : severity_ == Severity::Warning ? L"!"
                          : severity_ == Severity::Error ? L"x" : L"i";
    if (severity_ == Severity::Success) c.Check(cx, cy - 0.5f, 9.0f, col.iconText);
    else c.Text(glyph, Rect{cx - 9.0f, cy - 9.0f, cx + 9.0f, cy + 9.0f}, Font::Glyph, col.iconText, HAlign::Center);
    c.Text(message_, Rect{bounds.left + 52.0f, bounds.top + 12.0f, bounds.right - 16.0f, bounds.bottom - 12.0f}, Font::Body,
           p.text, HAlign::Left, true);
}

// ---------------------------------------------------------------------------------------------------------------------
// Tile / TextLabel
// ---------------------------------------------------------------------------------------------------------------------

void Tile::SetValue(const std::wstring& value, const std::wstring& note) {
    if (value_ == value && note_ == note) return;
    value_ = value;
    note_ = note;
    Invalidate();
}

void Tile::Draw(Canvas& c, const Palette& p) {
    c.FillRound(bounds, metric::kCardRadius, p.cardFill);
    c.StrokeRound(bounds, metric::kCardRadius, p.cardStroke);
    const float x = bounds.left + 16.0f;
    const float w = RW(bounds) - 32.0f;
    c.Text(label, Rect{x, bounds.top + 10.0f, bounds.right - 12.0f, bounds.top + 30.0f}, Font::Caption, p.textSecondary);
    Font f = Font::Value;
    const Gfx& g = c.gfx();
    float vw = g.MeasureWidth(value_, f);
    if (vw > w) {
        f = Font::ValueSmall;
        vw = g.MeasureWidth(value_, f);
    }
    const Rect vr{x, bounds.top + 32.0f, x + std::min(vw + 2.0f, w), bounds.bottom - 10.0f};
    c.Text(value_, vr, f, p.text, HAlign::Left);
    if (!note_.empty()) {
        const float nx = vr.right + 8.0f;
        if (nx < bounds.right - 16.0f)
            c.Text(note_, Rect{nx, vr.top, bounds.right - 12.0f, vr.bottom}, Font::Caption, p.textSecondary);
    }
}

void TextLabel::SetText(const std::wstring& t) {
    if (text == t) return;
    text = t;
    Invalidate();
}

float TextLabel::PreferredWidth(const Gfx& g) const { return g.MeasureWidth(text, Font::Body) + (keycap ? 26.0f : 2.0f); }

void TextLabel::Draw(Canvas& c, const Palette& p) {
    if (keycap) {
        const float h = 28.0f;
        const float cy = (bounds.top + bounds.bottom) * 0.5f;
        const Rect r{bounds.left, cy - h * 0.5f, bounds.right, cy + h * 0.5f};
        c.FillRound(r, metric::kControlRadius, p.ctrlFill);
        c.StrokeRound(r, metric::kControlRadius, p.ctrlStroke);
        c.Text(text, r, Font::Body, enabled ? p.text : p.textDisabled, HAlign::Center);
    } else {
        c.Text(text, bounds, Font::Body, enabled ? p.textSecondary : p.textDisabled, HAlign::Right);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// HeroBanner
// ---------------------------------------------------------------------------------------------------------------------

float HeroBanner::ScaleFor(float width) { return std::clamp(width / 600.0f, 0.62f, 1.0f); }

float HeroBanner::HeightForWidth(const Gfx&, float width) const { return 176.0f * ScaleFor(width); }

void HeroBanner::Draw(Canvas& c, const Palette& p) {
    const float w = RW(bounds), h = RH(bounds), s = ScaleFor(w);
    const float radius = 16.0f;
    // Brand navy gradient (the same in both themes), diagonal like the mockup.
    c.FillGradient(bounds, radius, {{0.0f, Rgb(0x202B48)}, {0.5f, Rgb(0x111A31)}, {1.0f, Rgb(0x17142F)}}, bounds.left, bounds.top,
                   bounds.right, bounds.bottom);
    c.PushClip(bounds);
    const float markSize = 100.0f * s;
    const float mx = bounds.left + 40.0f * s, my = bounds.top + (h - markSize) * 0.5f - 2.0f * s;
    c.LogoMark(Rect{mx, my, mx + markSize, my + markSize}, true);
    const float tx = mx + markSize + 44.0f * s;
    const float wordSize = 54.0f * s;
    const float wordY = bounds.top + 30.0f * s;
    c.Wordmark(tx - 2.0f * s, wordY, wordSize, Rgb(0xFFFFFF), Rgb(0xCFE9FF), Rgb(0x73D8FF));
    const float tagSize = std::max(11.5f, 13.0f * s);
    const float tagY = wordY + wordSize * 1.36f + 2.0f * s;
    c.SpacedText(L"GAME UPSCALING AND MORE", tx + 2.0f * s, tagY, tagSize, DWRITE_FONT_WEIGHT_MEDIUM, Rgb(0xB4C6E5), 0.27f * tagSize);
    const float ruleY = tagY + tagSize * 1.45f + 8.0f * s;
    const Rect rule{tx + 2.0f * s, ruleY, tx + 2.0f * s + 130.0f * s, ruleY + 3.0f};
    c.FillGradient(rule, 1.5f, {{0.0f, Rgb(0x5CE5FF, 0.85f)}, {1.0f, Rgb(0x9584FF, 0.85f)}}, rule.left, 0.0f, rule.right, 0.0f);
    c.PopClip();
    c.StrokeRound(bounds, radius, p.dark ? Argb(0x2EB4C6E5) : Argb(0x33203B6B), 1.0f);
}
// ---------------------------------------------------------------------------------------------------------------------
// NavItem
// ---------------------------------------------------------------------------------------------------------------------

void NavItem::Activate() {
    if (view) view->Navigate(index);
}

void NavItem::OnRelease(float, float, bool inside) {
    if (inside) Activate();
}

bool NavItem::OnKey(UINT vk) {
    if (!view) return false;
    if (vk == VK_DOWN) {
        view->MoveNav(1);
        return true;
    }
    if (vk == VK_UP) {
        view->MoveNav(-1);
        return true;
    }
    return false;
}

void NavItem::Draw(Canvas& c, const Palette& p) {
    const D2D1_COLOR_F fill = selected ? (pressed ? p.subtlePressed : p.subtleSelected)
                              : pressed ? p.subtlePressed
                              : hover   ? p.subtleHover
                                        : D2D1_COLOR_F{0, 0, 0, 0};
    if (fill.a > 0.0f) c.FillRound(bounds, metric::kControlRadius, fill);
    const float cy = (bounds.top + bounds.bottom) * 0.5f;
    if (selected)  // brand gradient indicator (the one place the cyan-violet gradient is used in the chrome)
        c.FillGradient(Rect{bounds.left, cy - 9.0f, bounds.left + 3.0f, cy + 9.0f}, 1.5f, {{0.0f, p.gradA}, {1.0f, p.gradB}}, 0.0f,
                       cy - 9.0f, 0.0f, cy + 9.0f);
    const float iconCx = compact ? (bounds.left + bounds.right) * 0.5f : bounds.left + 24.0f;
    c.Icon(icon, Rect{iconCx - 12.0f, cy - 12.0f, iconCx + 12.0f, cy + 12.0f}, Font::Icon, p.text);
    if (!compact)
        c.Text(label, Rect{bounds.left + (c.gfx().hasIcons() ? 48.0f : 16.0f), bounds.top, bounds.right - 8.0f, bounds.bottom},
               Font::Body, p.text, HAlign::Left);
    DrawFocus(c, p, bounds, metric::kControlRadius, 0.0f);
}

}  // namespace moonsr::app::ui
