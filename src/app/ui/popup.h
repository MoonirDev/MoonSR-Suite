// Drop-down list shown by a ComboBox: a borderless, shadowed, rounded popup window with hover and selection
// indicators, mouse wheel and keyboard support. It never takes activation; View forwards keyboard input to it.
// Closes on outside click, Esc, focus loss and window moves. Threading: UI thread only.
#pragma once

#include "gfx.h"

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

namespace moonsr::app::ui {

class DropdownPopup {
public:
    explicit DropdownPopup(Gfx& gfx) : gfx_(gfx), canvas_(gfx) {}
    ~DropdownPopup();

    static bool RegisterWindowClass(HINSTANCE instance);

    // `anchorScreenPx` is the combo box rectangle in screen pixels. Returns false if the window could not be created.
    bool Open(HWND owner, const std::vector<std::wstring>& items, int selected, const RECT& anchorScreenPx, float minWidthDip,
              UINT dpi, const Palette& palette);
    // `commit` = use the highlighted item. Fires onClosed(index) with -1 when cancelled.
    void Close(bool commit);
    bool IsOpen() const { return hwnd_ != nullptr; }
    bool OnKey(UINT vk);
    void OnWheel(int delta);
    void SetPalette(const Palette& p);

    std::function<void(int)> onClosed;

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT OnMessage(UINT msg, WPARAM wp, LPARAM lp);
    void Paint();
    int ItemAt(float xDip, float yDip) const;
    void ScrollTo(int index);
    float MaxScroll() const;
    float Scale() const { return static_cast<float>(dpi_) / 96.0f; }

    Gfx& gfx_;
    Canvas canvas_;
    HWND hwnd_ = nullptr;
    Palette pal_;
    std::vector<std::wstring> items_;
    int selected_ = -1;
    int hover_ = -1;
    UINT dpi_ = 96;
    float widthDip_ = 200.0f;
    float heightDip_ = 100.0f;
    float scroll_ = 0.0f;
    bool dragging_ = false;
};

}  // namespace moonsr::app::ui
