#include "gfx.h"

#include "core/log.h"

#include <algorithm>
#include <cmath>
#include <dwrite_1.h>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace moonsr::app::ui {
namespace {

bool HasFamily(IDWriteFontCollection* coll, const wchar_t* name) {
    UINT32 index = 0;
    BOOL exists = FALSE;
    return coll && SUCCEEDED(coll->FindFamilyName(name, &index, &exists)) && exists;
}

}  // namespace

bool Gfx::BuildFormats() {
    ComPtr<IDWriteFontCollection> sys;
    dw_->GetSystemFontCollection(sys.GetAddressOf(), FALSE);
    const wchar_t* text = HasFamily(sys.Get(), L"Segoe UI Variable Text") ? L"Segoe UI Variable Text" : L"Segoe UI";
    const wchar_t* display =
        HasFamily(sys.Get(), L"Segoe UI Variable Display") ? L"Segoe UI Variable Display" : L"Segoe UI";
    const wchar_t* icon = nullptr;
    fluentIcons_ = HasFamily(sys.Get(), L"Segoe Fluent Icons");
    if (fluentIcons_) icon = L"Segoe Fluent Icons";
    else if (HasFamily(sys.Get(), L"Segoe MDL2 Assets")) icon = L"Segoe MDL2 Assets";
    icons_ = icon != nullptr;
    fluentDisplayFamily_ = display;
    monoFamily_ = HasFamily(sys.Get(), L"Cascadia Mono") ? L"Cascadia Mono" : HasFamily(sys.Get(), L"Consolas") ? L"Consolas" : L"Courier New";
    if (retro_) {
        // Console look: Cascadia Mono (Windows Terminal's font), then Consolas, then Courier New. No icon glyphs.
        text = HasFamily(sys.Get(), L"Cascadia Mono") ? L"Cascadia Mono"
               : HasFamily(sys.Get(), L"Consolas")    ? L"Consolas"
                                                      : L"Courier New";
        display = text;
        icon = nullptr;
        icons_ = false;
        fluentIcons_ = false;
    }
    if (!font_.empty() && HasFamily(sys.Get(), font_.c_str())) {  // theme font override, only when installed
        text = display = font_.c_str();
    }
    MSR_LOG_INFO(L"UI fonts: text '{}', display '{}', icons '{}'", text, display, icon ? icon : L"(none)");

    struct Spec {
        const wchar_t* family;
        float size;
        DWRITE_FONT_WEIGHT weight;
    };
    const bool r = retro_;
    const DWRITE_FONT_WEIGHT strong = r ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_SEMI_BOLD;
    const Spec specs[static_cast<size_t>(Font::Count)] = {
        {text, r ? 13.0f : 14.0f, DWRITE_FONT_WEIGHT_NORMAL},      // Body
        {text, r ? 13.0f : 14.0f, strong},                          // BodyStrong
        {text, 12.0f, DWRITE_FONT_WEIGHT_NORMAL},                  // Caption
        {display, r ? 14.0f : 18.0f, strong},                       // Section
        {display, r ? 24.0f : 28.0f, strong},                       // Title
        {display, r ? 22.0f : 26.0f, strong},                       // Value
        {display, r ? 15.0f : 18.0f, strong},                       // ValueSmall
        {icon ? icon : text, 18.0f, DWRITE_FONT_WEIGHT_NORMAL},    // Icon
        {icon ? icon : text, 12.0f, DWRITE_FONT_WEIGHT_NORMAL},    // IconSmall
        {text, r ? 14.0f : 16.0f, strong},                          // NavTitle
        {text, 12.0f, DWRITE_FONT_WEIGHT_BOLD},                    // Glyph
    };
    for (size_t i = 0; i < static_cast<size_t>(Font::Count); ++i) {
        fmt_[i].Reset();
        ellipsis_[i].Reset();
        const HRESULT hr = dw_->CreateTextFormat(specs[i].family, nullptr, specs[i].weight, DWRITE_FONT_STYLE_NORMAL,
                                                 DWRITE_FONT_STRETCH_NORMAL, specs[i].size, L"en-us", fmt_[i].GetAddressOf());
        if (FAILED(hr)) {
            MSR_LOG_ERROR(L"CreateTextFormat failed (0x{:08X})", static_cast<unsigned>(hr));
            return false;
        }
        fmt_[i]->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        if (SUCCEEDED(dw_->CreateEllipsisTrimmingSign(fmt_[i].Get(), ellipsis_[i].GetAddressOf()))) {
            const DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            fmt_[i]->SetTrimming(&trim, ellipsis_[i].Get());
        }
    }
    displayFamily_ = display;
    return true;
}

void Gfx::SetStyle(const std::wstring& font, bool retro, float radiusCap, float borderWidth) {
    radiusCap_ = radiusCap;
    borderWidth_ = borderWidth;
    if (retro_ == retro && font_ == font) return;
    retro_ = retro;
    font_ = font;
    if (dw_) BuildFormats();
}

bool Gfx::Init() {
    HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d_.GetAddressOf());
    if (FAILED(hr)) {
        MSR_LOG_ERROR(L"D2D1CreateFactory failed (0x{:08X})", static_cast<unsigned>(hr));
        return false;
    }
    hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                             reinterpret_cast<IUnknown**>(dw_.GetAddressOf()));
    if (FAILED(hr)) {
        MSR_LOG_ERROR(L"DWriteCreateFactory failed (0x{:08X})", static_cast<unsigned>(hr));
        return false;
    }

    if (!BuildFormats()) return false;

    const D2D1_STROKE_STYLE_PROPERTIES sp = D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                                                         D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND);
    d2d_->CreateStrokeStyle(sp, nullptr, 0, roundStroke_.GetAddressOf());
    BuildLogoGeometry();
    return true;
}

namespace {

ComPtr<ID2D1Geometry> Combine(ID2D1Factory* f, ID2D1Geometry* a, ID2D1Geometry* b, D2D1_COMBINE_MODE mode) {
    ComPtr<ID2D1PathGeometry> pg;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(f->CreatePathGeometry(pg.GetAddressOf())) || FAILED(pg->Open(sink.GetAddressOf()))) return nullptr;
    const HRESULT hr = a->CombineWithGeometry(b, mode, nullptr, sink.Get());
    const HRESULT hc = sink->Close();
    if (FAILED(hr) || FAILED(hc)) return nullptr;
    return pg;
}

// Mark geometry in the 256-unit box; the same numbers are in tools/make_icon.ps1 and assets/brand/moonsr_mark.svg.
constexpr float kMX = 140, kMY = 124, kMR = 80;                // moon disc
constexpr float kCX = 168, kCY = 112, kCR = 71;                // cut disc
constexpr float kRX = 130, kRY = 150, kRA = 116, kRRot = -18;  // orbit ring: lower half of a rotated ellipse band
constexpr float kSX = 160, kSY = 98;                           // star centre
constexpr float kTrails[3][3] = {{-13, 3, 0.30f}, {-25, 6, 0.16f}, {-36, 9, 0.07f}};
// Visible box of the mark (with trails and ring), used to fit it into a rectangle.
constexpr float kBoxL = 16, kBoxT = 40, kBoxR = 244, kBoxB = 216;

D2D1_POINT_2F RingPoint(float x, float y) {  // ellipse-local point -> mark space
    const float a = kRRot * 3.14159265f / 180.0f, c = std::cos(a), s = std::sin(a);
    return D2D1::Point2F(kRX + x * c - y * s, kRY + x * s + y * c);
}

ComPtr<ID2D1Geometry> RingGeometry(ID2D1Factory* f, float outB, float inB) {
    ComPtr<ID2D1PathGeometry> pg;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(f->CreatePathGeometry(pg.GetAddressOf())) || FAILED(pg->Open(sink.GetAddressOf()))) return nullptr;
    // Left end -> bottom of the outer ellipse -> right end, then back along the inner ellipse (quarter arcs, so the arc size
    // is never ambiguous).
    sink->BeginFigure(RingPoint(-kRA, 0), D2D1_FIGURE_BEGIN_FILLED);
    sink->AddArc(D2D1::ArcSegment(RingPoint(0, outB), D2D1::SizeF(kRA, outB), kRRot, D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
    sink->AddArc(D2D1::ArcSegment(RingPoint(kRA, 0), D2D1::SizeF(kRA, outB), kRRot, D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
    sink->AddArc(D2D1::ArcSegment(RingPoint(0, inB), D2D1::SizeF(kRA, inB), kRRot, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
    sink->AddArc(D2D1::ArcSegment(RingPoint(-kRA, 0), D2D1::SizeF(kRA, inB), kRRot, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    if (FAILED(sink->Close())) return nullptr;
    return pg;
}

// Four-point star: tips at (cx, cy -/+ v) and (cx -/+ h, cy), concave sides through (cx +/- k, cy +/- k).
ComPtr<ID2D1Geometry> StarGeometry(ID2D1Factory* f, float cx, float cy, float v, float h, float k) {
    ComPtr<ID2D1PathGeometry> pg;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(f->CreatePathGeometry(pg.GetAddressOf())) || FAILED(pg->Open(sink.GetAddressOf()))) return nullptr;
    const D2D1_POINT_2F tips[4] = {{cx, cy - v}, {cx + h, cy}, {cx, cy + v}, {cx - h, cy}};
    const D2D1_POINT_2F ctl[4] = {{cx + k, cy - k}, {cx + k, cy + k}, {cx - k, cy + k}, {cx - k, cy - k}};
    sink->BeginFigure(tips[0], D2D1_FIGURE_BEGIN_FILLED);
    for (int i = 0; i < 4; ++i) sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(ctl[i], tips[(i + 1) % 4]));
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    if (FAILED(sink->Close())) return nullptr;
    return pg;
}

}  // namespace

// Logo geometry: crescent = moon disc minus the cut disc; rim = the crescent along the cut edge; ring; star. The simple
// variant has a larger moon, a thicker rim and ring and a larger star (matches the 16..32 px icon).
void Gfx::BuildLogoGeometry() {
    ID2D1Factory* f = d2d_.Get();
    for (int v = 0; v < 2; ++v) {
        const bool simple = v == 1;
        const float grow = simple ? 6.0f : 0.0f;
        ComPtr<ID2D1EllipseGeometry> moon, cutDisc;
        f->CreateEllipseGeometry(D2D1::Ellipse(D2D1::Point2F(kMX, kMY), kMR + grow, kMR + grow), moon.GetAddressOf());
        f->CreateEllipseGeometry(D2D1::Ellipse(D2D1::Point2F(kCX, kCY), kCR - grow * 0.4f, kCR - grow * 0.4f), cutDisc.GetAddressOf());
        if (!moon || !cutDisc) continue;
        LogoGeometry& g = logo_[v];
        g.crescent = Combine(f, moon.Get(), cutDisc.Get(), D2D1_COMBINE_MODE_EXCLUDE);
        ComPtr<ID2D1PathGeometry> band;
        ComPtr<ID2D1GeometrySink> sink;
        if (SUCCEEDED(f->CreatePathGeometry(band.GetAddressOf())) && SUCCEEDED(band->Open(sink.GetAddressOf()))) {
            const HRESULT hw = cutDisc->Widen(simple ? 10.0f : 7.0f, nullptr, nullptr, sink.Get());
            if (SUCCEEDED(sink->Close()) && SUCCEEDED(hw) && g.crescent)
                g.rim = Combine(f, g.crescent.Get(), band.Get(), D2D1_COMBINE_MODE_INTERSECT);
        }
        g.ring = simple ? RingGeometry(f, 46.0f, 22.0f) : RingGeometry(f, 40.0f, 25.0f);
        g.star = simple ? StarGeometry(f, kSX + 2.0f, kSY + 2.0f, 36.0f, 28.0f, 5.5f) : StarGeometry(f, kSX, kSY, 27.0f, 21.0f, 3.2f);
    }
}

ComPtr<IDWriteTextFormat> Gfx::MakeDisplayFormat(float size, DWRITE_FONT_WEIGHT weight) const {
    ComPtr<IDWriteTextFormat> f;
    if (FAILED(dw_->CreateTextFormat(displayFamily_.c_str(), nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                     size, L"en-us", f.GetAddressOf())))
        return nullptr;
    f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    return f;
}

ComPtr<IDWriteTextFormat> Gfx::MakeFamilyFormat(bool mono, float size, DWRITE_FONT_WEIGHT weight) const {
    ComPtr<IDWriteTextFormat> f;
    const std::wstring& fam = mono ? monoFamily_ : fluentDisplayFamily_;
    if (FAILED(dw_->CreateTextFormat(fam.c_str(), nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"en-us",
                                     f.GetAddressOf())))
        return nullptr;
    f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    return f;
}

float Gfx::MeasureWidth(const std::wstring& text, Font f) const {
    if (text.empty()) return 0.0f;
    IDWriteTextFormat* fmt = Format(f);
    fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(dw_->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), fmt, 4000.0f, 200.0f,
                                     layout.GetAddressOf())))
        return 0.0f;
    DWRITE_TEXT_METRICS m{};
    layout->GetMetrics(&m);
    return m.widthIncludingTrailingWhitespace;
}

float Gfx::MeasureHeight(const std::wstring& text, Font f, float maxWidth) const {
    IDWriteTextFormat* fmt = Format(f);
    const std::wstring& t = text.empty() ? std::wstring(L" ") : text;
    fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    ComPtr<IDWriteTextLayout> layout;
    float h = 20.0f;
    if (SUCCEEDED(dw_->CreateTextLayout(t.c_str(), static_cast<UINT32>(t.size()), fmt, std::max(maxWidth, 20.0f), 2000.0f,
                                        layout.GetAddressOf()))) {
        DWRITE_TEXT_METRICS m{};
        layout->GetMetrics(&m);
        h = m.height;
    }
    fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    return h;
}

// ---------------------------------------------------------------------------------------------------------------------

bool Canvas::Begin(HWND hwnd, UINT dpi, bool alpha) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    const UINT w = static_cast<UINT>(std::max<LONG>(1, rc.right));
    const UINT h = static_cast<UINT>(std::max<LONG>(1, rc.bottom));
    if (rt_ && (hwnd_ != hwnd || dpi_ != dpi || alpha_ != alpha)) Release();
    if (!rt_) {
        const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, alpha ? D2D1_ALPHA_MODE_PREMULTIPLIED : D2D1_ALPHA_MODE_IGNORE),
            static_cast<float>(dpi), static_cast<float>(dpi));
        const HRESULT hr = gfx_.d2d()->CreateHwndRenderTarget(
            props, D2D1::HwndRenderTargetProperties(hwnd, D2D1::SizeU(w, h), D2D1_PRESENT_OPTIONS_NONE), rt_.GetAddressOf());
        if (FAILED(hr)) {
            MSR_LOG_ERROR(L"CreateHwndRenderTarget failed (0x{:08X})", static_cast<unsigned>(hr));
            return false;
        }
        rt_->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1), brush_.GetAddressOf());
        rt_->SetTextAntialiasMode(alpha ? D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE : D2D1_TEXT_ANTIALIAS_MODE_DEFAULT);
        hwnd_ = hwnd;
        dpi_ = dpi;
        alpha_ = alpha;
    } else {
        const D2D1_SIZE_U sz = rt_->GetPixelSize();
        if (sz.width != w || sz.height != h) rt_->Resize(D2D1::SizeU(w, h));
    }
    clipDepth_ = 0;
    layerDepth_ = 0;
    phosphor_ = false;
    SetStyle(gfx_.radiusCap(), gfx_.borderWidth());
    rt_->BeginDraw();
    rt_->SetTransform(D2D1::Matrix3x2F::Identity());
    return true;
}

bool Canvas::End() {
    if (!rt_) return false;
    while (layerDepth_ > 0) PopOpacity();
    while (clipDepth_ > 0) PopClip();
    const HRESULT hr = rt_->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        MSR_LOG_WARN(L"Direct2D target lost; recreating");
        Release();
        return false;
    }
    if (FAILED(hr)) {
        MSR_LOG_ERROR(L"EndDraw failed (0x{:08X})", static_cast<unsigned>(hr));
        Release();
        return false;
    }
    return true;
}

void Canvas::Release() {
    brush_.Reset();
    dashStroke_.Reset();
    scanBrush_.Reset();
    layer_.Reset();
    rt_.Reset();
    hwnd_ = nullptr;
}

void Canvas::Clear(const D2D1_COLOR_F& c) {
    if (rt_) rt_->Clear(c);
}

bool Canvas::IsGlowColor(const D2D1_COLOR_F& c) const {
    auto matches = [&](const D2D1_COLOR_F& g) {
        return std::fabs(c.r - g.r) + std::fabs(c.g - g.g) + std::fabs(c.b - g.b) < 0.03f && c.a > 0.5f;
    };
    return matches(glowA_) || matches(glowB_);
}

void Canvas::FillRect(const Rect& r, const D2D1_COLOR_F& c) {
    if (!rt_) return;
    if (phosphor_ && IsGlowColor(c)) {  // phosphor bloom: two faint, slightly larger copies under the block
        brush_->SetColor(D2D1_COLOR_F{c.r, c.g, c.b, 0.07f * c.a});
        rt_->FillRectangle(Inflate(r, 3.0f), brush_.Get());
        brush_->SetColor(D2D1_COLOR_F{c.r, c.g, c.b, 0.14f * c.a});
        rt_->FillRectangle(Inflate(r, 1.5f), brush_.Get());
    }
    brush_->SetColor(c);
    rt_->FillRectangle(r, brush_.Get());
}

void Canvas::FillRound(const Rect& r, float radius, const D2D1_COLOR_F& c) {
    if (!rt_) return;
    brush_->SetColor(c);
    if (radiusCap_ >= 0.0f) radius = std::min(radius, radiusCap_);
    if (radius <= 0.0f) {
        rt_->FillRectangle(r, brush_.Get());  // square corners (console theme, or a theme with radius 0)
        return;
    }
    rt_->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), brush_.Get());
}

void Canvas::StrokeRound(const Rect& r, float radius, const D2D1_COLOR_F& c, float width) {
    if (!rt_) return;
    brush_->SetColor(c);
    if (width <= 1.0f) width = borderW_;
    if (radiusCap_ >= 0.0f) radius = std::min(radius, radiusCap_);
    const Rect in = Inflate(r, -width * 0.5f);
    if (radius <= 0.0f) {
        rt_->DrawRectangle(in, brush_.Get(), width);
        return;
    }
    rt_->DrawRoundedRectangle(D2D1::RoundedRect(in, std::max(0.0f, radius - width * 0.5f), std::max(0.0f, radius - width * 0.5f)),
                              brush_.Get(), width);
}

void Canvas::FillCircle(float cx, float cy, float radius, const D2D1_COLOR_F& c) {
    if (!rt_) return;
    brush_->SetColor(c);
    rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), radius, radius), brush_.Get());
}

void Canvas::StrokeCircle(float cx, float cy, float radius, const D2D1_COLOR_F& c, float width) {
    if (!rt_) return;
    brush_->SetColor(c);
    rt_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), radius - width * 0.5f, radius - width * 0.5f), brush_.Get(), width);
}

void Canvas::DashedRect(const Rect& r, const D2D1_COLOR_F& c, float width) {
    if (!rt_) return;
    brush_->SetColor(c);
    if (!dashStroke_) {
        D2D1_STROKE_STYLE_PROPERTIES sp = D2D1::StrokeStyleProperties();
        sp.dashStyle = D2D1_DASH_STYLE_DASH;
        gfx_.d2d()->CreateStrokeStyle(sp, nullptr, 0, dashStroke_.GetAddressOf());
    }
    rt_->DrawRectangle(Inflate(r, -width * 0.5f), brush_.Get(), width, dashStroke_.Get());
}

void Canvas::Line(float x1, float y1, float x2, float y2, const D2D1_COLOR_F& c, float width) {
    if (!rt_) return;
    brush_->SetColor(c);
    rt_->DrawLine(D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2), brush_.Get(), width, gfx_.roundStroke());
}

void Canvas::Chevron(float cx, float cy, float hw, const D2D1_COLOR_F& c, bool up) {
    const float hh = hw * 0.5f;
    const float s = up ? -1.0f : 1.0f;
    Line(cx - hw, cy - hh * s, cx, cy + hh * s, c, 1.4f);
    Line(cx, cy + hh * s, cx + hw, cy - hh * s, c, 1.4f);
}

void Canvas::Check(float cx, float cy, float size, const D2D1_COLOR_F& c) {
    const float h = size * 0.5f;
    Line(cx - h, cy, cx - h * 0.3f, cy + h * 0.7f, c, 1.6f);
    Line(cx - h * 0.3f, cy + h * 0.7f, cx + h, cy - h * 0.6f, c, 1.6f);
}

void Canvas::Text(const std::wstring& s, const Rect& r, Font f, const D2D1_COLOR_F& c, HAlign h, bool wrap) {
    if (!rt_ || s.empty()) return;
    IDWriteTextFormat* fmt = gfx_.Format(f);
    fmt->SetTextAlignment(h == HAlign::Left     ? DWRITE_TEXT_ALIGNMENT_LEADING
                          : h == HAlign::Center ? DWRITE_TEXT_ALIGNMENT_CENTER
                                                : DWRITE_TEXT_ALIGNMENT_TRAILING);
    fmt->SetParagraphAlignment(wrap ? DWRITE_PARAGRAPH_ALIGNMENT_NEAR : DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    fmt->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    if (phosphor_ && IsGlowColor(c)) {
        // Phosphor glow: the text eight times around its position at low alpha gives a soft halo (no effect pipeline
        // needed; the HWND target has no blur).
        static const float kDir[8][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {0.7f, 0.7f}, {-0.7f, 0.7f}, {0.7f, -0.7f}, {-0.7f, -0.7f}};
        brush_->SetColor(D2D1_COLOR_F{c.r, c.g, c.b, 0.085f * c.a});
        for (const auto& d : kDir) {
            const Rect o{r.left + d[0] * 1.6f, r.top + d[1] * 1.6f, r.right + d[0] * 1.6f, r.bottom + d[1] * 1.6f};
            rt_->DrawText(s.c_str(), static_cast<UINT32>(s.size()), fmt, o, brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE,
                          DWRITE_MEASURING_MODE_NATURAL);
        }
    }
    brush_->SetColor(c);
    rt_->DrawText(s.c_str(), static_cast<UINT32>(s.size()), fmt, r, brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP,
                  DWRITE_MEASURING_MODE_NATURAL);
    fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
}

void Canvas::Icon(wchar_t glyph, const Rect& r, Font f, const D2D1_COLOR_F& c) {
    if (!gfx_.hasIcons() || glyph == 0) return;
    if (glyph == kGlyphNeuralNet) {
        NeuralNetGlyph(r, gfx_.Format(f)->GetFontSize(), c);
        return;
    }
    if (glyph == kGlyphReliefLayers) {
        ReliefLayersGlyph(r, gfx_.Format(f)->GetFontSize(), c);
        return;
    }
    if (glyph == 0xE7A1 && !gfx_.hasFluentIcons()) glyph = 0xE9E9; // Contrast uses Equalizer in MDL2.
    Text(std::wstring(1, glyph), r, f, c, HAlign::Center);
}

// ---------------------------------------------------------------------------------------------------------------------
// Brand drawing
// ---------------------------------------------------------------------------------------------------------------------

namespace {
std::vector<D2D1_GRADIENT_STOP> ToStops(const std::vector<Canvas::Stop>& st) {
    std::vector<D2D1_GRADIENT_STOP> gs;
    for (const Canvas::Stop& s : st) gs.push_back(D2D1_GRADIENT_STOP{s.pos, s.color});
    return gs;
}
}  // namespace

void Canvas::FillGradient(const Rect& r, float radius, const std::vector<Stop>& stops, float x0, float y0, float x1, float y1) {
    if (!rt_ || stops.empty()) return;
    const std::vector<D2D1_GRADIENT_STOP> gs = ToStops(stops);
    ComPtr<ID2D1GradientStopCollection> coll;
    if (FAILED(rt_->CreateGradientStopCollection(gs.data(), static_cast<UINT32>(gs.size()), coll.GetAddressOf()))) return;
    ComPtr<ID2D1LinearGradientBrush> b;
    if (FAILED(rt_->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(x0, y0), D2D1::Point2F(x1, y1)),
                                              coll.Get(), b.GetAddressOf())))
        return;
    if (radius > 0.0f) rt_->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), b.Get());
    else rt_->FillRectangle(r, b.Get());
}

void Canvas::LogoMark(const Rect& r, const LogoColors& lc, bool glow) {
    if (!rt_) return;
    const float bw = kBoxR - kBoxL, bh = kBoxB - kBoxT;
    const float s = std::min(RW(r) / bw, RH(r) / bh);
    const bool simple = RH(r) * DpiScale() < 34.0f;  // simplified mark below about 34 device pixels
    const Gfx::LogoGeometry& g = gfx_.logo(simple);
    if (!g.crescent || !g.ring || !g.star) return;
    D2D1_MATRIX_3X2_F old;
    rt_->GetTransform(&old);
    const float ox = r.left + (RW(r) - bw * s) * 0.5f, oy = r.top + (RH(r) - bh * s) * 0.5f;
    const D2D1_MATRIX_3X2_F m = D2D1::Matrix3x2F::Translation(-kBoxL, -kBoxT) * D2D1::Matrix3x2F::Scale(s, s) *
                                D2D1::Matrix3x2F::Translation(ox, oy) * old;
    rt_->SetTransform(m);

    auto linear = [&](std::initializer_list<D2D1_GRADIENT_STOP> st, D2D1_POINT_2F a, D2D1_POINT_2F b) {
        ComPtr<ID2D1GradientStopCollection> coll;
        ComPtr<ID2D1LinearGradientBrush> br;
        if (SUCCEEDED(rt_->CreateGradientStopCollection(st.begin(), static_cast<UINT32>(st.size()), coll.GetAddressOf())))
            rt_->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(a, b), coll.Get(), br.GetAddressOf());
        return br;
    };
    auto halo = [&](float cx, float cy, float rad, const D2D1_COLOR_F& c) {
        const D2D1_COLOR_F z{c.r, c.g, c.b, 0.0f};
        const D2D1_GRADIENT_STOP st[4] = {{0.0f, c}, {0.35f, D2D1_COLOR_F{c.r, c.g, c.b, c.a * 0.65f}}, {0.7f, D2D1_COLOR_F{c.r, c.g, c.b, c.a * 0.22f}}, {1.0f, z}};
        ComPtr<ID2D1GradientStopCollection> coll;
        ComPtr<ID2D1RadialGradientBrush> br;
        if (SUCCEEDED(rt_->CreateGradientStopCollection(st, 4, coll.GetAddressOf())) &&
            SUCCEEDED(rt_->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(D2D1::Point2F(cx, cy), D2D1::Point2F(0, 0), rad, rad),
                                                     coll.Get(), br.GetAddressOf())))
            rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), rad, rad), br.Get());
    };

    if (glow && lc.glow.a > 0.0f) halo(kMX - 6.0f, kMY + 4.0f, kMR * 1.55f, lc.glow);
    if (!simple) {
        for (int i = 2; i >= 0; --i) {
            brush_->SetColor(D2D1_COLOR_F{lc.trail.r, lc.trail.g, lc.trail.b, kTrails[i][2]});
            rt_->SetTransform(D2D1::Matrix3x2F::Translation(kTrails[i][0], kTrails[i][1]) * m);
            rt_->FillGeometry(g.crescent.Get(), brush_.Get());
        }
        rt_->SetTransform(m);
    }
    if (auto mb = linear({{0.0f, lc.moonA}, {0.5f, lc.moonB}, {1.0f, lc.moonC}}, D2D1::Point2F(kMX - kMR, kMY - kMR),
                         D2D1::Point2F(kMX + kMR, kMY + kMR)))
        rt_->FillGeometry(g.crescent.Get(), mb.Get());
    if (g.rim) {
        brush_->SetColor(D2D1_COLOR_F{lc.rim.r, lc.rim.g, lc.rim.b, lc.rim.a * (simple ? 0.75f : 0.85f)});
        rt_->FillGeometry(g.rim.Get(), brush_.Get());
    }
    if (auto rb = linear({{0.0f, lc.ringA}, {1.0f, lc.ringB}}, D2D1::Point2F(kRX - kRA, 0), D2D1::Point2F(kRX + kRA, 0)))
        rt_->FillGeometry(g.ring.Get(), rb.Get());
    if (glow && lc.glow.a > 0.0f && !simple) halo(kSX, kSY, 22.0f, D2D1_COLOR_F{lc.star.r, lc.star.g, lc.star.b, 0.5f});
    brush_->SetColor(lc.star);
    rt_->FillGeometry(g.star.Get(), brush_.Get());
    rt_->SetTransform(old);
}

float Canvas::Wordmark(float x, float y, float size, const D2D1_COLOR_F& moon, const D2D1_COLOR_F& srA, const D2D1_COLOR_F& srB,
                       float* height, int monoOverride) {
    if (!rt_) return 0.0f;
    const bool mono = monoOverride < 0 ? gfx_.retro() : monoOverride != 0;
    const DWRITE_FONT_WEIGHT wt = mono ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_SEMI_BOLD;
    ComPtr<IDWriteTextFormat> fmt = monoOverride < 0 ? gfx_.MakeDisplayFormat(size, wt) : gfx_.MakeFamilyFormat(mono, size, wt);
    if (!fmt) return 0.0f;
    const float spacing = mono ? 0.0f : -0.02f * size;
    float lineH = size * 1.3f;
    auto make = [&](const wchar_t* t, ComPtr<IDWriteTextLayout>& out) {
        const UINT32 n = static_cast<UINT32>(wcslen(t));
        if (FAILED(gfx_.dwrite()->CreateTextLayout(t, n, fmt.Get(), 2000.0f, size * 2.0f, out.GetAddressOf()))) return 0.0f;
        ComPtr<IDWriteTextLayout1> l1;
        if (spacing != 0.0f && SUCCEEDED(out.As(&l1))) l1->SetCharacterSpacing(0.0f, spacing, 0.0f, DWRITE_TEXT_RANGE{0, n});
        DWRITE_TEXT_METRICS tm{};
        out->GetMetrics(&tm);
        lineH = tm.height;
        return tm.widthIncludingTrailingWhitespace;
    };
    ComPtr<IDWriteTextLayout> lm, ls;
    const float wm = make(L"Moon", lm), ws = make(L"SR", ls);
    if (!lm || !ls) return 0.0f;
    if (height) *height = lineH;
    if (phosphor_) {  // halo around the bright "SR" (console themes)
        brush_->SetColor(D2D1_COLOR_F{srB.r, srB.g, srB.b, 0.09f});
        static const float kDir[8][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {0.7f, 0.7f}, {-0.7f, 0.7f}, {0.7f, -0.7f}, {-0.7f, -0.7f}};
        for (const auto& d : kDir) rt_->DrawTextLayout(D2D1::Point2F(x + wm + d[0] * 1.8f, y + d[1] * 1.8f), ls.Get(), brush_.Get());
    }
    brush_->SetColor(moon);
    rt_->DrawTextLayout(D2D1::Point2F(x, y), lm.Get(), brush_.Get());
    const D2D1_GRADIENT_STOP gs[2] = {{0.0f, srA}, {1.0f, srB}};
    ComPtr<ID2D1GradientStopCollection> coll;
    ComPtr<ID2D1LinearGradientBrush> gb;
    if (SUCCEEDED(rt_->CreateGradientStopCollection(gs, 2, coll.GetAddressOf())))
        rt_->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(x + wm, y), D2D1::Point2F(x + wm + ws, y)),
                                       coll.Get(), gb.GetAddressOf());
    if (gb) rt_->DrawTextLayout(D2D1::Point2F(x + wm, y), ls.Get(), gb.Get());
    return wm + ws;
}

void Canvas::Scanlines(const Rect& r, float strength) {
    if (!rt_ || strength <= 0.0f) return;
    if (!scanBrush_) {
        // 1 x 3 device pixels: two clear rows and one dark row; the bitmap's DPI matches the target so it maps 1:1 to pixels.
        const uint32_t px[3] = {0x00000000u, 0x00000000u, 0xFF000000u};
        ComPtr<ID2D1Bitmap> bmp;
        const D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
                                                                 static_cast<float>(dpi_), static_cast<float>(dpi_));
        if (FAILED(rt_->CreateBitmap(D2D1::SizeU(1, 3), px, 4, bp, bmp.GetAddressOf()))) return;
        if (FAILED(rt_->CreateBitmapBrush(bmp.Get(), D2D1::BitmapBrushProperties(D2D1_EXTEND_MODE_WRAP, D2D1_EXTEND_MODE_WRAP,
                                                                                 D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR),
                                          scanBrush_.GetAddressOf())))
            return;
    }
    scanBrush_->SetOpacity(strength);
    D2D1_MATRIX_3X2_F old;
    rt_->GetTransform(&old);
    rt_->SetTransform(D2D1::Matrix3x2F::Identity());
    const D2D1_ANTIALIAS_MODE am = rt_->GetAntialiasMode();
    rt_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
    rt_->FillRectangle(r, scanBrush_.Get());
    rt_->SetAntialiasMode(am);
    rt_->SetTransform(old);
}

void Canvas::PushOpacity(const Rect& r, float alpha) {
    if (!rt_) return;
    if (!layer_ && FAILED(rt_->CreateLayer(nullptr, layer_.GetAddressOf()))) return;
    rt_->PushLayer(D2D1::LayerParameters(r, nullptr, D2D1_ANTIALIAS_MODE_ALIASED, D2D1::IdentityMatrix(), std::clamp(alpha, 0.0f, 1.0f)),
                   layer_.Get());
    ++layerDepth_;
}

void Canvas::PopOpacity() {
    if (!rt_ || layerDepth_ <= 0) return;
    rt_->PopLayer();
    --layerDepth_;
}

float Canvas::SpacedText(const std::wstring& s, float x, float y, float size, DWRITE_FONT_WEIGHT weight, const D2D1_COLOR_F& c,
                         float spacing) {
    if (!rt_ || s.empty()) return 0.0f;
    ComPtr<IDWriteTextFormat> fmt = gfx_.MakeDisplayFormat(size, weight);
    ComPtr<IDWriteTextLayout> l;
    if (!fmt || FAILED(gfx_.dwrite()->CreateTextLayout(s.c_str(), static_cast<UINT32>(s.size()), fmt.Get(), 4000.0f, size * 2.0f,
                                                       l.GetAddressOf())))
        return 0.0f;
    ComPtr<IDWriteTextLayout1> l1;
    if (SUCCEEDED(l.As(&l1))) l1->SetCharacterSpacing(0.0f, spacing, 0.0f, DWRITE_TEXT_RANGE{0, static_cast<UINT32>(s.size())});
    DWRITE_TEXT_METRICS m{};
    l->GetMetrics(&m);
    brush_->SetColor(c);
    rt_->DrawTextLayout(D2D1::Point2F(x, y), l.Get(), brush_.Get());
    return m.widthIncludingTrailingWhitespace;
}

// A three-layer network (2, 3 and 2 nodes, every node joined to the next layer) on the Segoe Fluent icon grid: the drawing
// spans about 16/18 of the em, lines are as thick as the font's 1 px outline strokes and the nodes are solid dots.
void Canvas::NeuralNetGlyph(const Rect& r, float em, const D2D1_COLOR_F& c) {
    if (!rt_) return;
    const float u = em / 18.0f;  // the nav icons are 18 DIP glyphs
    const float cx = (r.left + r.right) * 0.5f, cy = (r.top + r.bottom) * 0.5f;
    const float dx = 7.0f * u, outer = 3.9f * u, inner = 7.0f * u, node = 1.65f * u, line = std::max(0.8f, 0.85f * u);
    const D2D1_POINT_2F l0[2] = {{cx - dx, cy - outer}, {cx - dx, cy + outer}};
    const D2D1_POINT_2F l1[3] = {{cx, cy - inner}, {cx, cy}, {cx, cy + inner}};
    const D2D1_POINT_2F l2[2] = {{cx + dx, cy - outer}, {cx + dx, cy + outer}};
    brush_->SetColor(D2D1_COLOR_F{c.r, c.g, c.b, c.a * 0.72f});
    for (const auto& a : l0)
        for (const auto& b : l1) rt_->DrawLine(a, b, brush_.Get(), line, gfx_.roundStroke());
    for (const auto& a : l1)
        for (const auto& b : l2) rt_->DrawLine(a, b, brush_.Get(), line, gfx_.roundStroke());
    brush_->SetColor(c);
    for (const auto& p : l0) rt_->FillEllipse(D2D1::Ellipse(p, node, node), brush_.Get());
    for (const auto& p : l1) rt_->FillEllipse(D2D1::Ellipse(p, node, node), brush_.Get());
    for (const auto& p : l2) rt_->FillEllipse(D2D1::Ellipse(p, node, node), brush_.Get());
}

// Smart parallax: three flat layers seen at an angle, like the familiar "layers" sign, but each lower layer is shifted
// sideways so the stack reads as depth with parallax. The top layer is a full rhombus; the two below show only their
// lower edges (they are hidden behind it) and fade with distance. Same grid and stroke weight as NeuralNetGlyph.
void Canvas::ReliefLayersGlyph(const Rect& r, float em, const D2D1_COLOR_F& c) {
    if (!rt_) return;
    const float u = em / 18.0f;  // the nav icons are 18 DIP glyphs
    const float cx = (r.left + r.right) * 0.5f, cy = (r.top + r.bottom) * 0.5f;
    const float hw = 6.6f * u, hh = 3.4f * u, step = 3.5f * u, shift = 1.3f * u, line = std::max(0.8f, 0.9f * u);
    const float top = cy - step;  // centre of the top layer; the stack spans about cy - 6.9u .. cy + 6.9u
    // Lower layers first (drawn underneath), farthest = most shifted and faintest.
    const float alpha[2] = {0.72f, 0.45f};
    for (int k = 2; k >= 1; --k) {
        const float x = cx + shift * static_cast<float>(k) - shift, y = top + step * static_cast<float>(k);
        brush_->SetColor(D2D1_COLOR_F{c.r, c.g, c.b, c.a * alpha[k - 1]});
        rt_->DrawLine(D2D1::Point2F(x - hw, y), D2D1::Point2F(x, y + hh), brush_.Get(), line, gfx_.roundStroke());
        rt_->DrawLine(D2D1::Point2F(x, y + hh), D2D1::Point2F(x + hw, y), brush_.Get(), line, gfx_.roundStroke());
    }
    const float x0 = cx - shift;
    const D2D1_POINT_2F d[4] = {{x0 - hw, top}, {x0, top - hh}, {x0 + hw, top}, {x0, top + hh}};
    brush_->SetColor(c);
    for (int i = 0; i < 4; ++i) rt_->DrawLine(d[i], d[(i + 1) % 4], brush_.Get(), line, gfx_.roundStroke());
    rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x0, top), 1.1f * u, 1.1f * u), brush_.Get());  // surface point
}

void Canvas::PushClip(const Rect& r) {
    if (!rt_) return;
    rt_->PushAxisAlignedClip(r, D2D1_ANTIALIAS_MODE_ALIASED);
    ++clipDepth_;
}

void Canvas::PopClip() {
    if (!rt_ || clipDepth_ <= 0) return;
    rt_->PopAxisAlignedClip();
    --clipDepth_;
}

void Canvas::SetOffset(float dx, float dy) {
    if (rt_) rt_->SetTransform(D2D1::Matrix3x2F::Translation(dx, dy));
}

}  // namespace moonsr::app::ui
