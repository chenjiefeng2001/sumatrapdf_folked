/* Copyright 2024 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

// GPU-accelerated rendering backend using Direct2D. Provides texture upload
// and compositing for the render cache, replacing GDI StretchBlt/BitBlt with
// Direct2D DrawBitmap when a GPU is available.
//
// This is an optional, best-effort optimization: if GPU init fails the
// existing GDI path is used as fallback.

// Guard: this header only compiles with MSVC (D2D1 headers).
// Mingw cross-compiles skip GPU support.
#ifdef _MSC_VER

#include <d2d1.h>
#include <d2d1helper.h>

#pragma comment(lib, "d2d1.lib")

struct Pixmap;

struct GpuBackend {
    // Create the singleton GPU backend. Returns nullptr if init fails (no
    // D2D1 factory, no suitable adapter, etc.). The caller should not retry
    // on failure — GPU is simply unavailable.
    static GpuBackend* Create();

    GpuBackend();
    ~GpuBackend();

    // Not copyable
    GpuBackend(const GpuBackend&) = delete;
    GpuBackend& operator=(const GpuBackend&) = delete;

    // Whether the backend was successfully initialized and is usable.
    bool isAvailable = false;

    // Monotonically-increasing counter bumped each time the D2D factory or
    // render-target state is re-created (at present, this never re-creates in
    // practice — the counter is a forward-looking guard).  Callers compare
    // this against Pixmap::d2dDeviceGeneration to detect stale device-domain
    // bitmaps that would trigger D2DERR_WRONG_RESOURCE_DOMAIN.
    int GetDeviceGeneration() const { return deviceGeneration; }

    // Create an ID2D1Bitmap on the given render target from a Pixmap's pixel
    // data by copying (and flipping) the pixels into a D2D system-memory bitmap.
    // The returned bitmap is owned by the caller (stored in Pixmap::d2dBitmap).
    // IMPORTANT: Pass the same RT that will later call DrawBitmap — never a
    // temporary RT, because creating the bitmap on one RT and drawing on
    // another can cause driver-level crashes on certain GPU configurations.
    // Returns nullptr on failure.
    ID2D1Bitmap* CreateBitmapFromPixmap(ID2D1DCRenderTarget* rt, const Pixmap* pixmap);

    // Get a per-HDC D2D render target. The render target wraps an HDC so
    // D2D and GDI can interop on the same device context. Returns nullptr
    // if creation fails. Released automatically on destruction.
    ID2D1DCRenderTarget* GetRenderTarget(HDC hdc);

    // Force-recreate the D2D render target and bump the device generation so
    // that all cached ID2D1Bitmap instances are lazily re-created on the next
    // PaintTile. Call on the UI thread after detecting D2DERR_RECREATE_TARGET
    // (GPU device lost / driver reset). See docs/reports/d2d-device-generation-analysis.md §5.2.
    void RecreateRenderTarget();

    // ── D2D overlay helpers (replaces GDI+ for annotation/selection rendering) ──
    //
    // All return true on success, false on any D2D failure (caller falls back to GDI).

    // Fill a set of rects with a semi-transparent color (selection, find-match, read-aloud).
    // hdc: the device context to draw on (backbuffer).
    // screenRc: clipping rect in screen coords.
    // rects: list of rectangles to fill (will be clipped to screenRc and inflated by pad).
    // color, alpha: fill color + opacity (alpha 0xFF = opaque).
    // pad: extra pixels to inflate each rect before drawing.
    // drawBorder: if true, draw a thin border around the combined shape.
    static bool DrawOverlayRects(HDC hdc, Rect screenRc, Vec<Rect>& rects, COLORREF color, u8 alpha, int pad,
                                 bool drawBorder);

    // Draw a dashed rectangle border (for annotation editing selection).
    // width: pen width in pixels. Dash pattern is 4-on 2-off.
    static bool DrawDashedBorder(HDC hdc, Rect rect, COLORREF color, float width);

    // Draw a solid-filled + black-bordered resize handle square.
    // (x, y) is the top-left corner of the handle; size is the edge length.
    static bool DrawResizeHandle(HDC hdc, int x, int y, int size);

    // Fill a single rectangle with a semi-transparent color.
    static bool DrawFillRect(HDC hdc, Rect rect, COLORREF color, u8 alpha);

    // Draw a solid stroked rectangle border.
    static bool DrawSolidBorder(HDC hdc, Rect rect, COLORREF color, float width);

  private:
    // Low-level helpers -----------------------------------------------------------
    static ID2D1DCRenderTarget* GetRT(HDC hdc);
    static ID2D1SolidColorBrush* GetBrush(ID2D1DCRenderTarget* rt, COLORREF color, u8 alpha);

    ID2D1Factory* factory = nullptr;
    // cache the last render target (one per thread / HDC is wasteful but
    // acceptable since rendering is serialized in the UI thread).
    ID2D1DCRenderTarget* cachedRT = nullptr;
    HDC cachedHDC = nullptr;

    // Generation counter for D2D device-affinity checks. Incremented when
    // the D2D factory is re-created (not yet wired; present for future use).
    int deviceGeneration = 1;
};

// Global GPU backend singleton. Created on first canvas paint if GPU is
// available. Access through gGpuBackend (may be null).
extern GpuBackend* gGpuBackend;

#endif // _MSC_VER