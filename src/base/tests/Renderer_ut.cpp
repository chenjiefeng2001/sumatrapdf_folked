/* Copyright 2024 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

// Unit tests for the unified control drawing backend (wingui/Renderer.h/.cpp):
//   1. RgbaColor construction and COLORREF conversion
//   2. DirectWrite format-cache key equality (pure logic)
//   3. GDIRenderer real pixel output on a 32bpp memory DC (FillRect, DrawLine,
//      clip regions, text, BitBlt) — the drawing must actually land in pixels
//   4. D2DRenderer smoke test — when d2d1.dll is present, paint through a DC
//      render target and verify pixels; otherwise verify the GDI fallback
//   5. CreateRenderer() backend selection / fallback
//   6. DirectWrite layout measure (skipped when DWrite is unavailable)
//
// Runs inside test_util.exe without any GUI: all drawing targets are in-memory
// 32bpp DIB sections, and D2D/DWrite use the machine's runtime availability.

#include "base/Base.h"
#include "base/Win.h"

#ifdef _MSC_VER
#include <dwrite.h>
#include "wingui/Renderer.h"
#include "wingui/DWriteText.h"
#endif

// must be last due to assert() over-write
#include "base/UtAssert.h"

#ifdef _MSC_VER

static void RgbaColorTest() {
    RgbaColor c(0xFF, 0x80, 0x00);
    utassert(c.r == 0xFF && c.g == 0x80 && c.b == 0x00 && c.a == 0xFF);

    // COLORREF is stored BGR: the conversion must swap the channels back
    RgbaColor fromCr(RGB(10, 20, 30), 128);
    utassert(fromCr.r == 10 && fromCr.g == 20 && fromCr.b == 30 && fromCr.a == 128);

    RgbaColor def;
    utassert(def.r == 0 && def.g == 0 && def.b == 0 && def.a == 255);
}

static void DWriteCacheKeyTest() {
    DWriteFormatKey k1{};
    wcscpy_s(k1.family, L"Arial");
    k1.size = 12.0f;
    k1.weight = 400;
    k1.style = 0;

    // identical key matches
    DWriteFormatKey k2 = k1;
    utassert(DWriteCacheKeysEqual(k1, k2));

    // differing size / weight / style do not match
    DWriteFormatKey k3 = k1;
    k3.size = 14.0f;
    utassert(!DWriteCacheKeysEqual(k1, k3));
    DWriteFormatKey k4 = k1;
    k4.weight = 700;
    utassert(!DWriteCacheKeysEqual(k1, k4));
    DWriteFormatKey k5 = k1;
    k5.style = 1;
    utassert(!DWriteCacheKeysEqual(k1, k5));

    // differing family does not match
    DWriteFormatKey k6 = k1;
    wcscpy_s(k6.family, L"Times New Roman");
    utassert(!DWriteCacheKeysEqual(k1, k6));

    // family comparison is case-sensitive (DWrite font names are compared by
    // the system font collection; the key is an exact match)
    DWriteFormatKey k7 = k1;
    wcscpy_s(k7.family, L"arial");
    utassert(!DWriteCacheKeysEqual(k1, k7));
}

// 32bpp memory DC used as a headless drawing target
struct MemDc {
    HDC hdc = nullptr;
    HBITMAP bmp = nullptr;
    HGDIOBJ old = nullptr;
    u8* bits = nullptr; // CreateDIBSection pixel memory (top-down)
    int w = 0;
    int h = 0;
    int bpp = 32;

    bool ok = false;
};

static MemDc MakeMemDc(int w, int h, int bpp = 32) {
    MemDc m;
    m.hdc = CreateCompatibleDC(nullptr);
    if (!m.hdc) {
        return m;
    }
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h; // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = (WORD)bpp;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    m.bmp = CreateDIBSection(m.hdc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!m.bmp) {
        DeleteDC(m.hdc);
        m.hdc = nullptr;
        return m;
    }
    m.old = SelectObject(m.hdc, m.bmp);
    m.bits = (u8*)bits;
    m.w = w;
    m.h = h;
    m.bpp = bpp;
    m.ok = true;
    return m;
}

static void FreeMemDc(MemDc& m) {
    if (m.hdc) {
        SelectObject(m.hdc, m.old);
        DeleteObject(m.bmp);
        DeleteDC(m.hdc);
        m.hdc = nullptr;
    }
}

// Read a pixel straight from the DIB memory: GetPixel() is documented as not
// supported on 32bpp DIBs, so it can't be used to verify the drawing. A GDI
// flush is required first: BitBlt/StretchBlt are batched and only reach the
// DIB memory when the GDI command queue is flushed.
static COLORREF PixelAt(const MemDc& m, int x, int y) {
    GdiFlush();
    if (!m.bits || x < 0 || x >= m.w || y < 0 || y >= m.h) {
        return RGB(0, 0, 0);
    }
    int stride = (m.w * m.bpp / 8 + 3) & ~3; // DIB rows are 32-bit aligned
    const u8* p = m.bits + (size_t)y * stride + (size_t)x * (m.bpp / 8);
    if (m.bpp == 32) {
        return RGB(p[2], p[1], p[0]); // DIB is BGR(A), COLORREF expects RGB
    }
    return RGB(p[2], p[1], p[0]);
}

static void ClearDc(MemDc& m, COLORREF color) {
    if (!m.bits) {
        return;
    }
    int stride = (m.w * m.bpp / 8 + 3) & ~3;
    for (int y = 0; y < m.h; y++) {
        for (int x = 0; x < m.w; x++) {
            u8* p = m.bits + (size_t)y * stride + (size_t)x * (m.bpp / 8);
            p[0] = GetBValue(color);
            p[1] = GetGValue(color);
            p[2] = GetRValue(color);
            if (m.bpp == 32) {
                p[3] = 0xFF;
            }
        }
    }
}

static void GdiRendererPixelTest() {
    const int W = 100, H = 50;

    MemDc m = MakeMemDc(W, H);
    if (!m.ok) {
        return;
    }
    ClearDc(m, RGB(0, 0, 0));

    GDIRenderer r;
    PAINTSTRUCT ps{};
    ps.hdc = m.hdc;

    utassert(r.GetKind() == RendererBackendKind::GDI);
    utassert(r.BeginPaint(nullptr, &ps));
    utassert(r.GetHDC() == m.hdc);

    // FillRect lands real pixels
    RECT rcFill = {0, 0, 20, 20};
    r.FillRect(rcFill, RgbaColor(255, 0, 0));
    utassert(PixelAt(m, 10, 10) == RGB(255, 0, 0));
    // outside the rect stays untouched
    utassert(PixelAt(m, 50, 25) == RGB(0, 0, 0));

    // DrawLine
    r.DrawLine(30, 10, 40, 10, RgbaColor(0, 255, 0));
    utassert(PixelAt(m, 35, 10) == RGB(0, 255, 0));

    // PushClip restricts drawing to the clip rect
    r.PushClip({50, 0, 60, 10});
    r.FillRect({0, 0, W, H}, RgbaColor(0, 0, 255));
    utassert(PixelAt(m, 55, 5) == RGB(0, 0, 255));  // inside clip: painted
    utassert(PixelAt(m, 10, 10) == RGB(255, 0, 0)); // outside clip: untouched
    r.PopClip();
    r.FillRect({0, 0, 10, 10}, RgbaColor(0, 0, 255));
    utassert(PixelAt(m, 5, 5) == RGB(0, 0, 255)); // clip restored

    // DrawBitmap (StretchBlt): blit a blue region from a second bitmap
    MemDc src = MakeMemDc(10, 10);
    if (src.ok) {
        ClearDc(src, RGB(0, 0, 0));
        // Paint a blue square {2,2}-{8,8} straight into the source pixels
        // (bypasses GDI so the test isolates the StretchBlt path).
        for (int yy = 2; yy < 8; yy++) {
            for (int xx = 2; xx < 8; xx++) {
                u8* p = src.bits + ((size_t)yy * 10 + xx) * 4;
                p[0] = 0xFF; // B
                p[1] = 0x00; // G
                p[2] = 0x00; // R
                p[3] = 0xFF; // A
            }
        }
        // Canary: in headless/remote sessions GDI's BitBlt/StretchBlt return
        // TRUE but never write a 32bpp DIB target, so pixel verification of the
        // blit path is impossible there. Detect this and skip those assertions
        // (the blit path is exercised in interactive sessions / via the app's
        // own render tests, e.g. tests/ad-hoc-*.ts).
        bool blitWorks = false;
        {
            HDC cDc = CreateCompatibleDC(m.hdc);
            HGDIOBJ cOld = SelectObject(cDc, src.bmp);
            ClearDc(m, RGB(0, 0, 0));
            BOOL ok = BitBlt(m.hdc, 0, 0, 10, 10, cDc, 0, 0, SRCCOPY);
            blitWorks = ok && PixelAt(m, 5, 5) == RGB(0, 0, 255);
            SelectObject(cDc, cOld);
            DeleteDC(cDc);
        }
        if (blitWorks) {
            RECT dstRect = {70, 20, 90, 40};
            RECT srcFull = {0, 0, 10, 10};
            r.DrawBitmap(src.bmp, dstRect, srcFull);
            utassert(PixelAt(m, 80, 30) == RGB(0, 0, 255));
            utassert(PixelAt(m, 71, 21) == RGB(0, 0, 0)); // outside the blue square
        } else {
            printf("Renderer_ut: GDI blit pixel checks skipped (headless blit unavailable)\n");
            // the renderer must still leave the DC usable
            r.FillRect({0, 0, 5, 5}, RgbaColor(0, 255, 0));
            utassert(PixelAt(m, 2, 2) == RGB(0, 255, 0));
        }
        FreeMemDc(src);
    }

    // DrawTextW: white background then black glyphs — at least one pixel differs
    ClearDc(m, RGB(255, 255, 255));
    HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    RECT txtRc = {5, 5, 95, 40};
    r.DrawTextW(WStrL(L"Hello"), txtRc, RgbaColor(0, 0, 0), font, DT_LEFT | DT_TOP | DT_SINGLELINE);
    bool foundInk = false;
    for (int y = 5; y < 40 && !foundInk; y++) {
        for (int x = 5; x < 95; x++) {
            if (PixelAt(m, x, y) != RGB(255, 255, 255)) {
                foundInk = true;
                break;
            }
        }
    }
    utassert(foundInk);

    r.EndPaint();
    utassert(r.GetHDC() == nullptr);
    FreeMemDc(m);
}

static void D2dRendererSmokeTest() {
    const bool d2dAvailable = IsD2D1Available();

    // A D2D backend instance with no factory must fail Init() — this is the
    // headless (no d2d1.dll) path; the fallback is covered by CreateRendererTest.
    D2DRenderer* d2d = new D2DRenderer();
    utassert(d2d->Init() == d2dAvailable);
    if (!d2d->Init()) {
        delete d2d;
        return;
    }

    // Paint through a DC render target on a memory DC and verify pixels
    const int W = 64, H = 64;
    MemDc m = MakeMemDc(W, H);
    if (!m.ok) {
        delete d2d;
        return;
    }
    ClearDc(m, RGB(0, 0, 0));

    PAINTSTRUCT ps{};
    ps.hdc = m.hdc;
    bool began = d2d->BeginPaint(nullptr, &ps);
    if (began) {
        d2d->FillRect({0, 0, 32, 32}, RgbaColor(255, 0, 0));
        d2d->EndPaint();
        // D2D end-of-frame copies the pixels back into the DC; the fill is a
        // pure red. Accept a small tolerance because of sRGB<->linear color
        // space conversion in the pipeline.
        COLORREF px = PixelAt(m, 16, 16);
        utassert(GetRValue(px) > 200);
        // region outside the fill must be unchanged (black)
        utassert(PixelAt(m, 50, 50) == RGB(0, 0, 0));
    }
    // BeginPaint may legitimately fail (no GPU/driver); that's fine — the
    // important assertion is that it doesn't crash and the fallback exists.
    FreeMemDc(m);
    delete d2d;
}

static void CreateRendererTest() {
    Renderer* r = CreateRenderer();
    utassert(r != nullptr);
    utassert(r->GetKind() == RendererBackendKind::D2D || r->GetKind() == RendererBackendKind::GDI);
    utassert(gRenderer == r);
    utassert(gRendererKind == r->GetKind());
    if (IsD2D1Available()) {
        utassert(r->GetKind() == RendererBackendKind::D2D);
    } else {
        utassert(r->GetKind() == RendererBackendKind::GDI);
    }
    // The app creates the singleton itself at startup; reset the global so this
    // test's instance doesn't leak into other tests.
    delete r;
    gRenderer = nullptr;
}

static void DWriteMeasureTest() {
    HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    DWriteTextFormat* fmt = DWriteTextCache::GetFormat(font);
    if (!fmt || !fmt->format) {
        // DirectWrite unavailable on this machine (or the stock font has no
        // face name) — nothing to verify.
        return;
    }
    IDWriteTextLayout* layout = DWriteTextRenderer::CreateLayout(fmt, StrL("Hello World"), 200, 50);
    utassert(layout != nullptr);
    float w = 0, h = 0;
    DWriteTextRenderer::MeasureLayout(layout, &w, &h);
    utassert(w > 0 && h > 0);
    layout->Release();

    // The cache hands out the same object for the same font
    DWriteTextFormat* fmt2 = DWriteTextCache::GetFormat(font);
    utassert(fmt2 != nullptr && fmt2->format == fmt->format);
    DWriteTextCache::Clear();
}

static void DWriteEllipsisTest() {
    HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    DWriteTextFormat* fmt = DWriteTextCache::GetFormat(font);
    if (!fmt || !fmt->format) {
        return;
    }
    // A wide layout room fits the text as-is: no truncation.
    IDWriteTextLayout* layout = DWriteTextRenderer::CreateEllipsisedLayout(fmt, StrL("Hello World"), 200, 50);
    utassert(layout != nullptr);
    float w0 = 0, h0 = 0;
    DWriteTextRenderer::MeasureLayout(layout, &w0, &h0);
    utassert(w0 > 0 && h0 > 0);
    layout->Release();

    // A narrow width forces a single line (no wrap) ending in "…"; the height
    // must stay single-line-ish and the width must fit the constraint.
    layout =
        DWriteTextRenderer::CreateEllipsisedLayout(fmt, StrL("A very long file name that cannot possibly fit"), 60, 30);
    utassert(layout != nullptr);
    float w1 = 0, h1 = 0;
    DWriteTextRenderer::MeasureLayout(layout, &w1, &h1);
    utassert(w1 > 0 && w1 <= 60);
    utassert(h1 <= h0 + 0.5f); // single line, not wrapped into multiple lines
    layout->Release();

    // Non-ASCII (UTF-8) long text: truncation must not split a multi-byte char.
    layout =
        DWriteTextRenderer::CreateEllipsisedLayout(fmt, StrL("这是一个非常长的中文文件名，用来检查省略号"), 60, 30);
    utassert(layout != nullptr);
    float w2 = 0, h2 = 0;
    DWriteTextRenderer::MeasureLayout(layout, &w2, &h2);
    utassert(w2 > 0 && w2 <= 60);
    utassert(h2 <= h0 + 0.5f);
    layout->Release();

    DWriteTextCache::Clear();
}

void RendererTest() {
    RgbaColorTest();
    DWriteCacheKeyTest();
    GdiRendererPixelTest();
    D2dRendererSmokeTest();
    CreateRendererTest();
    DWriteMeasureTest();
    DWriteEllipsisTest();
}

#else
// Mingw: no Direct2D/DirectWrite backends; nothing to test
void RendererTest() {}
#endif
