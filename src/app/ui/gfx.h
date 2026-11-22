// Direct2D / DirectWrite resources and drawing helpers.
// Gfx: shared factories and text formats (UI thread). Canvas: one HWND render target with drawing helpers; recreated
// automatically after D2DERR_RECREATE_TARGET. All coordinates are DIPs (the target's DPI is the window DPI).
#pragma once

#include "theme.h"

#include <d2d1.h>
#include <d2d1helper.h>
#include <dwrite.h>
#include <dxgiformat.h>
#include <windows.h>
#include <wrl/client.h>

#include <string>
#include <vector>

namespace moonsr::app::ui {

using Rect = D2D1_RECT_F;
inline float RW(const Rect& r) { return r.right - r.left; }
inline float RH(const Rect& r) { return r.bottom - r.top; }
inline bool RectHas(const Rect& r, float x, float y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }
inline Rect Inflate(const Rect& r, float d) { return Rect{r.left - d, r.top - d, r.right + d, r.bottom + d}; }

enum class Font { Body, BodyStrong, Caption, Section, Title, Value, ValueSmall, Icon, IconSmall, NavTitle, Glyph, Count };
enum class HAlign { Left, Center, Right };

// Icon "glyphs" that are drawn as vectors by Canvas::Icon instead of coming from the icon font (control characters, so
// they never collide with a font glyph).
constexpr wchar_t kGlyphNeuralNet = 0x0001;  // small neural network: 2-3-2 nodes joined by thin lines

class Gfx {
public:
    bool Init();
    ID2D1Factory* d2d() const { return d2d_.Get(); }
    IDWriteFactory* dwrite() const { return dw_.Get(); }
    IDWriteTextFormat* Format(Font f) const { return fmt_[static_cast<size_t>(f)].Get(); }
    ID2D1StrokeStyle* roundStroke() const { return roundStroke_.Get(); }
    bool hasIcons() const { return icons_; }
    bool hasFluentIcons() const { return fluentIcons_; }
    // Theme style. `font`: family override (used when installed; empty = the theme's default family). `retro`: console look
    // (monospace fallback chain, smaller sizes, no icon glyphs). `radiusCap`: maximum corner radius in DIPs (< 0 = none).
    // `borderWidth`: width of 1 px borders in DIPs. Rebuilds the text formats when needed (callers re-layout afterwards).
    void SetStyle(const std::wstring& font, bool retro, float radiusCap, float borderWidth);
    bool retro() const { return retro_; }
    float radiusCap() const { return radiusCap_; }
    float borderWidth() const { return borderWidth_; }
    // A text format in the display family (Segoe UI Variable Display when installed) at an arbitrary size, for the brand
    // wordmark and tag line.
    Microsoft::WRL::ComPtr<IDWriteTextFormat> MakeDisplayFormat(float size, DWRITE_FONT_WEIGHT weight) const;
    // The same in the Fluent display family (`mono` false) or the console monospace family (true), whatever the theme.
    Microsoft::WRL::ComPtr<IDWriteTextFormat> MakeFamilyFormat(bool mono, float size, DWRITE_FONT_WEIGHT weight) const;
    // Cached geometry of the logo mark in its 256-unit box (assets/brand/moonsr_mark.svg). `simplified`: the simplified mark
    // (no trails, thicker ring, larger star and moon) used at small pixel sizes.
    struct LogoGeometry {
        Microsoft::WRL::ComPtr<ID2D1Geometry> crescent, rim, ring, star;
    };
    const LogoGeometry& logo(bool simplified) const { return logo_[simplified ? 1 : 0]; }
    float MeasureWidth(const std::wstring& text, Font f) const;
    float MeasureHeight(const std::wstring& text, Font f, float maxWidth) const;

private:
    void BuildLogoGeometry();
    bool BuildFormats();
    bool retro_ = false;
    std::wstring font_;
    float radiusCap_ = -1.0f, borderWidth_ = 1.0f;
    Microsoft::WRL::ComPtr<ID2D1Factory> d2d_;
    Microsoft::WRL::ComPtr<IDWriteFactory> dw_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt_[static_cast<size_t>(Font::Count)];
    Microsoft::WRL::ComPtr<IDWriteInlineObject> ellipsis_[static_cast<size_t>(Font::Count)];
    Microsoft::WRL::ComPtr<ID2D1StrokeStyle> roundStroke_;
    LogoGeometry logo_[2];
    std::wstring displayFamily_, fluentDisplayFamily_, monoFamily_;
    bool icons_ = false;
    bool fluentIcons_ = false;
};

class Canvas {
public:
    explicit Canvas(Gfx& gfx) : gfx_(gfx) {}
    Gfx& gfx() const { return gfx_; }

    // Creates or resizes the target and starts drawing. `alpha` = premultiplied alpha target (needed for Mica).
    bool Begin(HWND hwnd, UINT dpi, bool alpha);
    // Ends drawing. Returns false when the target was lost (it is recreated on the next Begin; repaint).
    bool End();
    void Release();
    bool valid() const { return rt_ != nullptr; }

    void Clear(const D2D1_COLOR_F& c);
    void FillRect(const Rect& r, const D2D1_COLOR_F& c);
    void FillRound(const Rect& r, float radius, const D2D1_COLOR_F& c);
    // Strokes inside `r` (the stroke is inset by half its width).
    void StrokeRound(const Rect& r, float radius, const D2D1_COLOR_F& c, float width = 1.0f);
    void FillCircle(float cx, float cy, float radius, const D2D1_COLOR_F& c);
    void StrokeCircle(float cx, float cy, float radius, const D2D1_COLOR_F& c, float width = 1.0f);
    // Dashed outline inside `r` (keyboard focus ring in the console theme).
    void DashedRect(const Rect& r, const D2D1_COLOR_F& c, float width = 1.0f);
    // Corner radius cap (< 0 = none; 0 = square, aliased geometry) and 1 px border width. Follows the Gfx style at Begin();
    // the theme picker flips it temporarily to draw a miniature of another theme.
    void SetStyle(float radiusCap, float borderWidth) {
        radiusCap_ = radiusCap;
        borderW_ = borderWidth;
        if (rt_) rt_->SetAntialiasMode(radiusCap == 0.0f ? D2D1_ANTIALIAS_MODE_ALIASED : D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    }
    float radiusCap() const { return radiusCap_; }
    float borderW() const { return borderW_; }
    void Line(float x1, float y1, float x2, float y2, const D2D1_COLOR_F& c, float width = 1.0f);
    void Chevron(float cx, float cy, float halfWidth, const D2D1_COLOR_F& c, bool up = false);
    void Check(float cx, float cy, float size, const D2D1_COLOR_F& c);
    void Text(const std::wstring& s, const Rect& r, Font f, const D2D1_COLOR_F& c, HAlign h = HAlign::Left,
              bool wrap = false);
    // Segoe Fluent Icons / MDL2 glyph centred in `r` (or one of the vector glyphs above, drawn at the same size and
    // stroke weight). No-op when no icon font is installed or the theme shows no icons (console themes).
    void Icon(wchar_t glyph, const Rect& r, Font f, const D2D1_COLOR_F& c);
    // Linear gradient fill of a (rounded) rectangle; the gradient axis runs from (x0,y0) to (x1,y1) in the current space.
    struct Stop {
        float pos;
        D2D1_COLOR_F color;
    };
    void FillGradient(const Rect& r, float radius, const std::vector<Stop>& stops, float x0, float y0, float x1, float y1);
    // The MoonSR logo mark (crescent with motion trails, orbit ring, four-point star) fitted into `r` (aspect kept, centred).
    // The simplified mark is used automatically when `r` is small in device pixels. `glow`: soft halo behind the moon and
    // the star (it overflows `r`). Geometry follows assets/brand/moonsr_mark.svg and tools/make_icon.ps1.
    void LogoMark(const Rect& r, const LogoColors& colors, bool glow);
    // "MoonSR" wordmark with the top-left at (x, y): "Moon" in `moon`, "SR" in a horizontal srA -> srB gradient. Uses the
    // display family (the monospace family in console themes; `mono` 0 / 1 forces either, for theme previews). Returns its
    // width; `height` receives the line height.
    float Wordmark(float x, float y, float size, const D2D1_COLOR_F& moon, const D2D1_COLOR_F& srA, const D2D1_COLOR_F& srB,
                   float* height = nullptr, int mono = -1);
    // Phosphor glow: while on, Text() and FillRect() in one of the two glow colours get a soft halo (console themes).
    void SetPhosphor(bool on, const D2D1_COLOR_F& a, const D2D1_COLOR_F& b) {
        phosphor_ = on;
        glowA_ = a;
        glowB_ = b;
    }
    bool phosphor() const { return phosphor_; }
    const D2D1_COLOR_F& glowA() const { return glowA_; }
    const D2D1_COLOR_F& glowB() const { return glowB_; }
    // Faint horizontal scanlines over `r` (one dark device-pixel row in three).
    void Scanlines(const Rect& r, float strength);
    // Draws everything until PopOpacity() at `alpha` (a Direct2D layer).
    void PushOpacity(const Rect& r, float alpha);
    void PopOpacity();
    float DpiScale() const { return static_cast<float>(dpi_) / 96.0f; }
    // Single-line text with extra letter spacing (DIPs), top-left at (x, y). Returns its width.
    float SpacedText(const std::wstring& s, float x, float y, float size, DWRITE_FONT_WEIGHT weight, const D2D1_COLOR_F& c, float spacing);
    void NeuralNetGlyph(const Rect& r, float emSize, const D2D1_COLOR_F& c);
    void PushClip(const Rect& r);
    void PopClip();
    void SetOffset(float dx, float dy);

private:
    Gfx& gfx_;
    Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> rt_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_;
    Microsoft::WRL::ComPtr<ID2D1StrokeStyle> dashStroke_;
    Microsoft::WRL::ComPtr<ID2D1BitmapBrush> scanBrush_;
    Microsoft::WRL::ComPtr<ID2D1Layer> layer_;
    bool phosphor_ = false;
    D2D1_COLOR_F glowA_{}, glowB_{};
    bool IsGlowColor(const D2D1_COLOR_F& c) const;
    float radiusCap_ = -1.0f, borderW_ = 1.0f;
    HWND hwnd_ = nullptr;
    UINT dpi_ = 96;
    bool alpha_ = false;
    int clipDepth_ = 0;
    int layerDepth_ = 0;
};

}  // namespace moonsr::app::ui
