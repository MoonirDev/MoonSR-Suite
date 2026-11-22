#include "theme.h"

#include <windows.h>

#include <algorithm>

namespace moonsr::app::ui {

D2D1_COLOR_F Rgb(uint32_t rgb, float alpha) {
    return D2D1_COLOR_F{static_cast<float>((rgb >> 16) & 0xFF) / 255.0f, static_cast<float>((rgb >> 8) & 0xFF) / 255.0f,
                        static_cast<float>(rgb & 0xFF) / 255.0f, alpha};
}

D2D1_COLOR_F Argb(uint32_t argb) { return Rgb(argb & 0xFFFFFF, static_cast<float>((argb >> 24) & 0xFF) / 255.0f); }

D2D1_COLOR_F Mix(const D2D1_COLOR_F& a, const D2D1_COLOR_F& b, float t) {
    return D2D1_COLOR_F{a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t};
}

float Luma(const D2D1_COLOR_F& c) { return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b; }

bool SystemUsesDarkApps() {
    DWORD value = 1, size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS)
        return false;
    return value == 0;
}

D2D1_COLOR_F SystemAccentColor() {
    DWORD v = 0, size = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM", L"AccentColor", RRF_RT_REG_DWORD, nullptr,
                     &v, &size) == ERROR_SUCCESS) {
        // Stored as 0xAABBGGRR.
        const uint32_t r = v & 0xFF, g = (v >> 8) & 0xFF, b = (v >> 16) & 0xFF;
        return Rgb((r << 16) | (g << 8) | b);
    }
    return Rgb(0x0078D4);
}

Palette MakePalette(bool dark) {
    Palette p;
    p.dark = dark;
    if (dark) {
        // Navy surfaces derived from the brand background gradient (#202B48 / #111A31 / #17142F).
        p.bgA = Rgb(0x202B48);
        p.bgB = Rgb(0x111A31);
        p.bgC = Rgb(0x17142F);
        p.bg = p.bgB;
        p.micaTint = Rgb(0x111A31, 0.74f);
        p.cardFill = Argb(0x14B4C6E5);
        p.cardStroke = Argb(0x26B4C6E5);
        p.divider = Argb(0x1AB4C6E5);
        p.text = Rgb(0xFFFFFF);
        p.textSecondary = Rgb(0xB4C6E5);
        p.textDisabled = Argb(0x80B4C6E5);
        p.ctrlFill = Argb(0x1AB4C6E5);
        p.ctrlHover = Argb(0x2CB4C6E5);
        p.ctrlPressed = Argb(0x10B4C6E5);
        p.ctrlDisabled = Argb(0x0DB4C6E5);
        p.ctrlStroke = Argb(0x33B4C6E5);
        p.strong = Rgb(0x8EA3CB);  // off-state switch outline, slider rail (>= 3:1 on the navy surfaces)
        p.accent = Rgb(0x5BC7FF);
        p.accentHover = Rgb(0x73D8FF);
        p.accentPressed = Rgb(0x5CE5FF);
        p.accentDisabled = Argb(0x30B4C6E5);
        p.gradA = Rgb(0x5CE5FF);
        p.gradB = Rgb(0x9584FF);
        p.brandSR = Rgb(0x73D8FF);
        p.subtleHover = Argb(0x1CB4C6E5);
        p.subtlePressed = Argb(0x10B4C6E5);
        p.subtleSelected = Argb(0x335BC7FF);
        p.thumb = Rgb(0x1E2A4A);
        p.popupFill = Rgb(0x19233F);
        p.popupStroke = Argb(0x45B4C6E5);
        p.focus = Rgb(0x73D8FF);
        p.textOnAccent = Rgb(0x0A1226);
        p.info = {Rgb(0x1A2B4D), p.accent, p.textOnAccent};
        p.success = {Rgb(0x153A38), Rgb(0x6CCB5F), Rgb(0x000000)};
        p.warning = {Rgb(0x3B3520), Rgb(0xFCE100), Rgb(0x000000)};
        p.error = {Rgb(0x42263A), Rgb(0xFF99A4), Rgb(0x000000)};
    } else {
        // Light variant: same hue family, accent darkened to #0A6DAE (>= 4.5:1 for text on the light surfaces).
        p.bgA = Rgb(0xF4F8FE);
        p.bgB = Rgb(0xE8EFFA);
        p.bgC = Rgb(0xEFECFA);
        p.bg = p.bgB;
        p.micaTint = Rgb(0xEEF3FB, 0.66f);
        p.cardFill = Argb(0xCCFFFFFF);
        p.cardStroke = Argb(0x2A203B6B);
        p.divider = Argb(0x1A203B6B);
        p.text = Rgb(0x111A31);
        p.textSecondary = Rgb(0x44526E);
        p.textDisabled = Argb(0x7A111A31);
        p.ctrlFill = Argb(0xD9FFFFFF);
        p.ctrlHover = Argb(0xF0E6EEFB);
        p.ctrlPressed = Argb(0xA8D6E2F6);
        p.ctrlDisabled = Argb(0x70E6EDF8);
        p.ctrlStroke = Argb(0x38203B6B);
        p.strong = Rgb(0x5B6A89);
        p.accent = Rgb(0x0A6DAE);
        p.accentHover = Rgb(0x085C95);
        p.accentPressed = Rgb(0x064A78);
        p.accentDisabled = Argb(0x40111A31);
        p.gradA = Rgb(0x0A86C4);
        p.gradB = Rgb(0x6A55E0);
        p.brandSR = Rgb(0x0A6DAE);
        p.subtleHover = Argb(0x140A4C8C);
        p.subtlePressed = Argb(0x0A0A4C8C);
        p.subtleSelected = Argb(0x240A6DAE);
        p.thumb = Rgb(0xFFFFFF);
        p.popupFill = Rgb(0xFAFCFF);
        p.popupStroke = Argb(0x38203B6B);
        p.focus = Rgb(0x084C7D);
        p.textOnAccent = Rgb(0xFFFFFF);
        p.info = {Rgb(0xE3EEFB), p.accent, p.textOnAccent};
        p.success = {Rgb(0xDFF6DD), Rgb(0x0F7B0F), Rgb(0xFFFFFF)};
        p.warning = {Rgb(0xFFF4CE), Rgb(0x9D5D00), Rgb(0xFFFFFF)};
        p.error = {Rgb(0xFDE7E9), Rgb(0xC42B1C), Rgb(0xFFFFFF)};
    }
    return p;
}

}  // namespace moonsr::app::ui
