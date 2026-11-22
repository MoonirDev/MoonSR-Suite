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
    // Cached geometry of the logo mark, in the unit space of the SVG mockup (see Canvas::LogoMark).
    ID2D1Geometry* logoTopRightTile(bool inset) const { return tileTR_[inset ? 1 : 0].Get(); }
    ID2D1Geometry* logoCrescent() const { return crescent_.Get(); }
    ID2D1Geometry* logoHighlight(int i) const { return highlight_[i].Get(); }
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
    Microsoft::WRL::ComPtr<ID2D1Geometry> tileTR_[2], crescent_, highlight_[2];
    std::wstring displayFamily_;
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
    // Segoe Fluent Icons / MDL2 glyph centred in `r`. No-op when no icon font is installed.
    void Icon(wchar_t glyph, const Rect& r, Font f, const D2D1_COLOR_F& c);
    // Linear gradient fill of a (rounded) rectangle; the gradient axis runs from (x0,y0) to (x1,y1) in the current space.
    struct Stop {
        float pos;
        D2D1_COLOR_F color;
    };
    void FillGradient(const Rect& r, float radius, const std::vector<Stop>& stops, float x0, float y0, float x1, float y1);
    // The MoonSR logo mark (2x2 glass tiles, crescent, check) fitted into the rect `r`. `detailed` adds the halo, glow, drop shadow
    // and highlights (these overflow `r`); without it the mark is simplified for small sizes. Geometry follows
    // assets/brand/moonsr_mockup.svg and tools/make_icon.ps1.
    void LogoMark(const Rect& r, bool detailed, bool onLight = false);
    // "MoonSR" wordmark with the top-left at (x, y). Returns its width.
    float Wordmark(float x, float y, float size, const D2D1_COLOR_F& moonA, const D2D1_COLOR_F& moonB, const D2D1_COLOR_F& sr);
    // Single-line text with extra letter spacing (DIPs), top-left at (x, y). Returns its width.
    float SpacedText(const std::wstring& s, float x, float y, float size, DWRITE_FONT_WEIGHT weight, const D2D1_COLOR_F& c, float spacing);
    void PushClip(const Rect& r);
    void PopClip();
    void SetOffset(float dx, float dy);

private:
    Gfx& gfx_;
    Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> rt_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_;
    Microsoft::WRL::ComPtr<ID2D1StrokeStyle> dashStroke_;
    float radiusCap_ = -1.0f, borderW_ = 1.0f;
    HWND hwnd_ = nullptr;
    UINT dpi_ = 96;
    bool alpha_ = false;
    int clipDepth_ = 0;
};

}  // namespace moonsr::app::ui
