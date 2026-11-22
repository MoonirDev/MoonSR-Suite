#include "popup.h"

#include "core/log.h"

#include <dwmapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>

namespace moonsr::app::ui {
namespace {

constexpr wchar_t kPopupClass[] = L"MoonSR.Dropdown";
constexpr float kItemHeight = 34.0f;
constexpr float kPad = 5.0f;
constexpr int kMaxVisible = 9;

#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif

}  // namespace

DropdownPopup::~DropdownPopup() {
    if (hwnd_) {
        ReleaseCapture();
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

bool DropdownPopup::RegisterWindowClass(HINSTANCE instance) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_DROPSHADOW;
    wc.lpfnWndProc = &DropdownPopup::WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kPopupClass;
    return RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

void DropdownPopup::SetPalette(const Palette& p) {
    pal_ = p;
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

float DropdownPopup::MaxScroll() const {
    const float content = static_cast<float>(items_.size()) * kItemHeight + 2.0f * kPad;
    return std::max(0.0f, content - heightDip_);
}

bool DropdownPopup::Open(HWND owner, const std::vector<std::wstring>& items, int selected, const RECT& anchor,
                         float minWidthDip, UINT dpi, const Palette& palette) {
    if (hwnd_) Close(false);
    if (items.empty()) return false;
    items_ = items;
    selected_ = selected;
    hover_ = selected;
    dpi_ = dpi;
    pal_ = palette;
    dragging_ = false;

    const float scale = Scale();
    float contentW = 0.0f;
    for (const auto& s : items_) contentW = std::max(contentW, gfx_.MeasureWidth(s, Font::Body));
    widthDip_ = std::max(minWidthDip, contentW + 2.0f * kPad + 12.0f + 12.0f + 8.0f);
    const int visible = std::min(static_cast<int>(items_.size()), kMaxVisible);
    heightDip_ = static_cast<float>(visible) * kItemHeight + 2.0f * kPad;

    int wpx = static_cast<int>(std::ceil(widthDip_ * scale));
    int hpx = static_cast<int>(std::ceil(heightDip_ * scale));

    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromRect(&anchor, MONITOR_DEFAULTTONEAREST), &mi);
    const RECT work = mi.rcWork;
    wpx = std::min<int>(wpx, work.right - work.left - 8);
    widthDip_ = static_cast<float>(wpx) / scale;
    int x = anchor.left;
    if (x + wpx > work.right - 4) x = work.right - 4 - wpx;
    x = std::max<int>(x, work.left + 4);
    const int gap = static_cast<int>(std::lround(2.0f * scale));
    int y = anchor.bottom + gap;
    if (y + hpx > work.bottom - 4) {
        const int above = anchor.top - gap - hpx;
        if (above >= work.top + 4) {
            y = above;
        } else {
            hpx = std::max<int>(static_cast<int>(kItemHeight * scale) + 8, work.bottom - 4 - y);
            heightDip_ = static_cast<float>(hpx) / scale;
        }
    }

    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST, kPopupClass, L"", WS_POPUP, x, y, wpx, hpx,
                            owner, nullptr, GetModuleHandleW(nullptr), this);
    if (!hwnd_) {
        MSR_LOG_ERROR(L"Dropdown window creation failed ({})", GetLastError());
        return false;
    }
    const DWORD round = 2;  // DWMWCP_ROUND
    DwmSetWindowAttribute(hwnd_, DWMWA_WINDOW_CORNER_PREFERENCE, &round, sizeof(round));
    scroll_ = 0.0f;
    ScrollTo(selected_ >= 0 ? selected_ : 0);
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    UpdateWindow(hwnd_);
    SetCapture(hwnd_);
    return true;
}

void DropdownPopup::Close(bool commit) {
    if (!hwnd_) return;
    const int result = commit && hover_ >= 0 ? hover_ : -1;
    HWND h = hwnd_;
    hwnd_ = nullptr;
    canvas_.Release();
    if (GetCapture() == h) ReleaseCapture();
    DestroyWindow(h);
    if (onClosed) onClosed(result);
}

void DropdownPopup::ScrollTo(int index) {
    if (index < 0) return;
    const float top = kPad + static_cast<float>(index) * kItemHeight;
    const float bottom = top + kItemHeight;
    if (top - scroll_ < kPad) scroll_ = top - kPad;
    if (bottom - scroll_ > heightDip_ - kPad) scroll_ = bottom - (heightDip_ - kPad);
    scroll_ = std::clamp(scroll_, 0.0f, MaxScroll());
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

bool DropdownPopup::OnKey(UINT vk) {
    if (!hwnd_) return false;
    const int n = static_cast<int>(items_.size());
    int h = hover_ < 0 ? (selected_ >= 0 ? selected_ : 0) : hover_;
    switch (vk) {
    case VK_ESCAPE:
        Close(false);
        return true;
    case VK_RETURN:
    case VK_SPACE:
        Close(true);
        return true;
    case VK_TAB:
        Close(false);
        return false;
    case VK_UP:
        h = std::max(0, h - 1);
        break;
    case VK_DOWN:
        h = std::min(n - 1, h + 1);
        break;
    case VK_HOME:
        h = 0;
        break;
    case VK_END:
        h = n - 1;
        break;
    case VK_PRIOR:
        h = std::max(0, h - kMaxVisible + 1);
        break;
    case VK_NEXT:
        h = std::min(n - 1, h + kMaxVisible - 1);
        break;
    default:
        return true;  // swallow everything else while open
    }
    hover_ = h;
    ScrollTo(h);
    InvalidateRect(hwnd_, nullptr, FALSE);
    return true;
}

void DropdownPopup::OnWheel(int delta) {
    if (!hwnd_) return;
    scroll_ = std::clamp(scroll_ - static_cast<float>(delta) / 120.0f * kItemHeight * 2.0f, 0.0f, MaxScroll());
    InvalidateRect(hwnd_, nullptr, FALSE);
}

int DropdownPopup::ItemAt(float x, float y) const {
    if (x < 0 || x >= widthDip_ || y < kPad - 1 || y >= heightDip_ - kPad + 1) return -1;
    const int i = static_cast<int>(std::floor((y - kPad + scroll_) / kItemHeight));
    return i >= 0 && i < static_cast<int>(items_.size()) ? i : -1;
}

LRESULT CALLBACK DropdownPopup::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    DropdownPopup* self = nullptr;
    if (msg == WM_NCCREATE) {
        self = static_cast<DropdownPopup*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    } else {
        self = reinterpret_cast<DropdownPopup*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (!self || self->hwnd_ != hwnd) return DefWindowProcW(hwnd, msg, wp, lp);
    return self->OnMessage(msg, wp, lp);
}

LRESULT DropdownPopup::OnMessage(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd_, &ps);
        Paint();
        EndPaint(hwnd_, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        const float s = Scale();
        const int i = ItemAt(static_cast<float>(GET_X_LPARAM(lp)) / s, static_cast<float>(GET_Y_LPARAM(lp)) / s);
        if (i >= 0 && i != hover_) {
            hover_ = i;
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return 0;
    }
    case WM_LBUTTONDOWN: {
        const float s = Scale();
        const float x = static_cast<float>(GET_X_LPARAM(lp)) / s, y = static_cast<float>(GET_Y_LPARAM(lp)) / s;
        if (x < 0 || y < 0 || x >= widthDip_ || y >= heightDip_) {
            Close(false);  // outside click
        } else {
            const int i = ItemAt(x, y);
            if (i >= 0) hover_ = i;
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        const float s = Scale();
        const int i = ItemAt(static_cast<float>(GET_X_LPARAM(lp)) / s, static_cast<float>(GET_Y_LPARAM(lp)) / s);
        if (i >= 0) {
            hover_ = i;
            Close(true);
        }
        return 0;
    }
    case WM_MOUSEWHEEL:
        OnWheel(GET_WHEEL_DELTA_WPARAM(wp));
        return 0;
    case WM_CAPTURECHANGED:
        if (hwnd_ && reinterpret_cast<HWND>(lp) != hwnd_) PostMessageW(hwnd_, WM_APP + 7, 0, 0);
        return 0;
    case WM_APP + 7:
        if (hwnd_ && GetCapture() != hwnd_) Close(false);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

void DropdownPopup::Paint() {
    if (!canvas_.Begin(hwnd_, dpi_, false)) return;
    canvas_.Clear(pal_.popupFill);
    canvas_.PushClip(Rect{0, kPad - 1.0f, widthDip_, heightDip_ - kPad + 1.0f});
    for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
        const float top = kPad + static_cast<float>(i) * kItemHeight - scroll_;
        if (top + kItemHeight < 0 || top > heightDip_) continue;
        const Rect r{kPad, top + 1.0f, widthDip_ - kPad - (MaxScroll() > 0 ? 6.0f : 0.0f), top + kItemHeight - 1.0f};
        if (i == hover_) canvas_.FillRound(r, 4.0f, i == selected_ ? pal_.subtleSelected : pal_.subtleHover);
        else if (i == selected_) canvas_.FillRound(r, 4.0f, pal_.subtleHover);
        if (i == selected_) canvas_.FillRound(Rect{r.left, (r.top + r.bottom) * 0.5f - 8.0f, r.left + 3.0f, (r.top + r.bottom) * 0.5f + 8.0f},
                                              1.5f, pal_.accent);
        canvas_.Text(items_[static_cast<size_t>(i)], Rect{r.left + 12.0f, r.top, r.right - 8.0f, r.bottom}, Font::Body,
                     pal_.text, HAlign::Left);
    }
    canvas_.PopClip();
    if (MaxScroll() > 0.0f) {
        const float total = static_cast<float>(items_.size()) * kItemHeight + 2.0f * kPad;
        const float th = std::max(24.0f, heightDip_ * heightDip_ / total);
        const float ty = (heightDip_ - th) * (scroll_ / MaxScroll());
        canvas_.FillRound(Rect{widthDip_ - 6.0f, ty + 2.0f, widthDip_ - 3.0f, ty + th - 2.0f}, 1.5f, pal_.strong);
    }
    canvas_.StrokeRound(Rect{0, 0, widthDip_, heightDip_}, 8.0f, pal_.popupStroke, 1.0f);
    if (!canvas_.End()) InvalidateRect(hwnd_, nullptr, FALSE);
}

}  // namespace moonsr::app::ui
