// Rendering foundation: palette, DPI-scaled DirectWrite fonts and a Direct2D canvas.
//
// Mica needs per-pixel alpha in the window's redirection surface, which GDI cannot produce
// (it writes alpha 0). Every surface is therefore painted with Direct2D into a premultiplied
// 32-bit DIB and copied to the window as a whole: transparent pixels show the Mica backdrop,
// cards are translucent layers on top of it, text is anti-aliased with correct alpha.
#pragma once
#include "support/Handle.h"
#include <Windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <array>
#include <functional>
#include <string_view>

namespace DL::Ui {

using Microsoft::WRL::ComPtr;

constexpr D2D1_COLOR_F Rgb(unsigned hex, float alpha = 1.0f) {
    return {static_cast<float>((hex >> 16) & 0xFF) / 255.0f, static_cast<float>((hex >> 8) & 0xFF) / 255.0f,
            static_cast<float>(hex & 0xFF) / 255.0f, alpha};
}
constexpr D2D1_COLOR_F White(float alpha) { return {1.0f, 1.0f, 1.0f, alpha}; }
constexpr D2D1_COLOR_F Black(float alpha) { return {0.0f, 0.0f, 0.0f, alpha}; }

// Windows 11 dark-theme layer values, with the Linux red as the accent.
namespace Color {
inline constexpr D2D1_COLOR_F Card = White(0.050f);
inline constexpr D2D1_COLOR_F CardStroke = White(0.075f);
inline constexpr D2D1_COLOR_F Control = White(0.061f);
inline constexpr D2D1_COLOR_F ControlHot = White(0.090f);
inline constexpr D2D1_COLOR_F ControlPressed = White(0.035f);
inline constexpr D2D1_COLOR_F ControlDisabled = White(0.040f);
inline constexpr D2D1_COLOR_F ControlStroke = White(0.075f);
inline constexpr D2D1_COLOR_F Text = White(1.000f);
inline constexpr D2D1_COLOR_F Text2 = White(0.786f);
inline constexpr D2D1_COLOR_F Text3 = White(0.545f);
inline constexpr D2D1_COLOR_F TextDisabled = White(0.363f);
inline constexpr D2D1_COLOR_F Accent = Rgb(0xD70A53);
inline constexpr D2D1_COLOR_F AccentHot = Rgb(0xE5336F);
inline constexpr D2D1_COLOR_F AccentPressed = Rgb(0xB30845);
inline constexpr D2D1_COLOR_F AccentText = Rgb(0xFF6F9C);  // accent for text on dark layers
inline constexpr D2D1_COLOR_F Success = Rgb(0x6CCB5F);
inline constexpr D2D1_COLOR_F Caution = Rgb(0xFCE100);
inline constexpr D2D1_COLOR_F Critical = Rgb(0xFF99A4);
inline constexpr D2D1_COLOR_F Hover = White(0.045f);
inline constexpr D2D1_COLOR_F Selected = White(0.080f);
inline constexpr D2D1_COLOR_F Track = White(0.100f);
inline constexpr D2D1_COLOR_F Flyout = Rgb(0x2C2C2C);  // popup menus are opaque windows
inline constexpr D2D1_COLOR_F FlyoutHot = Rgb(0x3A3A3A);
inline constexpr D2D1_COLOR_F FlyoutStroke = Rgb(0x454545);
inline constexpr D2D1_COLOR_F OpaqueBase = Rgb(0x202020);  // window background without Mica
}  // namespace Color

COLORREF ToColorRef(D2D1_COLOR_F c);  // opaque colours only

enum class Font : std::size_t { Body, Strong, Caption, Label, Mono, Icon, IconSmall, Title, Count };

// DirectWrite formats for one DPI. Sizes are in physical pixels (render targets run at 96 DPI).
class Fonts {
public:
    void Create(UINT dpi);
    [[nodiscard]] IDWriteTextFormat* operator[](Font f) const { return m_formats[static_cast<std::size_t>(f)].Get(); }
    [[nodiscard]] UINT dpi() const noexcept { return m_dpi; }
    [[nodiscard]] int S(int v) const noexcept { return MulDiv(v, static_cast<int>(m_dpi), 96); }
    [[nodiscard]] float F(float v) const noexcept { return v * static_cast<float>(m_dpi) / 96.0f; }
    [[nodiscard]] float Size(Font f) const { return m_sizes[static_cast<std::size_t>(f)]; }
    [[nodiscard]] const wchar_t* UiFamily() const noexcept { return m_uiFamily; }

private:
    UINT m_dpi = 96;
    const wchar_t* m_uiFamily = L"Segoe UI";
    std::array<ComPtr<IDWriteTextFormat>, static_cast<std::size_t>(Font::Count)> m_formats;
    std::array<float, static_cast<std::size_t>(Font::Count)> m_sizes{};
};

// Process-wide graphics objects (UI thread only).
class Graphics {
public:
    static Graphics& Get();
    [[nodiscard]] bool Ready() const noexcept { return m_d2d && m_dwrite; }
    [[nodiscard]] ID2D1Factory* D2D() const noexcept { return m_d2d.Get(); }
    [[nodiscard]] IDWriteFactory* DWrite() const noexcept { return m_dwrite.Get(); }
    ID2D1DCRenderTarget* Target();
    void ResetTarget() { m_target.Reset(); }
    bool HasFamily(const wchar_t* family) const;

    // Mica is decided per process: either every window gets it or none does.
    bool mica = false;
    [[nodiscard]] D2D1_COLOR_F Base() const noexcept { return mica ? Black(0.0f) : Color::OpaqueBase; }
    // The pixel value a child window must fill with to look like `layer` drawn over the base.
    [[nodiscard]] D2D1_COLOR_F Surface(D2D1_COLOR_F layer) const noexcept;
    // GDI colour that reproduces `layer` (GDI pixels have alpha 0: on Mica they add to the backdrop).
    [[nodiscard]] COLORREF GdiSurface(D2D1_COLOR_F layer) const noexcept;

    // Reusable 32-bit top-down DIB (grows only).
    HDC Buffer(int width, int height);

private:
    Graphics();
    ~Graphics();
    ComPtr<ID2D1Factory> m_d2d;
    ComPtr<IDWriteFactory> m_dwrite;
    ComPtr<ID2D1DCRenderTarget> m_target;
    MemoryDc m_memDc;  // destroyed after the DIB is deselected (see ~Graphics)
    Gdi<HBITMAP> m_dib;
    HGDIOBJ m_oldBitmap = nullptr;
    int m_bufferWidth = 0, m_bufferHeight = 0;
};

enum class Align { Left, Center, Right };

class Canvas {
public:
    Canvas(ID2D1RenderTarget& target, const Fonts& fonts);
    [[nodiscard]] const Fonts& fonts() const noexcept { return m_fonts; }
    [[nodiscard]] ID2D1RenderTarget& target() noexcept { return m_rt; }

    void Clear(D2D1_COLOR_F c);
    void Fill(const D2D1_RECT_F& r, D2D1_COLOR_F c, float radius = 0);
    void Stroke(const D2D1_RECT_F& r, D2D1_COLOR_F c, float radius = 0, float width = 1);
    void Line(D2D1_POINT_2F a, D2D1_POINT_2F b, D2D1_COLOR_F c, float width = 1);
    void Circle(D2D1_POINT_2F center, float radius, D2D1_COLOR_F c);
    // Single line, vertically centred, ellipsis when it does not fit.
    void Text(std::wstring_view text, const D2D1_RECT_F& r, Font font, D2D1_COLOR_F c, Align align = Align::Left);
    // Wrapped paragraph from the top of `r`; returns the height used.
    float Paragraph(std::wstring_view text, const D2D1_RECT_F& r, Font font, D2D1_COLOR_F c);
    float Width(std::wstring_view text, Font font);
    float ParagraphHeight(std::wstring_view text, float width, Font font);
    void PushClip(const D2D1_RECT_F& r);
    void PopClip();

private:
    ID2D1SolidColorBrush* Brush(D2D1_COLOR_F c);
    ID2D1RenderTarget& m_rt;
    const Fonts& m_fonts;
    ComPtr<ID2D1SolidColorBrush> m_brush;
};

// Text width without a render target (layout and menus measure before painting).
float TextWidth(const Fonts& fonts, Font font, std::wstring_view text);
float TextHeight(const Fonts& fonts, Font font, std::wstring_view text, float width);  // wrapped

// Paints `rect` (client coordinates of `dc`) through the shared DIB.
// `gdi` runs after Direct2D on the same buffer, for areas that must match GDI-drawn child controls.
void Paint(HDC dc, const RECT& rect, const Fonts& fonts, const std::function<void(Canvas&)>& draw,
           const std::function<void(HDC)>& gdi = {});

constexpr D2D1_RECT_F ToF(const RECT& r) {
    return {static_cast<float>(r.left), static_cast<float>(r.top), static_cast<float>(r.right), static_cast<float>(r.bottom)};
}
constexpr D2D1_RECT_F Inset(const D2D1_RECT_F& r, float dx, float dy) { return {r.left + dx, r.top + dy, r.right - dx, r.bottom - dy}; }

// Applies dark title bar + Mica (or the opaque fallback) to a top-level window.
void ApplyBackdrop(HWND window);

}  // namespace DL::Ui
