// View: the whole client area of the main window. Owns the navigation pane, the pages, focus, hover, scrolling,
// the dropdown popup and animation timing, and renders everything to one Direct2D target. MainWindow forwards
// window messages to it. Repaints happen only on request (Invalidate). Timers: one animation timer (16 ms) that runs only
// while something moves (hover / press fades, toggles, slider thumbs, page transitions, afterglow, scrolling) and a
// 530 ms cursor-blink timer that runs only while a blinking cursor is on screen (console themes) - both only with visual
// effects on and both stopped while the window is minimised or hidden. Threading: UI thread only.
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
    static constexpr UINT_PTR kBlinkTimerId = 91;
    static constexpr UINT kBlinkMs = 530;

    explicit View(Gfx& gfx);
    ~View();

    void Attach(HWND hwnd, UINT dpi);
    Page* AddPage(const std::wstring& title, wchar_t navIcon, const std::wstring& navLabel);

    void SetTheme(const Palette& palette);
    void SetDpi(UINT dpi);
    void SetMica(bool on);  // a translucent system backdrop (Mica or Acrylic) is behind the client area
    // Visual effects (setting AND Windows animation / transparency effects): transitions, fades, glow, scanlines,
    // blinking. Off: everything is drawn in its final state, cursors are steady.
    void SetEffects(bool on);
    bool effects() const { return effects_; }
    bool Afterglow() const { return effects_ && pal_.retro && pal_.glow; }  // console phosphor afterglow
    bool CursorOn() const { return !effects_ || blinkOn_; }                // blinking cursors: visible phase
    void ResetBlink();                       // typing: show the cursor and restart the blink period
    // The window is shown (false while minimised or hidden in the tray): stops / restarts the timers.
    void SetShown(bool shown);
    void OnBlinkTimer();
    // Tests: scrolls the page / the first scrollable control under (x, y) (client DIPs).
    void TestScroll(float y) { SetScroll(y); }
    void TestWheel(float x, float y, float notches);
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
    bool OnChar(wchar_t ch);                 // WM_CHAR: forwarded to the focused text entry control
    void OnWindowFocusChanged(bool active);  // text entry commits when the window loses the keyboard
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
    bool NeedsBlink() const;
    void UpdateBlinkTimer();
    void StartAnimTimer();
    bool AnyAnimPending() const;
    void SnapStates();

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
    bool effects_ = true, shown_ = true, blinkOn_ = true, blinking_ = false;
    float pageT_ = 1.0f;  // page-switch transition progress (Fluent themes)
    ULONGLONG lastTick_ = 0;
    std::wstring navStatus_;
    bool navActive_ = false;
};

}  // namespace moonsr::app::ui
