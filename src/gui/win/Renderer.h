/* Copyright 2024 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

// Abstract rendering backend interface. D2DRenderer and GDIRenderer implement
// this so controls can draw with a single API regardless of the backend.
// Created and selected once during app init; fallback to GDI when D2D is
// unavailable.

// This header has no include guard — the project relies on controlled include
// order (base/Base.h first pulls in the common prerequisites).

struct GdiplusFont;
struct ID2D1SolidColorBrush;

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
    RgbaColor(COLORREF cr, uint8_t alpha = 255) : r(GetRValue(cr)), g(GetGValue(cr)), b(GetBValue(cr)), a(alpha) {}
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
    // Rounded rect (radius in pixels; <= 0 degrades to a plain rect). Needed by
    // tabs / notifications / overlay scrollbar to keep their rounded look when
    // painting through the unified backend.
    virtual void FillRoundRect(RECT rc, RgbaColor color, float radius) = 0;
    virtual void DrawRoundRect(RECT rc, RgbaColor color, float radius, float strokeWidth = 1.0f) = 0;
    // Filled triangle (scrollbar arrows, small chevrons).
    virtual void FillTriangle(int x1, int y1, int x2, int y2, int x3, int y3, RgbaColor color) = 0;

    // Text
    virtual void DrawText(Str text, RECT rc, RgbaColor color, HFONT font, UINT format = 0) = 0;
    virtual void DrawTextW(WStr text, RECT rc, RgbaColor color, HFONT font, UINT format = 0) = 0;

    // Bitmap
    virtual void DrawBitmap(HBITMAP hbmp, RECT dst, RECT src) = 0;

    // SVG icon rendering (via GDI+ or D2D SVG).
    // Default implementation is a no-op: SVG rasterization is an engine
    // capability (mupdf's fz_new_image_from_svg, see Toolbar), not something a
    // drawing backend can do standalone. Callers needing icons rasterize via
    // the engine pipeline and hand the resulting HBITMAP to DrawBitmap().
    virtual void DrawSvgIcon(const char* svgData, int svgLen, RECT rc, RgbaColor color);

    // Clip
    virtual void PushClip(RECT rc) = 0;
    virtual void PopClip() = 0;

    // Antialiasing hint
    virtual void SetTextAntialias(bool enable) = 0;
};

// Global renderer singleton. Set once at startup.
extern Renderer* gRenderer;
extern RendererBackendKind gRendererKind;

// ── Concrete backends ───────────────────────────────────────────────

// GDI backend: paints directly onto the HDC from BeginPaint. Always
// constructible; the fallback for every machine.
struct GDIRenderer : Renderer {
    HDC hdc = nullptr;
    HWND hwnd = nullptr;
    int savedDc = 0; // SaveDC() id of the active PushClip, 0 when none

    RendererBackendKind GetKind() override;
    bool BeginPaint(HWND hwnd, PAINTSTRUCT* ps) override;
    void EndPaint() override;
    HDC GetHDC() override;
    void FillRect(RECT rc, RgbaColor color) override;
    void FillRectF(const RectF& rc, RgbaColor color) override;
    void DrawRect(RECT rc, RgbaColor color, float strokeWidth = 1.0f) override;
    void DrawLine(int x1, int y1, int x2, int y2, RgbaColor color, float strokeWidth = 1.0f) override;
    void FillRoundRect(RECT rc, RgbaColor color, float radius) override;
    void DrawRoundRect(RECT rc, RgbaColor color, float radius, float strokeWidth = 1.0f) override;
    void FillTriangle(int x1, int y1, int x2, int y2, int x3, int y3, RgbaColor color) override;

    void DrawText(Str text, RECT rc, RgbaColor color, HFONT font, UINT format = 0) override;
    void DrawTextW(WStr text, RECT rc, RgbaColor color, HFONT font, UINT format = 0) override;
    void DrawBitmap(HBITMAP hbmp, RECT dst, RECT src) override;
    void PushClip(RECT rc) override;
    void PopClip() override;
    void SetTextAntialias(bool enable) override;
};

// Direct2D backend: paints through an ID2D1DCRenderTarget bound to the HDC.
// Text goes through DirectWrite (ClearType sub-pixel). Runtime-only: D2D is
// loaded dynamically, so on machines without d2d1.dll the constructor does not
// create a factory and CreateRenderer() falls back to GDIRenderer.
struct D2DRenderer : Renderer {
    struct ID2D1Factory* factory = nullptr;
    struct ID2D1DCRenderTarget* rt = nullptr;
    HDC hdc = nullptr;
    HWND hwnd = nullptr;
    // Single reusable fill/stroke brush: CreateSolidColorBrush once, then
    // SetColor per primitive (avoids per-draw COM allocation churn).
    struct ID2D1SolidColorBrush* cachedBrush = nullptr;

    D2DRenderer();
    ~D2DRenderer() override;

    // Create the D2D factory (dynamic d2d1.dll load). Returns false when D2D
    // is unavailable; the instance is then unusable and must be discarded.
    bool Init();

    RendererBackendKind GetKind() override;
    bool BeginPaint(HWND hwnd, PAINTSTRUCT* ps) override;
    void EndPaint() override;
    HDC GetHDC() override;
    void FillRect(RECT rc, RgbaColor color) override;
    void FillRectF(const RectF& rc, RgbaColor color) override;
    void DrawRect(RECT rc, RgbaColor color, float strokeWidth = 1.0f) override;
    void FillRoundRect(RECT rc, RgbaColor color, float radius) override;
    void DrawRoundRect(RECT rc, RgbaColor color, float radius, float strokeWidth = 1.0f) override;
    void FillTriangle(int x1, int y1, int x2, int y2, int x3, int y3, RgbaColor color) override;

    void DrawLine(int x1, int y1, int x2, int y2, RgbaColor color, float strokeWidth = 1.0f) override;
    void DrawText(Str text, RECT rc, RgbaColor color, HFONT font, UINT format = 0) override;
    void DrawTextW(WStr text, RECT rc, RgbaColor color, HFONT font, UINT format = 0) override;
    void DrawBitmap(HBITMAP hbmp, RECT dst, RECT src) override;
    void PushClip(RECT rc) override;
    void PopClip() override;
    void SetTextAntialias(bool enable) override;

  private:
    ID2D1SolidColorBrush* CreateBrush(RgbaColor color);
};

// Whether Direct2D can be used at all on this machine (d2d1.dll present and
// D2D1CreateFactory exported).
bool IsD2D1Available();

// Create the backend appropriate for this machine: D2D when available,
// otherwise GDI. Sets the gRenderer / gRendererKind globals and returns the
// new instance (caller owns it; the app creates it once at startup).
Renderer* CreateRenderer();
