// NumberBox: single-line integer text entry (see controls.h). Threading: UI thread only.
#include "controls.h"

#include "view.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace moonsr::app::ui {

namespace {
D2D1_COLOR_F Fade(const D2D1_COLOR_F& c, float a) { return D2D1_COLOR_F{c.r, c.g, c.b, c.a * a}; }
}  // namespace

void NumberBox::Setup(int minV, int maxV, const wchar_t* suffix) {
    min_ = minV;
    max_ = std::max(minV, maxV);
    suffix_ = suffix ? suffix : L"";
    SetValue(std::clamp(value_, min_, max_));
}

void NumberBox::SetValue(int v, bool notify) {
    v = std::clamp(v, min_, max_);
    clamped_ = false;
    const bool changed = v != value_;
    value_ = v;
    typed_ = v;
    text_ = std::to_wstring(v);
    caret_ = anchor_ = static_cast<int>(text_.size());
    Invalidate();
    if (changed && notify && onChange) onChange();
}

void NumberBox::Commit(bool notify) {
    clamped_ = false;
    if (text_.empty()) {  // nothing typed: keep the previous value
        text_ = std::to_wstring(value_);
        caret_ = anchor_ = static_cast<int>(text_.size());
        Invalidate();
        return;
    }
    long long n = 0;
    for (wchar_t ch : text_) n = std::min<long long>(n * 10 + (ch - L'0'), 1000000000LL);
    typed_ = static_cast<int>(n);
    const int v = static_cast<int>(std::clamp<long long>(n, min_, max_));
    clamped_ = v != n;
    const bool changed = v != value_;
    value_ = v;
    text_ = std::to_wstring(v);
    caret_ = anchor_ = static_cast<int>(text_.size());
    Invalidate();
    if (notify && onChange && (changed || clamped_)) onChange();
}

void NumberBox::Replace(const std::wstring& s) {
    std::wstring digits;
    for (wchar_t ch : s)
        if (ch >= L'0' && ch <= L'9') digits += ch;
    const int lo = std::min(caret_, anchor_), hi = std::max(caret_, anchor_);
    const std::wstring t = text_.substr(0, static_cast<size_t>(lo)) + digits + text_.substr(static_cast<size_t>(hi));
    if (t.size() > 7) return;  // far beyond any valid value
    text_ = t;
    caret_ = anchor_ = lo + static_cast<int>(digits.size());
    clamped_ = false;
    Invalidate();
}

float NumberBox::XOf(int index) const {
    const float x0 = bounds.left + 12.0f;
    if (!gfx_ || index <= 0) return x0;
    return x0 + gfx_->MeasureWidth(text_.substr(0, static_cast<size_t>(index)), Font::Body);
}

int NumberBox::CaretFromX(float x) const {
    int best = 0;
    float bestD = 1e9f;
    for (int i = 0; i <= static_cast<int>(text_.size()); ++i) {
        const float d = std::fabs(XOf(i) - x);
        if (d < bestD) {
            bestD = d;
            best = i;
        }
    }
    return best;
}

void NumberBox::OnPress(float x, float) {
    caret_ = anchor_ = CaretFromX(bounds.left + x);
    Invalidate();
}

void NumberBox::OnDrag(float x, float) {
    caret_ = CaretFromX(bounds.left + x);
    Invalidate();
}

void NumberBox::OnFocusChanged(bool gained, bool byKeyboard) {
    if (gained) {
        if (byKeyboard) {  // Tab into the box selects everything so typing replaces it
            anchor_ = 0;
            caret_ = static_cast<int>(text_.size());
        }
    } else {
        Commit(true);
    }
    Invalidate();
}

bool NumberBox::OnChar(wchar_t ch) {
    if (!enabled) return false;
    if (ch >= L'0' && ch <= L'9') {
        Replace(std::wstring(1, ch));
        return true;
    }
    return ch >= 32;  // other printable characters are swallowed
}

bool NumberBox::OnKey(UINT vk) {
    if (!enabled) return false;
    const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    const int n = static_cast<int>(text_.size());
    auto move = [&](int to) {
        caret_ = std::clamp(to, 0, n);
        if (!shift) anchor_ = caret_;
        Invalidate();
    };
    if (ctrl) {
        switch (vk) {
        case 'A':
            anchor_ = 0;
            caret_ = n;
            Invalidate();
            return true;
        case 'C':
        case 'X':
            if (HasSelection() && OpenClipboard(nullptr)) {
                const int lo = std::min(caret_, anchor_), hi = std::max(caret_, anchor_);
                const std::wstring sel = text_.substr(static_cast<size_t>(lo), static_cast<size_t>(hi - lo));
                EmptyClipboard();
                if (HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (sel.size() + 1) * sizeof(wchar_t))) {
                    if (void* m = GlobalLock(h)) {
                        memcpy(m, sel.c_str(), (sel.size() + 1) * sizeof(wchar_t));
                        GlobalUnlock(h);
                        SetClipboardData(CF_UNICODETEXT, h);
                    } else {
                        GlobalFree(h);
                    }
                }
                CloseClipboard();
                if (vk == 'X') Replace(L"");
            }
            return true;
        case 'V':
            if (OpenClipboard(nullptr)) {
                if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
                    if (const wchar_t* m = static_cast<const wchar_t*>(GlobalLock(h))) {
                        const std::wstring s = m;
                        GlobalUnlock(h);
                        Replace(s);
                    }
                }
                CloseClipboard();
            }
            return true;
        default:
            break;
        }
    }
    switch (vk) {
    case VK_LEFT:
        if (!shift && HasSelection()) move(std::min(caret_, anchor_));
        else move(caret_ - 1);
        return true;
    case VK_RIGHT:
        if (!shift && HasSelection()) move(std::max(caret_, anchor_));
        else move(caret_ + 1);
        return true;
    case VK_HOME:
        move(0);
        return true;
    case VK_END:
        move(n);
        return true;
    case VK_BACK:
        if (!HasSelection() && caret_ > 0) anchor_ = caret_ - 1;
        Replace(L"");
        return true;
    case VK_DELETE:
        if (!HasSelection() && caret_ < n) anchor_ = caret_ + 1;
        Replace(L"");
        return true;
    case VK_UP:
    case VK_DOWN: {
        Commit(false);
        const int before = value_;
        SetValue(value_ + (vk == VK_UP ? 1 : -1));
        if (value_ != before && onChange) onChange();
        anchor_ = 0;
        caret_ = static_cast<int>(text_.size());
        return true;
    }
    default:
        return false;
    }
}

void NumberBox::Draw(Canvas& c, const Palette& p) {
    if (p.retro) {
        DrawRetro(c, p);
        return;
    }
    gfx_ = &c.gfx();
    const D2D1_COLOR_F fill = !enabled ? p.ctrlDisabled : focused ? p.cardFill : hover ? p.ctrlHover : p.ctrlFill;
    c.FillRound(bounds, metric::kControlRadius, fill);
    const D2D1_COLOR_F stroke = clamped_ ? p.error.icon : focused ? p.accent : p.ctrlStroke;
    c.StrokeRound(bounds, metric::kControlRadius, stroke, (focused || clamped_) ? 2.0f : 1.0f);
    const D2D1_COLOR_F tc = enabled ? p.text : p.textDisabled;
    const Rect inner{bounds.left + 12.0f, bounds.top, bounds.right - 12.0f, bounds.bottom};
    const float cy = (bounds.top + bounds.bottom) * 0.5f;
    if (!suffix_.empty())
        c.Text(suffix_, inner, Font::Body, enabled ? p.textSecondary : p.textDisabled, HAlign::Right);
    if (focused && HasSelection()) {
        const float a = XOf(std::min(caret_, anchor_)), b = XOf(std::max(caret_, anchor_));
        c.FillRect(Rect{a, cy - 9.0f, b, cy + 9.0f}, Fade(p.accent, 0.35f));
    }
    c.Text(text_, inner, Font::Body, tc, HAlign::Left);
    if (focused && !HasSelection() && enabled) {
        const float x = XOf(caret_);
        c.Line(x, cy - 9.0f, x, cy + 9.0f, p.text, 1.0f);
    }
    DrawFocus(c, p, bounds, metric::kControlRadius);
}

}  // namespace moonsr::app::ui
