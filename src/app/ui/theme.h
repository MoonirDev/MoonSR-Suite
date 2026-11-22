// Fluent-style theme with the MoonSR brand: navy surfaces and a cyan accent in dark, a matching light variant with the
// same accent hue darkened for contrast, brand gradient colours and layout metrics (all in DIPs). The accent is fixed to
// the brand; the Windows accent colour is not used (SystemAccentColor() is kept for a future opt-in setting).
// Threading: UI thread only.
#pragma once

#include <d2d1.h>

#include <cstdint>

namespace moonsr::app::ui {

D2D1_COLOR_F Rgb(uint32_t rgb, float alpha = 1.0f);
D2D1_COLOR_F Argb(uint32_t argb);  // 0xAARRGGBB, straight (non-premultiplied) alpha
D2D1_COLOR_F Mix(const D2D1_COLOR_F& a, const D2D1_COLOR_F& b, float t);
float Luma(const D2D1_COLOR_F& c);

struct InfoColors {
    D2D1_COLOR_F fill;
    D2D1_COLOR_F icon;      // colour of the filled icon disc
    D2D1_COLOR_F iconText;  // glyph drawn on the disc
};

struct Palette {
    bool dark = true;
    D2D1_COLOR_F bg;  // solid colour behind everything when Mica is not available
    D2D1_COLOR_F cardFill, cardStroke, divider;
    D2D1_COLOR_F text, textSecondary, textDisabled, textOnAccent;
    D2D1_COLOR_F ctrlFill, ctrlHover, ctrlPressed, ctrlDisabled, ctrlStroke;
    D2D1_COLOR_F strong;  // off-state switch outline, slider rail
    D2D1_COLOR_F accent, accentHover, accentPressed, accentDisabled;
    D2D1_COLOR_F subtleHover, subtlePressed, subtleSelected;
    D2D1_COLOR_F thumb, popupFill, popupStroke, focus;
    D2D1_COLOR_F gradA, gradB;   // brand gradient (cyan to violet), used sparingly: nav indicator, rules, meters
    D2D1_COLOR_F brandSR;        // the "SR" of the wordmark
    D2D1_COLOR_F bgA, bgB, bgC;  // window background gradient (top-left, middle, bottom-right) when Mica is not used
    D2D1_COLOR_F micaTint;       // translucent tint painted over Mica so the backdrop picks up the brand hue
    InfoColors info, success, warning, error;
};

Palette MakePalette(bool dark);
bool SystemUsesDarkApps();
D2D1_COLOR_F SystemAccentColor();

namespace metric {
constexpr float kNavWidth = 280.0f;
constexpr float kNavCompactWidth = 56.0f;
constexpr float kNavCompactBelow = 760.0f;   // client width (DIP) under which the nav pane collapses to icons
constexpr float kHeaderHeight = 76.0f;
constexpr float kPadX = 36.0f;
constexpr float kPadXNarrow = 20.0f;
constexpr float kContentMaxWidth = 960.0f;
constexpr float kCardRadius = 8.0f;
constexpr float kControlRadius = 4.0f;
constexpr float kRowHeight = 56.0f;
constexpr float kRowHeightDesc = 68.0f;
constexpr float kItemGap = 8.0f;
constexpr float kControlHeight = 32.0f;
constexpr float kMinClientWidth = 640.0f;
constexpr float kMinClientHeight = 480.0f;
constexpr float kDefaultClientWidth = 1120.0f;
constexpr float kDefaultClientHeight = 740.0f;
}  // namespace metric

}  // namespace moonsr::app::ui
