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

// Returns `a` minus `b` as a path geometry.
ComPtr<ID2D1Geometry> Subtract(ID2D1Factory* f, ID2D1Geometry* a, ID2D1Geometry* b) {
    ComPtr<ID2D1PathGeometry> pg;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(f->CreatePathGeometry(pg.GetAddressOf())) || FAILED(pg->Open(sink.GetAddressOf()))) return nullptr;
    const HRESULT hr = a->CombineWithGeometry(b, D2D1_COMBINE_MODE_EXCLUDE, nullptr, sink.Get());
    const HRESULT hc = sink->Close();
    if (FAILED(hr) || FAILED(hc)) return nullptr;
    return pg;
}

ComPtr<ID2D1Geometry> Bezier(ID2D1Factory* f, D2D1_POINT_2F p0, D2D1_POINT_2F c1, D2D1_POINT_2F c2, D2D1_POINT_2F p1) {
    ComPtr<ID2D1PathGeometry> pg;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(f->CreatePathGeometry(pg.GetAddressOf())) || FAILED(pg->Open(sink.GetAddressOf()))) return nullptr;
    sink->BeginFigure(p0, D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddBezier(D2D1::BezierSegment(c1, c2, p1));
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    if (FAILED(sink->Close())) return nullptr;
    return pg;
}

}  // namespace

// Logo geometry in the unit space of assets/brand/moonsr_mockup.svg. The mockup's dark "cut" circle (#20233F) is a real
// cut here: the top-right tile and the moon circle both have it subtracted, so the mark works on any background.
void Gfx::BuildLogoGeometry() {
    ComPtr<ID2D1EllipseGeometry> cut, moon;
    d2d_->CreateEllipseGeometry(D2D1::Ellipse(D2D1::Point2F(291, 76), 35, 35), cut.GetAddressOf());
    d2d_->CreateEllipseGeometry(D2D1::Ellipse(D2D1::Point2F(275, 89), 36, 36), moon.GetAddressOf());
    if (!cut || !moon) return;
    for (int i = 0; i < 2; ++i) {
        const float in = i ? 5.0f : 0.0f;
        ComPtr<ID2D1RoundedRectangleGeometry> tile;
        d2d_->CreateRoundedRectangleGeometry(
            D2D1::RoundedRect(D2D1::RectF(207 + in, 72 + in, 307 - in, 195 - in), 29 - in * 0.5f, 29 - in * 0.5f),
            tile.GetAddressOf());
        if (tile) tileTR_[i] = Subtract(d2d_.Get(), tile.Get(), cut.Get());
    }
    crescent_ = Subtract(d2d_.Get(), moon.Get(), cut.Get());
    highlight_[0] = Bezier(d2d_.Get(), D2D1::Point2F(104, 97), D2D1::Point2F(128, 80), D2D1::Point2F(156, 82), D2D1::Point2F(177, 93));
    highlight_[1] = Bezier(d2d_.Get(), D2D1::Point2F(224, 97), D2D1::Point2F(241, 85), D2D1::Point2F(263, 84), D2D1::Point2F(282, 92));
}

ComPtr<IDWriteTextFormat> Gfx::MakeDisplayFormat(float size, DWRITE_FONT_WEIGHT weight) const {
    ComPtr<IDWriteTextFormat> f;
    if (FAILED(dw_->CreateTextFormat(displayFamily_.c_str(), nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                     size, L"en-us", f.GetAddressOf())))
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
    SetStyle(gfx_.radiusCap(), gfx_.borderWidth());
    rt_->BeginDraw();
    rt_->SetTransform(D2D1::Matrix3x2F::Identity());
    return true;
}

bool Canvas::End() {
    if (!rt_) return false;
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
    rt_.Reset();
    hwnd_ = nullptr;
}

void Canvas::Clear(const D2D1_COLOR_F& c) {
    if (rt_) rt_->Clear(c);
}

void Canvas::FillRect(const Rect& r, const D2D1_COLOR_F& c) {
    if (!rt_) return;
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
    brush_->SetColor(c);
    rt_->DrawText(s.c_str(), static_cast<UINT32>(s.size()), fmt, r, brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP,
                  DWRITE_MEASURING_MODE_NATURAL);
    fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
}

void Canvas::Icon(wchar_t glyph, const Rect& r, Font f, const D2D1_COLOR_F& c) {
    if (!gfx_.hasIcons() || glyph == 0) return;
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

void Canvas::LogoMark(const Rect& r, bool detailed, bool onLight) {
    if (!rt_) return;
    // Unit space of the SVG: the glass grid spans (74,72)-(307,307).
    const float s = std::min(RW(r), RH(r)) / 233.0f;
    D2D1_MATRIX_3X2_F old;
    rt_->GetTransform(&old);
    const float ox = r.left + (RW(r) - 233.0f * s) * 0.5f, oy = r.top + (RH(r) - 233.0f * s) * 0.5f;
    rt_->SetTransform(D2D1::Matrix3x2F::Translation(-74.0f, -72.0f) * D2D1::Matrix3x2F::Scale(s, s) *
                      D2D1::Matrix3x2F::Translation(ox, oy) * old);

    auto linear = [&](const std::vector<Stop>& st, D2D1_POINT_2F a, D2D1_POINT_2F b) {
        const std::vector<D2D1_GRADIENT_STOP> gs = ToStops(st);
        ComPtr<ID2D1GradientStopCollection> coll;
        ComPtr<ID2D1LinearGradientBrush> br;
        if (SUCCEEDED(rt_->CreateGradientStopCollection(gs.data(), static_cast<UINT32>(gs.size()), coll.GetAddressOf())))
            rt_->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(a, b), coll.Get(), br.GetAddressOf());
        return br;
    };
    auto radial = [&](const std::vector<Stop>& st, D2D1_POINT_2F c, float rx, float ry) {
        const std::vector<D2D1_GRADIENT_STOP> gs = ToStops(st);
        ComPtr<ID2D1GradientStopCollection> coll;
        ComPtr<ID2D1RadialGradientBrush> br;
        if (SUCCEEDED(rt_->CreateGradientStopCollection(gs.data(), static_cast<UINT32>(gs.size()), coll.GetAddressOf())))
            rt_->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(c, D2D1::Point2F(0, 0), rx, ry), coll.Get(),
                                           br.GetAddressOf());
        return br;
    };

    if (detailed) {
        // Halo (radial, clipped to a circle) and the blurred glow disc (erf profile = gaussian-blurred circle r109).
        if (auto halo = radial({{0.0f, Rgb(0x45BFFF, 0.30f)}, {1.0f, Rgb(0x536AFF, 0.0f)}}, D2D1::Point2F(199, 176), 175, 139))
            rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(197, 177), 157, 157), halo.Get());
        std::vector<Stop> gl;
        for (int i = 0; i <= 12; ++i) {
            const float t = static_cast<float>(i) / 12.0f, d = 166.0f * t;
            gl.push_back({t, Rgb(0x588DFF, 0.65f * 0.5f * static_cast<float>(std::erfc((d - 109.0) / (19.0 * 1.41421356))))});
        }
        if (auto glow = radial(gl, D2D1::Point2F(191, 190), 166, 166))
            rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(191, 190), 166, 166), glow.Get());
        // Drop shadow of the tiles: a few expanding translucent layers (a cheap stand-in for the blurred alpha).
        const float rcs[4][5] = {{74, 72, 123, 123, 31}, {207, 72, 100, 123, 29}, {74, 207, 123, 100, 29}, {207, 207, 100, 100, 28}};
        const float ex[5] = {-3.0f, 2.0f, 7.0f, 12.0f, 17.0f}, al[5] = {0.060f, 0.045f, 0.035f, 0.030f, 0.022f};
        for (int k = 4; k >= 0; --k)
            for (const auto& q : rcs)
                FillRound(Rect{q[0] - ex[k], q[1] + 11.0f - ex[k], q[0] + q[2] + ex[k], q[1] + q[3] + 11.0f + ex[k]}, q[4] + ex[k],
                          D2D1_COLOR_F{0, 0, 0, al[k]});
    }

    const float in = detailed ? 0.0f : 5.0f;
    const uint32_t c0 = detailed ? 0xB8F5FF : 0x7DDDFF;
    const float a0 = detailed ? 0.88f : 1.0f, a1 = detailed ? 0.73f : 0.95f, a2 = detailed ? 0.82f : 0.97f;
    // On a light surface the small mark uses deeper tile colours so it keeps its contrast.
    const bool deep = onLight && !detailed;
    auto glass = linear({{0.0f, Rgb(deep ? 0x2FA6EA : c0, a0)}, {0.45f, Rgb(deep ? 0x3A93E6 : 0x5BC7FF, a1)}, {1.0f, Rgb(deep ? 0x6A5CE3 : 0x8279FF, a2)}},
                        D2D1::Point2F(67, 49), D2D1::Point2F(309, 307));
    if (glass) {
        const float op[4] = {1.0f, detailed ? 0.78f : 0.92f, detailed ? 0.69f : 0.88f, detailed ? 0.57f : 0.84f};
        const float rr[4][5] = {{74, 72, 123, 123, 31}, {207, 72, 100, 123, 29}, {74, 207, 123, 100, 29}, {207, 207, 100, 100, 28}};
        for (int i = 0; i < 4; ++i) {
            glass->SetOpacity(op[i]);
            if (i == 1) {
                if (gfx_.logoTopRightTile(!detailed)) rt_->FillGeometry(gfx_.logoTopRightTile(!detailed), glass.Get());
            } else {
                const float* q = rr[i];
                const float rad = q[4] - in * 0.5f;
                rt_->FillRoundedRectangle(
                    D2D1::RoundedRect(D2D1::RectF(q[0] + in, q[1] + in, q[0] + q[2] - in, q[1] + q[3] - in), rad, rad), glass.Get());
            }
        }
    }
    if (detailed) {
        brush_->SetColor(Rgb(0xFFFFFF, 0.55f));
        if (gfx_.logoHighlight(0)) rt_->DrawGeometry(gfx_.logoHighlight(0), brush_.Get(), 5.0f, gfx_.roundStroke());
        brush_->SetColor(Rgb(0xFFFFFF, 0.45f));
        if (gfx_.logoHighlight(1)) rt_->DrawGeometry(gfx_.logoHighlight(1), brush_.Get(), 5.0f, gfx_.roundStroke());
    }
    brush_->SetColor(Rgb(0xFFF2C7));
    if (gfx_.logoCrescent()) rt_->FillGeometry(gfx_.logoCrescent(), brush_.Get());
    brush_->SetColor(Rgb(0xF4FBFF));
    const float cw = detailed ? 8.0f : 11.0f;
    rt_->DrawLine(D2D1::Point2F(240, 258), D2D1::Point2F(254, 272), brush_.Get(), cw, gfx_.roundStroke());
    rt_->DrawLine(D2D1::Point2F(254, 272), D2D1::Point2F(285, 236), brush_.Get(), cw, gfx_.roundStroke());
    rt_->SetTransform(old);
}

float Canvas::Wordmark(float x, float y, float size, const D2D1_COLOR_F& moonA, const D2D1_COLOR_F& moonB, const D2D1_COLOR_F& sr) {
    if (!rt_) return 0.0f;
    ComPtr<IDWriteTextFormat> fmt = gfx_.MakeDisplayFormat(size, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    if (!fmt) return 0.0f;
    const float spacing = -0.04f * size;
    auto make = [&](const wchar_t* t, ComPtr<IDWriteTextLayout>& out) {
        const UINT32 n = static_cast<UINT32>(wcslen(t));
        if (FAILED(gfx_.dwrite()->CreateTextLayout(t, n, fmt.Get(), 2000.0f, size * 2.0f, out.GetAddressOf()))) return 0.0f;
        ComPtr<IDWriteTextLayout1> l1;
        if (SUCCEEDED(out.As(&l1))) l1->SetCharacterSpacing(0.0f, spacing, 0.0f, DWRITE_TEXT_RANGE{0, n});
        DWRITE_TEXT_METRICS m{};
        out->GetMetrics(&m);
        return m.widthIncludingTrailingWhitespace;
    };
    ComPtr<IDWriteTextLayout> lm, ls;
    const float wm = make(L"Moon", lm), ws = make(L"SR", ls);
    if (!lm || !ls) return 0.0f;
    const D2D1_GRADIENT_STOP gs[2] = {{0.0f, moonA}, {1.0f, moonB}};
    ComPtr<ID2D1GradientStopCollection> coll;
    ComPtr<ID2D1LinearGradientBrush> gb;
    if (SUCCEEDED(rt_->CreateGradientStopCollection(gs, 2, coll.GetAddressOf())))
        rt_->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(x, y), D2D1::Point2F(x + wm + ws, y + size)),
                                       coll.Get(), gb.GetAddressOf());
    if (gb) rt_->DrawTextLayout(D2D1::Point2F(x, y), lm.Get(), gb.Get());
    brush_->SetColor(sr);
    rt_->DrawTextLayout(D2D1::Point2F(x + wm, y), ls.Get(), brush_.Get());
    return wm + ws;
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
