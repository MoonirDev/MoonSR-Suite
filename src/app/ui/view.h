// View: the whole client area of the main window. Owns the navigation pane, the pages, focus, hover, scrolling,
// the dropdown popup and animation timing, and renders everything to one Direct2D target. MainWindow forwards
// window messages to it. Repaints happen only on request (Invalidate); animations use a short timer that stops
// when nothing moves. Threading: UI thread only.
#pragma once

#include "controls.h"
#include "dialog.h"
#include "gfx.h"
#include "page.h"
#include "popup.h"
#include "theme.h"

#include <windows.h>

#include <functional>
#include <memory>
#include <vector>

namespace moonsr::app::ui {

class View {
public:
    static constexpr UINT_PTR kAnimTimerId = 90;

    explicit View(Gfx& gfx);
    ~View();

    void Attach(HWND hwnd, UINT dpi);
    Page* AddPage(const std::wstring& title, wchar_t navIcon, const std::wstring& navLabel);

    void SetTheme(bool dark);
    void SetDpi(UINT dpi);
    void SetMica(bool on);
    const Palette& palette() const { return pal_; }
    Gfx& gfx() { return gfx_; }
    UINT dpi() const { return dpi_; }
    float Scale() const { return static_cast<float>(dpi_) / 96.0f; }
    void SetNavStatus(const std::wstring& text, bool active);

    // Window message entry points.
    void OnPaint();
    void OnSize();
    void OnMouseMove(int px, int py);
    void OnMouseDown(int px, int py);
    void OnMouseUp(int px, int py);
    void OnMouseLeave();
    void OnMouseWheel(int delta, int px, int py);
    bool OnKeyDown(UINT vk);
    void OnAnimTimer();
    void OnWindowDeactivated();
    bool WantsHandCursor() const { return dialog_ ? dialog_->WantsHand() : (hover_ && hover_->IsLink()); }
    // Alt+Up / Alt+Down: forwarded to the focused control. Esc: closes the dialog or cancels a drag; true = consumed.
    bool OnAltKey(UINT vk);
    bool OnEscape();
    // Modal dialog (one at a time). The spec callbacks run after the dialog has closed.
    void ShowDialog(ModalDialog::Spec spec);
    bool DialogOpen() const { return dialog_ != nullptr; }

    // Services for controls and the owner.
    void Invalidate();
    void RequestLayout();
    void RequestAnimation();
    void OpenDropdown(ComboBox* combo);
    void CloseDropdown();
    bool DropdownOpen() const { return popup_.IsOpen(); }
    void Navigate(int pageIndex);
    void MoveNav(int delta);
    void OnControlEnabledChanged(Control* c);
    void FocusControl(Control* c, bool keyboard);
    ComboBox* FirstComboOnPage() const;

    std::function<void()> onDefaultAction;  // Enter with no control claiming it, on page `defaultActionPage`
    int defaultActionPage = 0;
    std::function<void(int)> onPageChanged;

private:
    void Layout();
    void DrawNav(float w, float h);
    void DrawHeader(float w);
    void DrawScrollbar(float w, float h);
    Control* HitTest(float x, float y, bool* inNav) const;
    void ToLocal(const Control* c, bool inNav, float x, float y, float* lx, float* ly) const;
    float MaxScroll() const;
    void SetScroll(float v);
    void ScrollIntoView(const Control* c);
    void FinishDialog();
    void MoveFocus(int dir);
    std::vector<Control*> FocusChain() const;
    Rect ScrollThumb() const;
    float ClientW() const;
    float ClientH() const;

    Gfx& gfx_;
    Canvas canvas_;
    DropdownPopup popup_;
    std::unique_ptr<ModalDialog> dialog_;
    HWND hwnd_ = nullptr;
    UINT dpi_ = 96;
    Palette pal_;
    bool mica_ = false;

    std::vector<std::unique_ptr<Page>> pages_;
    std::vector<std::unique_ptr<NavItem>> nav_;
    int cur_ = 0;

    bool layoutDirty_ = true;
    bool compact_ = false;
    float navW_ = metric::kNavWidth;
    float contentLeft_ = 0, contentW_ = 0, pageH_ = 0, scroll_ = 0;

    Control* hover_ = nullptr;
    Control* pressed_ = nullptr;
    bool pressedInNav_ = false;
    Control* focus_ = nullptr;
    ComboBox* popupCombo_ = nullptr;
    bool trackingMouse_ = false;
    float mouseX_ = -1, mouseY_ = -1;
    bool scrollHover_ = false, scrollDrag_ = false;
    float dragOffset_ = 0;
    bool animating_ = false;
    ULONGLONG lastTick_ = 0;
    std::wstring navStatus_;
    bool navActive_ = false;
};

}  // namespace moonsr::app::ui
