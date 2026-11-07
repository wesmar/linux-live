#include "gui/Theme.h"
#include <dwmapi.h>
#include <uxtheme.h>
#include <algorithm>
#include <cmath>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "gdi32.lib")

namespace DL::Ui {

COLORREF ToColorRef(D2D1_COLOR_F c) {
    auto byte = [](float v) { return static_cast<BYTE>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
    return RGB(byte(c.r), byte(c.g), byte(c.b));
}

// ---------------------------------------------------------------------------
// Graphics
// ---------------------------------------------------------------------------
Graphics& Graphics::Get() {
    static Graphics instance;
    return instance;
}

Graphics::Graphics() {
    D2D1_FACTORY_OPTIONS options{};
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), &options,
                      reinterpret_cast<void**>(m_d2d.GetAddressOf()));
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                        reinterpret_cast<IUnknown**>(m_dwrite.GetAddressOf()));
}

Graphics::~Graphics() {
    if (m_memDc && m_oldBitmap) SelectObject(m_memDc.get(), m_oldBitmap);
    m_dib.reset();
    m_memDc.reset();
}

ID2D1DCRenderTarget* Graphics::Target() {
    if (!m_target && m_d2d) {
        // Software rendering: no GPU read-back per paint and no device-loss handling for a small UI.
        const auto props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            96.0f, 96.0f);
        m_d2d->CreateDCRenderTarget(&props, m_target.GetAddressOf());
    }
    return m_target.Get();
}

bool Graphics::HasFamily(const wchar_t* family) const {
    if (!m_dwrite) return false;
    ComPtr<IDWriteFontCollection> fonts;
    if (FAILED(m_dwrite->GetSystemFontCollection(fonts.GetAddressOf(), FALSE))) return false;
    UINT32 index = 0;
    BOOL exists = FALSE;
    return SUCCEEDED(fonts->FindFamilyName(family, &index, &exists)) && exists;
}

D2D1_COLOR_F Graphics::Surface(D2D1_COLOR_F layer) const noexcept {
    if (mica) return layer;
    const D2D1_COLOR_F b = Color::OpaqueBase;
    return {layer.r * layer.a + b.r * (1 - layer.a), layer.g * layer.a + b.g * (1 - layer.a),
            layer.b * layer.a + b.b * (1 - layer.a), 1.0f};
}

COLORREF Graphics::GdiSurface(D2D1_COLOR_F layer) const noexcept {
    if (!mica) return ToColorRef(Surface(layer));
    // Premultiplied value with alpha 0: DWM adds it to the backdrop, matching the D2D layer.
    return ToColorRef({layer.r * layer.a, layer.g * layer.a, layer.b * layer.a, 1.0f});
}

HDC Graphics::Buffer(int width, int height) {
    width = std::max(width, 1);
    height = std::max(height, 1);
    if (m_memDc && width <= m_bufferWidth && height <= m_bufferHeight) return m_memDc.get();
    if (!m_memDc) m_memDc.reset(CreateCompatibleDC(nullptr));
    if (!m_memDc) return nullptr;
    if (m_oldBitmap) SelectObject(m_memDc.get(), m_oldBitmap);
    m_oldBitmap = nullptr;
    m_dib.reset();
    // Round up to avoid reallocating on every pixel of a window resize.
    m_bufferWidth = std::max(m_bufferWidth, (width + 255) & ~255);
    m_bufferHeight = std::max(m_bufferHeight, (height + 255) & ~255);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof info.bmiHeader;
    info.bmiHeader.biWidth = m_bufferWidth;
    info.bmiHeader.biHeight = -m_bufferHeight;  // top-down
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    m_dib.reset(CreateDIBSection(m_memDc.get(), &info, DIB_RGB_COLORS, &bits, nullptr, 0));
    if (!m_dib) return nullptr;
    m_oldBitmap = SelectObject(m_memDc.get(), m_dib.get());
    return m_memDc.get();
}

// ---------------------------------------------------------------------------
// Fonts
// ---------------------------------------------------------------------------
void Fonts::Create(UINT dpi) {
    m_dpi = dpi ? dpi : 96;
    auto& g = Graphics::Get();
    if (!g.DWrite()) return;
    m_uiFamily = g.HasFamily(L"Segoe UI Variable Text") ? L"Segoe UI Variable Text" : L"Segoe UI";
    const wchar_t* display = g.HasFamily(L"Segoe UI Variable Display") ? L"Segoe UI Variable Display" : L"Segoe UI";
    const wchar_t* mono = g.HasFamily(L"Cascadia Mono") ? L"Cascadia Mono" : L"Consolas";
    const wchar_t* icons = g.HasFamily(L"Segoe Fluent Icons") ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets";

    struct Spec {
        Font font;
        const wchar_t* family;
        float size;
        DWRITE_FONT_WEIGHT weight;
    };
    const Spec specs[] = {
        {Font::Body, m_uiFamily, 13.0f, DWRITE_FONT_WEIGHT_NORMAL},
        {Font::Strong, m_uiFamily, 13.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD},
        {Font::Caption, m_uiFamily, 11.5f, DWRITE_FONT_WEIGHT_NORMAL},
        {Font::Label, m_uiFamily, 10.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD},
        {Font::Mono, mono, 11.5f, DWRITE_FONT_WEIGHT_NORMAL},
        {Font::Icon, icons, 13.0f, DWRITE_FONT_WEIGHT_NORMAL},
        {Font::IconSmall, icons, 10.0f, DWRITE_FONT_WEIGHT_NORMAL},
        {Font::Title, display, 19.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD},
    };
    ComPtr<IDWriteInlineObject> ellipsis;
    for (const Spec& s : specs) {
        auto& slot = m_formats[static_cast<std::size_t>(s.font)];
        slot.Reset();
        const float size = F(s.size);
        m_sizes[static_cast<std::size_t>(s.font)] = size;
        if (FAILED(g.DWrite()->CreateTextFormat(s.family, nullptr, s.weight, DWRITE_FONT_STYLE_NORMAL,
                                                DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", slot.GetAddressOf())))
            continue;
        slot->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        slot->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        if (g.DWrite()->CreateEllipsisTrimmingSign(slot.Get(), ellipsis.ReleaseAndGetAddressOf()) == S_OK) {
            const DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            slot->SetTrimming(&trimming, ellipsis.Get());
        }
    }
}

// ---------------------------------------------------------------------------
// Canvas
// ---------------------------------------------------------------------------
Canvas::Canvas(ID2D1RenderTarget& target, const Fonts& fonts) : m_rt(target), m_fonts(fonts) {}

ID2D1SolidColorBrush* Canvas::Brush(D2D1_COLOR_F c) {
    if (!m_brush) m_rt.CreateSolidColorBrush(c, m_brush.GetAddressOf());
    else m_brush->SetColor(c);
    return m_brush.Get();
}

void Canvas::Clear(D2D1_COLOR_F c) { m_rt.Clear(c); }

void Canvas::Fill(const D2D1_RECT_F& r, D2D1_COLOR_F c, float radius) {
    if (r.right <= r.left || r.bottom <= r.top) return;
    if (radius > 0) m_rt.FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), Brush(c));
    else m_rt.FillRectangle(r, Brush(c));
}

void Canvas::Stroke(const D2D1_RECT_F& r, D2D1_COLOR_F c, float radius, float width) {
    const D2D1_RECT_F in = Inset(r, width / 2, width / 2);  // keep the stroke inside, on pixel centres
    if (in.right <= in.left || in.bottom <= in.top) return;
    if (radius > 0) m_rt.DrawRoundedRectangle(D2D1::RoundedRect(in, radius, radius), Brush(c), width);
    else m_rt.DrawRectangle(in, Brush(c), width);
}

void Canvas::Line(D2D1_POINT_2F a, D2D1_POINT_2F b, D2D1_COLOR_F c, float width) { m_rt.DrawLine(a, b, Brush(c), width); }

void Canvas::Circle(D2D1_POINT_2F center, float radius, D2D1_COLOR_F c) {
    m_rt.FillEllipse(D2D1::Ellipse(center, radius, radius), Brush(c));
}

void Canvas::Text(std::wstring_view text, const D2D1_RECT_F& r, Font font, D2D1_COLOR_F c, Align align) {
    IDWriteTextFormat* format = m_fonts[font];
    if (!format || text.empty() || r.right <= r.left) return;
    format->SetTextAlignment(align == Align::Center ? DWRITE_TEXT_ALIGNMENT_CENTER
                             : align == Align::Right ? DWRITE_TEXT_ALIGNMENT_TRAILING
                                                     : DWRITE_TEXT_ALIGNMENT_LEADING);
    m_rt.DrawText(text.data(), static_cast<UINT32>(text.size()), format, r, Brush(c), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

namespace {
ComPtr<IDWriteTextLayout> Layout(IDWriteTextFormat* format, std::wstring_view text, float width, bool wrap) {
    ComPtr<IDWriteTextLayout> layout;
    auto* dw = Graphics::Get().DWrite();
    if (!dw || !format) return layout;
    if (FAILED(dw->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()), format, std::max(width, 1.0f), 1.0e6f,
                                    layout.GetAddressOf())))
        return layout;
    layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    if (wrap) {
        layout->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        const DWRITE_TRIMMING none{DWRITE_TRIMMING_GRANULARITY_NONE, 0, 0};
        layout->SetTrimming(&none, nullptr);
    }
    return layout;
}
}  // namespace

float TextWidth(const Fonts& fonts, Font font, std::wstring_view text) {
    auto layout = Layout(fonts[font], text, 1.0e6f, false);
    DWRITE_TEXT_METRICS m{};
    if (!layout || FAILED(layout->GetMetrics(&m))) return 0;
    return m.widthIncludingTrailingWhitespace;
}

float Canvas::Width(std::wstring_view text, Font font) { return TextWidth(m_fonts, font, text); }

float TextHeight(const Fonts& fonts, Font font, std::wstring_view text, float width) {
    auto layout = Layout(fonts[font], text, width, true);
    DWRITE_TEXT_METRICS m{};
    if (!layout || FAILED(layout->GetMetrics(&m))) return 0;
    return m.height;
}

float Canvas::ParagraphHeight(std::wstring_view text, float width, Font font) { return TextHeight(m_fonts, font, text, width); }

float Canvas::Paragraph(std::wstring_view text, const D2D1_RECT_F& r, Font font, D2D1_COLOR_F c) {
    auto layout = Layout(m_fonts[font], text, r.right - r.left, true);
    if (!layout) return 0;
    m_rt.DrawTextLayout({r.left, r.top}, layout.Get(), Brush(c), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    DWRITE_TEXT_METRICS m{};
    layout->GetMetrics(&m);
    return m.height;
}

void Canvas::PushClip(const D2D1_RECT_F& r) { m_rt.PushAxisAlignedClip(r, D2D1_ANTIALIAS_MODE_ALIASED); }
void Canvas::PopClip() { m_rt.PopAxisAlignedClip(); }

// ---------------------------------------------------------------------------
// Paint
// ---------------------------------------------------------------------------
void Paint(HDC dc, const RECT& rect, const Fonts& fonts, const std::function<void(Canvas&)>& draw,
           const std::function<void(HDC)>& gdi) {
    const int width = rect.right - rect.left, height = rect.bottom - rect.top;
    if (width <= 0 || height <= 0) return;
    auto& g = Graphics::Get();
    HDC buffer = g.Buffer(width, height);
    ID2D1DCRenderTarget* target = g.Target();
    if (!buffer || !target) return;

    const RECT local{0, 0, width, height};
    if (FAILED(target->BindDC(buffer, &local))) return;
    target->BeginDraw();
    target->SetTransform(D2D1::Matrix3x2F::Translation(static_cast<float>(-rect.left), static_cast<float>(-rect.top)));
    target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    {
        Canvas canvas(*target, fonts);
        draw(canvas);
    }
    if (target->EndDraw() == D2DERR_RECREATE_TARGET) g.ResetTarget();
    if (gdi) {
        const int saved = SaveDC(buffer);
        SetViewportOrgEx(buffer, -rect.left, -rect.top, nullptr);
        gdi(buffer);
        RestoreDC(buffer, saved);
    }
    BitBlt(dc, rect.left, rect.top, width, height, buffer, 0, 0, SRCCOPY);
}

// ---------------------------------------------------------------------------
// Window backdrop
// ---------------------------------------------------------------------------
void ApplyBackdrop(HWND window) {
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(window, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof dark);
    HIGHCONTRASTW contrast{sizeof contrast};
    const bool highContrast = SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof contrast, &contrast, 0) &&
                              (contrast.dwFlags & HCF_HIGHCONTRASTON);
    auto& g = Graphics::Get();
    const DWM_SYSTEMBACKDROP_TYPE backdrop = highContrast ? DWMSBT_NONE : DWMSBT_MAINWINDOW;
    g.mica = SUCCEEDED(DwmSetWindowAttribute(window, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof backdrop)) && !highContrast;
    const MARGINS margins = g.mica ? MARGINS{-1, -1, -1, -1} : MARGINS{0, 0, 0, 0};
    DwmExtendFrameIntoClientArea(window, &margins);
    if (!g.mica) {
        const COLORREF caption = ToColorRef(Color::OpaqueBase);  // Windows 11 without Mica: seamless caption
        DwmSetWindowAttribute(window, DWMWA_CAPTION_COLOR, &caption, sizeof caption);
    }
}

}  // namespace DL::Ui
