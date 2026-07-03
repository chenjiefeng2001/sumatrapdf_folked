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

    // Create (or reuse a cached) ID2D1Bitmap from a Pixmap's HBITMAP by
    // copying pixels into a D2D bitmap. The caller must Release() the
    // returned bitmap when done. Returns nullptr on failure.
    ID2D1Bitmap* CreateBitmapFromPixmap(const Pixmap* pixmap);

    // Get a per-HDC D2D render target. The render target wraps an HDC so
    // D2D and GDI can interop on the same device context. Returns nullptr
    // if creation fails. Released automatically on destruction.
    ID2D1DCRenderTarget* GetRenderTarget(HDC hdc);

  private:
    ID2D1Factory* factory = nullptr;
    // cache the last render target (one per thread / HDC is wasteful but
    // acceptable since rendering is serialized in the UI thread).
    ID2D1DCRenderTarget* cachedRT = nullptr;
    HDC cachedHDC = nullptr;
};

// Global GPU backend singleton. Created on first canvas paint if GPU is
// available. Access through gGpuBackend (may be null).
extern GpuBackend* gGpuBackend;

#endif // _MSC_VER