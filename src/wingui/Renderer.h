/* Copyright 2024 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

// Abstract rendering backend interface. D2DRenderer and GDIRenderer implement
// this so controls can draw with a single API regardless of the backend.
// Created and selected once during app init; fallback to GDI when D2D is
// unavailable.

// This header has no include guard — the project relies on controlled include
// order (base/Base.h first pulls in the common prerequisites).

struct GdiplusFont;

enum class RendererBackendKind {
    GDI,
    D2D,
};

// Uniform color type for both backends
struct RgbaColor {
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
    uint8_t a = 255;

    RgbaColor() = default;
    RgbaColor(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) : r(r), g(g), b(b), a(a) {}
    // implicit conversion from COLORREF (BGR)
    RgbaColor(COLORREF cr, uint8_t alpha = 255)
        : r(GetRValue(cr)), g(GetGValue(cr)), b(GetBValue(cr)), a(alpha) {}
};

struct RectF;

// Minimal drawing operations needed by our custom controls. A control that
// needs more (e.g. SVG icon rendering, gradient fills) can dynamic_cast to
// the concrete backend.
struct Renderer {
    virtual ~Renderer() = default;

    virtual RendererBackendKind GetKind() = 0;

    // Lifecycle
    virtual bool BeginPaint(HWND hwnd, PAINTSTRUCT* ps) = 0;
    virtual void EndPaint() = 0;

    // Returns an HDC usable for GDI interop (e.g. ExtTextOut fallback).
    // For GDIRenderer this is the same HDC from BeginPaint; for D2DRenderer
    // it obtains the HDC from the D2D DC render target.
    virtual HDC GetHDC() = 0;

    // 2D Primitives
    virtual void FillRect(RECT rc, RgbaColor color) = 0;
    virtual void FillRectF(const RectF& rc, RgbaColor color) = 0;
    virtual void DrawRect(RECT rc, RgbaColor color, float strokeWidth = 1.0f) = 0;
    virtual void DrawLine(int x1, int y1, int x2, int y2, RgbaColor color, float strokeWidth = 1.0f) = 0;

    // Text
    virtual void DrawText(Str text, RECT rc, RgbaColor color, HFONT font, UINT format = 0) = 0;
    virtual void DrawTextW(WStr text, RECT rc, RgbaColor color, HFONT font, UINT format = 0) = 0;

    // Bitmap
    virtual void DrawBitmap(HBITMAP hbmp, RECT dst, RECT src) = 0;

    // SVG icon rendering (via GDI+ or D2D SVG)
    virtual void DrawSvgIcon(const char* svgData, int svgLen, RECT rc, RgbaColor color) = 0;

    // Clip
    virtual void PushClip(RECT rc) = 0;
    virtual void PopClip() = 0;

    // Antialiasing hint
    virtual void SetTextAntialias(bool enable) = 0;
};

// Global renderer singleton. Set once at startup.
extern Renderer* gRenderer;
extern RendererBackendKind gRendererKind;

