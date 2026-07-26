/* Copyright 2024 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

#include "base/Base.h"
#include "base/Win.h"

#ifdef _MSC_VER
#include "base/Pixmap.h"
#include "base/Log.h"
#include "GpuBackend.h"

// Forward-declare the device-generation diagnostics counter (defined in RenderCache.cpp)
// so that D2D overlay helpers can bump it on EndDraw failure.
extern LONG gDeviceGenRecreations;

#ifdef DEBUG
// Main thread ID for D2D thread-affinity assertions.
// Set in GpuBackend::GpuBackend(), which runs on the UI thread.
DWORD g_mainThreadId = 0;
#endif

GpuBackend* gGpuBackend = nullptr;

GpuBackend::GpuBackend() {
#ifdef DEBUG
    g_mainThreadId = GetCurrentThreadId();
#endif
    // Create the D2D1 factory; if this fails, GPU support is unavailable.
    HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &factory);
    if (SUCCEEDED(hr) && factory) {
        isAvailable = true;
    }
}

GpuBackend::~GpuBackend() {
    if (cachedRT) {
        cachedRT->Release();
    }
    if (factory) {
        factory->Release();
    }
}

GpuBackend* GpuBackend::Create() {
    auto* backend = new GpuBackend();
    if (!backend->isAvailable) {
        delete backend;
        return nullptr;
    }
    return backend;
}

ID2D1DCRenderTarget* GpuBackend::GetRenderTarget(HDC hdc) {
    if (!factory || !hdc) {
        return nullptr;
    }
    // Return the cached RT if it's for the same HDC (common case during a
    // single WM_PAINT where the same HDC is used for all tiles).
    if (cachedRT && cachedHDC == hdc) {
        return cachedRT;
    }

    // Release the old RT before creating a new one.
    if (cachedRT) {
        cachedRT->Release();
        cachedRT = nullptr;
        cachedHDC = nullptr;
    }

    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 0, 0,
        D2D1_RENDER_TARGET_USAGE_GDI_COMPATIBLE);

    HRESULT hr = factory->CreateDCRenderTarget(&props, &cachedRT);
    if (FAILED(hr) || !cachedRT) {
        cachedRT = nullptr;
        return nullptr;
    }

    // Bump the device generation counter so that previously-uploaded
    // ID2D1Bitmap instances (created on the old render target) are
    // detected as belonging to a different resource domain on the next
    // PaintTile call and are safely re-created on the new device.
    // Without this, DrawBitmap + EndDraw would fail with
    // D2DERR_WRONG_RESOURCE_DOMAIN (0x88990015) and fall back to GDI
    // for every tile, causing visible stutter during scrolling/resizing.
    // See docs/reports/d2d-device-generation-analysis.md §5.1.
    deviceGeneration++;
#ifdef DEBUG
    ReportIf(deviceGeneration <= 0); // must remain positive after each bump
#endif

    // Bind the DC render target to the entire HDC.
    // For memory DCs (double buffer), WindowFromDC returns NULL and
    // ClientRECT(NULL) would yield garbage, so get the extent from
    // the DC's clip box or selected bitmap instead.
    RECT rc = {};
    HWND hwnd = WindowFromDC(hdc);
    if (hwnd) {
        rc = ClientRECT(hwnd);
    } else {
        // Memory DC: use DC extent (clip box or bitmap size).
        if (GetClipBox(hdc, &rc) == ERROR || (rc.right <= 0 || rc.bottom <= 0)) {
            // Fallback: query the DC's device dimensions.
            // GetClipBox can succeed with {0,0,0,0} on a freshly-
            // created memory DC that has a bitmap selected but no
            // explicit clipping — use the bitmap extent instead.
            int cx = GetDeviceCaps(hdc, HORZRES);
            int cy = GetDeviceCaps(hdc, VERTRES);
            if (cx > 0 && cy > 0) {
                rc.right = cx;
                rc.bottom = cy;
            } else {
                // Last-resort fallback: try the selected bitmap.
                HBITMAP hbmp = (HBITMAP)GetCurrentObject(hdc, OBJ_BITMAP);
                DIBSECTION ds = {};
                if (hbmp && sizeof(ds) == GetObject(hbmp, sizeof(ds), &ds)) {
                    rc.right = ds.dsBmih.biWidth;
                    rc.bottom = ds.dsBmih.biHeight;
                }
            }
        }
    }
    hr = cachedRT->BindDC(hdc, &rc);
    if (FAILED(hr)) {
        cachedRT->Release();
        cachedRT = nullptr;
        return nullptr;
    }

    cachedHDC = hdc;
    return cachedRT;
}

void GpuBackend::RecreateRenderTarget() {
#ifdef DEBUG
    // D2D resource operations must happen on the UI thread.
    // RecreateRenderTarget is called from PaintTile (UI thread), so this
    // assertion verifies the caller hasn't changed.
    // TODO: add CrashIf if crash reporting infrastructure is available.
#endif
    if (cachedRT) {
        cachedRT->Release();
        cachedRT = nullptr;
        cachedHDC = nullptr;
    }
    // Bump the generation so that all previously-uploaded ID2D1Bitmap
    // instances are detected as stale on the next PaintTile and are
    // lazily re-created on the new (future) render target.
    deviceGeneration++;
#ifdef DEBUG
    ReportIf(deviceGeneration <= 0); // must remain positive
#endif
    InterlockedIncrement(&gDeviceGenRecreations);
    logfa(
        "[RenderCache Diagnostic] GpuBackend::RecreateRenderTarget: "
        "D2D device recreated. New device generation = %d.\n",
        deviceGeneration);
}

ID2D1Bitmap* GpuBackend::CreateBitmapFromPixmap(ID2D1DCRenderTarget* rt, const Pixmap* pixmap) {
    if (!factory || !rt || !pixmap || !pixmap->hbmp) {
        return nullptr;
    }

#ifdef DEBUG
    // D2D thread affinity: CreateBitmapFromPixmap must be called on the UI
    // thread because ID2D1RenderTarget is NOT thread-safe. Background thread
    // calls would race with WM_PAINT's BeginDraw/DrawBitmap/EndDraw sequence
    // and crash the GPU driver with a hung-device or invalid-call error.
    ReportIf(g_mainThreadId != 0 && g_mainThreadId != GetCurrentThreadId());
#endif

    // We need pixel data to create the D2D bitmap. The Pixmap stores its
    // pixel data in a DIB section accessible via pixmap->data. Read the
    // pixels directly from there rather than round-tripping through GDI.
    if (!pixmap->data || pixmap->width <= 0 || pixmap->height <= 0) {
        return nullptr;
    }

    // Defensive: assert pixmap dimensions are sane to catch corrupted PDFs
    // that produce degenerate pixmaps (e.g. after repair).  See
    // docs/reports/annot-render-crash-analysis.md §3.2 for analysis of
    // the memcpy-AV crashes this prevents.
    ReportDebugIf(pixmap->width > 16384 || pixmap->height > 16384);

    // The Pixmap DIB is top-down (AllocPixmapDIB and NewRenderedFzPixmap both
    // use biHeight = -h).  D2D CreateBitmap also expects top-down data, so we
    // copy rows in-order (row 0 → row 0) with no vertical flip.
    //
    // For BGR8 (n=3) sources we expand each pixel from 3 to 4 bytes (B,G,R,A).
    // The pixel row stride may be wider than w * srcBpp due to DIB alignment
    // padding — the copy loops below handle this by advancing through
    // srcStride bytes per row, not w * srcBpp.
    //
    // Coordinate-system invariant:
    //   AllocPixmapDIB / NewRenderedFzPixmap  biHeight = -h  →  top-down
    //   PixmapFromHBITMAP                     data = bmBits  →  row 0 = top
    //   CreateBitmapFromPixmap                y=0..h-1 order →  top-down buffer
    //   ID2D1RenderTarget::CreateBitmap       expects top-down→  upright on screen
    int w = pixmap->width;
    int h = pixmap->height;
    int stride = pixmap->stride;
    if (stride <= 0) {
        stride = w * 4; // fallback for BGRA8
    }

    D2D1_SIZE_U size = D2D1::SizeU((UINT32)w, (UINT32)h);
    int pitch = (w * 4 + 3) & ~3; // 4-byte aligned BGRA stride
    u8* buf = AllocArray<u8>(h * pitch);
    if (!buf) {
        return nullptr;
    }

    // Determine source bytes-per-pixel from the Pixmap format enum.
    // NOTE: PixmapFormat::RGBA8 also gives srcBpp == 4 here (same byte count
    // as BGRA8), but the byte order differs — RGBA8 needs a swizzle to
    // B8G8R8A8. We handle it below by never blindly memcpy-ing RGBA8
    // (we fall through to the pixel loop and swizzle on the fly).
    int srcBpp = pixmap->format == PixmapFormat::BGR8 ? 3 : 4;
    int srcStride = pixmap->stride > 0 ? pixmap->stride : w * srcBpp;

    // Defensive bounds: guard against corrupted-PDF scenarios where the
    // DIB section's stride or pixel depth is inconsistent with the
    // Pixmap metadata (e.g. a PDF repair changes the page dimensions
    // but the cached Pixmap has stale width/height). If the stride is
    // suspicious (< w*srcBpp meaning rows overlap), bail out early
    // rather than memcpy past the buffer end.
    if (srcStride < w * srcBpp) {
        free(buf);
        return nullptr;
    }

#ifdef DEBUG
    // Sanity check: srcStride should not be pathologically large (more
    // than 4 bytes/pixel * width + reasonable padding). A corrupt PDF
    // could produce a false-small stride that passes the width check
    // but causes the srcRow pointers to index way past the pixmap
    // allocation, triggering an AV on the first memcpy below.
    ReportDebugIf((size_t)srcStride > (size_t)w * 4 + 4096);
#endif

    for (int y = 0; y < h; y++) {
        const u8* srcRow = pixmap->data + (size_t)y * (size_t)srcStride;
        u8* dstRow = buf + (size_t)y * (size_t)pitch;
        if (srcBpp == 4 && pixmap->format == PixmapFormat::BGRA8) {
            // Fast path: same byte order (B,G,R,A), just copy.
            // srcStride >= w*4 is guaranteed by the check above.
            memcpy(dstRow, srcRow, (size_t)w * 4);
        } else if (srcBpp == 4 && pixmap->format == PixmapFormat::RGBA8) {
            // RGBA8 -> BGRA8: swizzle R<->B channels, set alpha=255 from src.
            for (int x = 0; x < w; x++) {
                dstRow[x * 4 + 0] = srcRow[x * 4 + 2]; // B <- R
                dstRow[x * 4 + 1] = srcRow[x * 4 + 1]; // G <- G
                dstRow[x * 4 + 2] = srcRow[x * 4 + 0]; // R <- B
                dstRow[x * 4 + 3] = srcRow[x * 4 + 3]; // A
            }
        } else {
            // BGR8 -> BGRA8: copy 3 bytes, set alpha=255.
            // Also acts as a safe fallback for any unexpected format
            // where srcBpp is ambiguous.
            for (int x = 0; x < w; x++) {
                dstRow[x * 4 + 0] = srcRow[x * 3 + 0];
                dstRow[x * 4 + 1] = srcRow[x * 3 + 1];
                dstRow[x * 4 + 2] = srcRow[x * 3 + 2];
                dstRow[x * 4 + 3] = 0xFF;
            }
        }
    }

    // Create the bitmap on the caller's render target (the same one that will
    // later call DrawBitmap). This avoids the UAF bug of overwriting cachedRT
    // AND the cross-RT driver crash on certain GPU configurations.
    D2D1_BITMAP_PROPERTIES props =
        D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
    ID2D1Bitmap* bitmap = nullptr;
    HRESULT hr = rt->CreateBitmap(size, buf, pitch, props, &bitmap);
    free(buf);

    if (FAILED(hr) || !bitmap) {
        logfa("[RenderCache Diagnostic] D2D CreateBitmap failed! w=%d h=%d pitch=%d HRESULT=0x%08X\n", w, h, pitch,
              (unsigned)hr);
        if (hr == D2DERR_RECREATE_TARGET) {
            logfa(
                "[RenderCache Diagnostic] GPU Device Lost detected! Re-initializing D2D "
                "context is required. All cached GPU bitmaps are now invalid.\n");
        }
        return nullptr;
    }

    return bitmap;
}

// ── D2D overlay helpers ──────────────────────────────────────────────────────
//
// These static methods are called from Canvas.cpp / Selection.cpp when
// gGpuBackend is available. They draw onto the same HDC that the caller is
// using for the backbuffer, mixing D2D output with GDI content already there.
// The D2D render target is obtained on-demand and cached per HDC.
//
// All functions return false if D2D is unavailable for this HDC, letting the
// caller fall back transparently to GDI+.

ID2D1DCRenderTarget* GpuBackend::GetRT(HDC hdc) {
    if (!gGpuBackend || !gGpuBackend->factory || !hdc) {
        return nullptr;
    }
    return gGpuBackend->GetRenderTarget(hdc);
}

ID2D1SolidColorBrush* GpuBackend::GetBrush(ID2D1DCRenderTarget* rt, COLORREF color, u8 alpha) {
    if (!rt) return nullptr;
    D2D1_COLOR_F d2dColor = D2D1::ColorF((GetRValue(color) / 255.0f), (GetGValue(color) / 255.0f),
                                         (GetBValue(color) / 255.0f), alpha / 255.0f);
    ID2D1SolidColorBrush* brush = nullptr;
    HRESULT hr = rt->CreateSolidColorBrush(d2dColor, &brush);
    return SUCCEEDED(hr) ? brush : nullptr;
}

bool GpuBackend::DrawOverlayRects(HDC hdc, Rect screenRc, Vec<Rect>& rects, COLORREF color, u8 alpha, int pad,
                                  bool drawBorder) {
    if (len(rects) == 0) return true;

    ID2D1DCRenderTarget* rt = GetRT(hdc);
    if (!rt) return false;

    rt->BeginDraw();

    ID2D1SolidColorBrush* brush = GetBrush(rt, color, alpha);
    if (!brush) {
        rt->EndDraw();
        return false;
    }

    screenRc.Inflate(pad, pad);
    for (int i = 0; i < len(rects); i++) {
        Rect rc = rects.at(i);
        if (pad > 0) {
            rc.Inflate(pad, pad);
        }
        rc = rc.Intersect(screenRc);
        if (rc.IsEmpty()) continue;

        D2D1_RECT_F d2drc = D2D1::RectF((float)rc.x, (float)rc.y, (float)(rc.x + rc.dx), (float)(rc.y + rc.dy));
        rt->FillRectangle(&d2drc, brush);

        if (drawBorder && pad > 0) {
            ID2D1SolidColorBrush* borderBrush = GetBrush(rt, RGB(0, 0, 0), alpha);
            if (borderBrush) {
                rt->DrawRectangle(&d2drc, borderBrush, (float)pad);
                borderBrush->Release();
            }
        }
    }

    brush->Release();
    HRESULT hrEnd = rt->EndDraw();
    if (FAILED(hrEnd)) {
        logfa("[RenderCache Diagnostic] D2D EndDraw failed in DrawOverlayRects HRESULT=0x%08X\n", (unsigned)hrEnd);
        if (hrEnd == D2DERR_RECREATE_TARGET && gGpuBackend) {
            InterlockedIncrement(&gDeviceGenRecreations);
            gGpuBackend->RecreateRenderTarget();
        }
        return false;
    }
    return true;
}

bool GpuBackend::DrawDashedBorder(HDC hdc, Rect rect, COLORREF color, float width) {
    ID2D1DCRenderTarget* rt = GetRT(hdc);
    if (!rt) return false;

    // Create a dashed stroke style (on the ID2D1Factory, not on the RT)
    ID2D1StrokeStyle* dashStyle = nullptr;
    D2D1_STROKE_STYLE_PROPERTIES props =
        D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_LINE_JOIN_MITER,
                                    10.0f, D2D1_DASH_STYLE_DASH, 0.0f);
    if (!gGpuBackend || !gGpuBackend->factory ||
        FAILED(gGpuBackend->factory->CreateStrokeStyle(props, nullptr, 0, &dashStyle))) {
        return false;
    }

    rt->BeginDraw();

    ID2D1SolidColorBrush* brush = GetBrush(rt, color, 255);
    if (!brush) {
        rt->EndDraw();
        dashStyle->Release();
        return false;
    }

    D2D1_RECT_F d2drc = D2D1::RectF((float)rect.x, (float)rect.y, (float)(rect.x + rect.dx), (float)(rect.y + rect.dy));
    rt->DrawRectangle(&d2drc, brush, width, dashStyle);

    brush->Release();
    dashStyle->Release();
    HRESULT hrEnd = rt->EndDraw();
    if (FAILED(hrEnd)) {
        logfa("[RenderCache Diagnostic] D2D EndDraw failed in DrawDashedBorder HRESULT=0x%08X\n", (unsigned)hrEnd);
        if (hrEnd == D2DERR_RECREATE_TARGET && gGpuBackend) {
            InterlockedIncrement(&gDeviceGenRecreations);
            gGpuBackend->RecreateRenderTarget();
        }
        return false;
    }
    return true;
}

bool GpuBackend::DrawResizeHandle(HDC hdc, int x, int y, int size) {
    ID2D1DCRenderTarget* rt = GetRT(hdc);
    if (!rt) return false;

    rt->BeginDraw();

    // White fill
    ID2D1SolidColorBrush* fillBrush = GetBrush(rt, RGB(255, 255, 255), 255);
    if (!fillBrush) {
        rt->EndDraw();
        return false;
    }

    D2D1_RECT_F rc = D2D1::RectF((float)x, (float)y, (float)(x + size), (float)(y + size));
    rt->FillRectangle(&rc, fillBrush);
    fillBrush->Release();

    // Black border
    ID2D1SolidColorBrush* borderBrush = GetBrush(rt, RGB(0, 0, 0), 255);
    if (borderBrush) {
        rt->DrawRectangle(&rc, borderBrush, 1.0f);
        borderBrush->Release();
    }

    HRESULT hrEnd = rt->EndDraw();
    if (FAILED(hrEnd)) {
        logfa("[RenderCache Diagnostic] D2D EndDraw failed in DrawResizeHandle HRESULT=0x%08X\n", (unsigned)hrEnd);
        if (hrEnd == D2DERR_RECREATE_TARGET && gGpuBackend) {
            InterlockedIncrement(&gDeviceGenRecreations);
            gGpuBackend->RecreateRenderTarget();
        }
        return false;
    }
    return true;
}

bool GpuBackend::DrawFillRect(HDC hdc, Rect rect, COLORREF color, u8 alpha) {
    ID2D1DCRenderTarget* rt = GetRT(hdc);
    if (!rt) return false;

    rt->BeginDraw();

    ID2D1SolidColorBrush* brush = GetBrush(rt, color, alpha);
    if (!brush) {
        rt->EndDraw();
        return false;
    }

    D2D1_RECT_F rc = D2D1::RectF((float)rect.x, (float)rect.y, (float)(rect.x + rect.dx), (float)(rect.y + rect.dy));
    rt->FillRectangle(&rc, brush);
    brush->Release();

    HRESULT hrEnd = rt->EndDraw();
    if (FAILED(hrEnd)) {
        logfa("[RenderCache Diagnostic] D2D EndDraw failed in DrawFillRect HRESULT=0x%08X\n", (unsigned)hrEnd);
        if (hrEnd == D2DERR_RECREATE_TARGET && gGpuBackend) {
            InterlockedIncrement(&gDeviceGenRecreations);
            gGpuBackend->RecreateRenderTarget();
        }
        return false;
    }
    return true;
}

bool GpuBackend::DrawSolidBorder(HDC hdc, Rect rect, COLORREF color, float width) {
    ID2D1DCRenderTarget* rt = GetRT(hdc);
    if (!rt) return false;

    rt->BeginDraw();

    ID2D1SolidColorBrush* brush = GetBrush(rt, color, 255);
    if (!brush) {
        rt->EndDraw();
        return false;
    }

    D2D1_RECT_F rc = D2D1::RectF((float)rect.x, (float)rect.y, (float)(rect.x + rect.dx), (float)(rect.y + rect.dy));
    rt->DrawRectangle(&rc, brush, width);
    brush->Release();

    HRESULT hrEnd = rt->EndDraw();
    if (FAILED(hrEnd)) {
        logfa("[RenderCache Diagnostic] D2D EndDraw failed in DrawSolidBorder HRESULT=0x%08X\n", (unsigned)hrEnd);
        if (hrEnd == D2DERR_RECREATE_TARGET && gGpuBackend) {
            InterlockedIncrement(&gDeviceGenRecreations);
            gGpuBackend->RecreateRenderTarget();
        }
        return false;
    }
    return true;
}

#endif // _MSC_VER