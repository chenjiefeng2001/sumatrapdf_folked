/* Copyright 2024 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

#include "base/Base.h"
#include "base/Win.h"

#ifdef _MSC_VER
#include "base/Pixmap.h"
#include "GpuBackend.h"

GpuBackend* gGpuBackend = nullptr;

GpuBackend::GpuBackend() {
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
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
        0, 0,
        D2D1_RENDER_TARGET_USAGE_GDI_COMPATIBLE
    );

    HRESULT hr = factory->CreateDCRenderTarget(&props, &cachedRT);
    if (FAILED(hr) || !cachedRT) {
        cachedRT = nullptr;
        return nullptr;
    }

    // Bind the DC render target to the entire HDC.
    RECT rc = ClientRECT(WindowFromDC(hdc));
    hr = cachedRT->BindDC(hdc, &rc);
    if (FAILED(hr)) {
        cachedRT->Release();
        cachedRT = nullptr;
        return nullptr;
    }

    cachedHDC = hdc;
    return cachedRT;
}

ID2D1Bitmap* GpuBackend::CreateBitmapFromPixmap(const Pixmap* pixmap) {
    if (!factory || !pixmap || !pixmap->hbmp) {
        return nullptr;
    }

    // We need pixel data to create the D2D bitmap. The Pixmap stores its
    // pixel data in a DIB section accessible via pixmap->data. Read the
    // pixels directly from there rather than round-tripping through GDI.
    if (!pixmap->data || pixmap->width <= 0 || pixmap->height <= 0) {
        return nullptr;
    }

    // The DIB is bottom-up BGR(A)8; D2D expects top-down BGRA with
    // premultiplied alpha or no alpha. The render cache renders with
    // opaque backgrounds, so we use D2D1_ALPHA_MODE_IGNORE and copy
    // pixels as-is (BGR -> D2D's B8G8R8A8 is the same byte order).
    int w = pixmap->width;
    int h = pixmap->height;
    int stride = pixmap->stride;
    if (stride <= 0) {
        stride = w * 4; // fallback for BGRA8
    }

    // Create a temporary D2D1 bitmap from system memory.
    D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE)
    );

    // We need a temporary render target to create the bitmap. Use the
    // screen DC as the backing so we don't allocate an offscreen RT.
    HDC screenDC = GetDC(nullptr);
    ID2D1DCRenderTarget* rt = GetRenderTarget(screenDC);
    if (!rt) {
        ReleaseDC(nullptr, screenDC);
        return nullptr;
    }

    ID2D1Bitmap* bitmap = nullptr;
    D2D1_SIZE_U size = D2D1::SizeU((UINT32)w, (UINT32)h);
    // Pixmap data is bottom-up: the first pixel row in memory is the
    // bottommost row on screen. D2D expects top-down. We compensate by
    // passing a negative stride or flipping the Y. The simpler approach:
    // copy the pixels row-by-row in reverse to a temp buffer. For large
    // tiles (up to screen size ~1920x1080) this is fast enough.
    // We also need to handle the BGR vs BGRA stride: if n is 3 (BGR8),
    // pad each row to 4 bytes.
    int pitch = (w * 4 + 3) & ~3; // 4-byte aligned BGRA stride
    u8* flipped = AllocArray<u8>(h * pitch);
    if (!flipped) {
        ReleaseDC(nullptr, screenDC);
        return nullptr;
    }

    int srcBpp = pixmap->format == PixmapFormat::BGR8 ? 3 : 4;
    int srcStride = pixmap->stride > 0 ? pixmap->stride : w * srcBpp;

    for (int y = 0; y < h; y++) {
        const u8* srcRow = pixmap->data + (size_t)(h - 1 - y) * (size_t)srcStride;
        u8* dstRow = flipped + (size_t)y * (size_t)pitch;
        if (srcBpp == 4) {
            memcpy(dstRow, srcRow, (size_t)w * 4);
        } else {
            // BGR8 -> BGRA8: copy 3 bytes, set alpha=255
            for (int x = 0; x < w; x++) {
                dstRow[x * 4 + 0] = srcRow[x * 3 + 0];
                dstRow[x * 4 + 1] = srcRow[x * 3 + 1];
                dstRow[x * 4 + 2] = srcRow[x * 3 + 2];
                dstRow[x * 4 + 3] = 0xFF;
            }
        }
    }

    HRESULT hr = rt->CreateBitmap(size, flipped, pitch, props, &bitmap);
    free(flipped);

    // We must not hold the screen DC's render target for the rest of the
    // paint cycle since we'll switch to the actual window HDC later.
    // Release it now — it will be recreated on demand.
    if (cachedRT && cachedHDC == screenDC) {
        // Detach by releasing; GetRenderTarget will recreate next time.
        cachedRT->Release();
        cachedRT = nullptr;
        cachedHDC = nullptr;
    }
    ReleaseDC(nullptr, screenDC);

    if (FAILED(hr) || !bitmap) {
        return nullptr;
    }
    return bitmap;
}

#endif // _MSC_VER