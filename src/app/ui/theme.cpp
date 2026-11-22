#include "theme.h"

#include <windows.h>

#include <algorithm>
#include <cmath>

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

static Palette MakeLightPalette() {
    Palette p;
    p.dark = false;
    {
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
        // Logo: the brand gradient deepened for contrast on the light surfaces; no glow.
        p.logoA = Rgb(0x0891B2);
        p.logoMid = Rgb(0x3B5BDB);
        p.logoB = Rgb(0x7C3AED);
        p.glow = false;
        p.backdrop = Backdrop::Mica;
    }
    return p;
}

static Palette MakeDarkPalette() {
    Palette p;
    p.dark = true;
    {
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
        // Logo as designed: cyan ring, blue crescent turning violet, white star, soft glow.
        p.logoA = Rgb(0x3EE0FF);
        p.logoMid = Rgb(0x5B7CFF);
        p.logoB = Rgb(0xA979FF);
        p.glow = true;
        p.backdrop = Backdrop::Mica;
    }
    return p;
}

// Phosphor colours of a console theme: background, normal / dim / disabled text, borders, accent (normal / bright / dark /
// disabled), focus and the status colours. Retro uses the Windows Terminal "Campbell" family (green); Retro Amber an amber
// monitor.
struct Phosphor {
    uint32_t bg, text, textDim, textOff, border, divider, ctrlStroke, strong, hover, press;
    uint32_t accent, accentHi, accentLo, accentOff, focus;
    uint32_t info, success, warning, error;
};

static Palette MakeConsolePalette(const Phosphor& f) {
    Palette p;
    p.dark = true;
    p.retro = true;
    p.promptHeaders = true;
    p.opaqueBg = true;
    p.radius = 0.0f;
    p.font = L"Cascadia Mono";
    p.bgA = p.bgB = p.bgC = p.bg = Rgb(f.bg);
    p.micaTint = Rgb(f.bg);
    p.cardFill = Rgb(f.bg);
    p.cardStroke = Rgb(f.border);
    p.divider = Rgb(f.divider);
    p.text = Rgb(f.text);
    p.textSecondary = Rgb(f.textDim);
    p.textDisabled = Rgb(f.textOff);
    p.ctrlFill = Rgb(f.bg);
    p.ctrlHover = Rgb(f.hover);
    p.ctrlPressed = Rgb(f.press);
    p.ctrlDisabled = Rgb(f.bg);
    p.ctrlStroke = Rgb(f.ctrlStroke);
    p.strong = Rgb(f.strong);
    p.accent = Rgb(f.accent);
    p.accentHover = Rgb(f.accentHi);
    p.accentPressed = Rgb(f.accentLo);
    p.accentDisabled = Rgb(f.accentOff);
    p.gradA = p.accent;
    p.gradB = p.accent;
    p.brandSR = p.accent;
    p.subtleHover = Rgb(f.hover);
    p.subtlePressed = Rgb(f.press);
    p.subtleSelected = p.accent;  // inverted selection: accent background, black text
    p.thumb = p.accent;
    p.popupFill = Rgb(f.bg);
    p.popupStroke = p.accent;
    p.focus = Rgb(f.focus);
    p.textOnAccent = Rgb(f.bg);
    p.info = {Rgb(f.bg), Rgb(f.info), Rgb(f.bg)};
    p.success = {Rgb(f.bg), Rgb(f.success), Rgb(f.bg)};
    p.warning = {Rgb(f.bg), Rgb(f.warning), Rgb(f.bg)};
    p.error = {Rgb(f.bg), Rgb(f.error), Rgb(f.bg)};
    // The logo in the phosphor colour; phosphor glow, scanlines and a slightly translucent (acrylic) background.
    p.logoA = Rgb(f.accentHi);
    p.logoMid = Rgb(f.accent);
    p.logoB = Rgb(f.accentLo);
    p.glow = true;
    p.scanlines = true;
    p.backdrop = Backdrop::Acrylic;
    p.backdropAlpha = 0.88f;
    return p;
}

// Retro console theme (Windows Terminal "Campbell" family): near-black background, light grey text, green accent.
static Palette MakeRetroPalette() {
    return MakeConsolePalette(Phosphor{0x0C0C0C, 0xCCCCCC, 0x8A8A8A, 0x555555, 0x4A4A4A, 0x2A2A2A, 0x5C5C5C, 0x767676, 0x1F1F1F,
                                       0x2E2E2E, 0x16C60C, 0x4CE043, 0x13A10E, 0x2E4A2C, 0xF2F2F2, 0x3A96DD, 0x16C60C, 0xE9B824,
                                       0xE74856});
}

// Retro Amber: the same console look on an amber monitor. Text is pale amber, dim text an orange-brown; the status colours
// stay distinguishable from the orange accent (success green, warning pale yellow-white, error red, info cyan).
static Palette MakeRetroAmberPalette() {
    return MakeConsolePalette(Phosphor{0x0D0A06, 0xFFD9A8, 0xB07A3C, 0x5E4224, 0x5A4026, 0x2E2214, 0x6E4E2C, 0x8C6438, 0x24190C,
                                       0x34230F, 0xFF9F1C, 0xFFC061, 0xD97F0B, 0x4A3214, 0xFFF1DE, 0x5FC8E8, 0x7DD87A, 0xFFF3A0,
                                       0xFF4D4D});
}

Palette MakePalette(ThemeId theme) {
    switch (theme) {
    case ThemeId::Retro: return MakeRetroPalette();
    case ThemeId::RetroAmber: return MakeRetroAmberPalette();
    case ThemeId::Light: return MakeLightPalette();
    default: return MakeDarkPalette();
    }
}

const wchar_t* ThemeKey(ThemeId t) {
    switch (t) {
    case ThemeId::Retro: return L"retro";
    case ThemeId::RetroAmber: return L"retro-amber";
    case ThemeId::Light: return L"light";
    default: return L"dark";
    }
}

ThemeId ThemeFromKey(const std::wstring& key) {
    return key == L"retro"         ? ThemeId::Retro
           : key == L"retro-amber" ? ThemeId::RetroAmber
           : key == L"light"       ? ThemeId::Light
                                   : ThemeId::Dark;
}

D2D1_COLOR_F HueShift(const D2D1_COLOR_F& c, float degrees) {
    const float mx = std::max({c.r, c.g, c.b}), mn = std::min({c.r, c.g, c.b}), d = mx - mn;
    if (d <= 1e-5f) return c;
    float h = mx == c.r ? std::fmod((c.g - c.b) / d, 6.0f) : mx == c.g ? (c.b - c.r) / d + 2.0f : (c.r - c.g) / d + 4.0f;
    h = std::fmod(std::fmod(h * 60.0f + degrees, 360.0f) + 360.0f, 360.0f);
    const float cc = d, x = cc * (1.0f - std::fabs(std::fmod(h / 60.0f, 2.0f) - 1.0f)), m = mx - cc;
    float r = 0, g = 0, b = 0;
    if (h < 60) { r = cc; g = x; }
    else if (h < 120) { r = x; g = cc; }
    else if (h < 180) { g = cc; b = x; }
    else if (h < 240) { g = x; b = cc; }
    else if (h < 300) { r = x; b = cc; }
    else { r = cc; b = x; }
    return D2D1_COLOR_F{r + m, g + m, b + m, c.a};
}

LogoColors LogoColorsFor(const Palette& p) {
    const D2D1_COLOR_F white = Rgb(0xFFFFFF), black = Rgb(0x000000);
    auto alpha = [](const D2D1_COLOR_F& c, float a) { return D2D1_COLOR_F{c.r, c.g, c.b, a}; };
    LogoColors c;
    c.ringA = p.logoA;
    c.ringB = p.logoB;
    c.trail = p.logoMid;
    c.moonB = p.logoMid;
    c.moonC = p.logoB;
    if (p.retro) {
        // Phosphor: every part in shades of the accent; star and rim nearly white-hot.
        c.moonA = Mix(p.logoA, white, 0.25f);
        c.rim = Mix(p.logoA, white, 0.65f);
        c.star = Mix(p.logoA, white, 0.75f);
        c.glow = alpha(p.logoMid, p.glow ? 0.30f : 0.0f);
        c.srA = Mix(p.logoA, white, 0.40f);
        c.srB = p.logoA;
    } else if (p.dark) {
        c.moonA = Mix(p.logoA, white, 0.55f);
        c.rim = Mix(p.logoB, white, 0.82f);
        c.star = white;
        c.glow = alpha(p.logoMid, p.glow ? 0.40f : 0.0f);
        c.srA = p.logoA;
        c.srB = p.logoB;
    } else {
        // Light backgrounds: deeper body, a pale rim inside the crescent and a dark star so it stays visible.
        c.moonA = Mix(p.logoA, white, 0.15f);
        c.rim = Mix(p.logoMid, white, 0.70f);
        c.star = Mix(p.logoMid, black, 0.35f);
        c.glow = alpha(p.logoMid, p.glow ? 0.16f : 0.0f);
        c.srA = p.logoA;
        c.srB = p.logoB;
    }
    return c;
}

}  // namespace moonsr::app::ui
