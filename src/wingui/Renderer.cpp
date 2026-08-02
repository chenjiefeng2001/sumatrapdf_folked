/* Copyright 2024 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

// Unified control drawing backends. GDIRenderer paints directly onto the HDC
// from BeginPaint; D2DRenderer paints through an ID2D1DCRenderTarget bound to
// that HDC with text going through DirectWrite (ClearType). The app picks one
// once at startup via CreateRenderer(): D2D when d2d1.dll is available, GDI
// otherwise (see risk #3 in the UI modernization report).
//
// D2D is loaded dynamically (GetProcAddress on d2d1.dll) rather than linked:
//   * test_util.exe can compile this file without pulling in d2d1.lib
//   * on machines without d2d1.dll the D2D backend simply never inits and the
//     app falls back to GDI, matching the project convention of loading
//     optional OS APIs dynamically (WinDynCalls, pointer input, etc.)

#include "base/Base.h"
#include "base/Win.h"

#ifdef _MSC_VER
#include <d2d1.h>
#include <d2d1helper.h>
#include <dwrite.h>
#include "wingui/Renderer.h"
#include "wingui/DWriteText.h"

// last: Log.h
#include "base/Log.h"

typedef HRESULT(WINAPI* FnD2D1CreateFactory)(D2D1_FACTORY_TYPE factoryType, REFIID riid,
                                             const D2D1_FACTORY_OPTIONS* factoryOptions, void** factory);

static FnD2D1CreateFactory gFnD2D1CreateFactory = nullptr;
static bool gD2D1Checked = false;

static void EnsureD2D1Loaded() {
    if (gD2D1Checked) {
        return;
    }
    gD2D1Checked = true;
    HMODULE d2d1 = LoadLibraryW(L"d2d1.dll");
    if (d2d1) {
        gFnD2D1CreateFactory = (FnD2D1CreateFactory)GetProcAddress(d2d1, "D2D1CreateFactory");
    }
}

bool IsD2D1Available() {
    EnsureD2D1Loaded();
    return gFnD2D1CreateFactory != nullptr;
}

Renderer* gRenderer = nullptr;
RendererBackendKind gRendererKind = RendererBackendKind::GDI;

// Default: SVG rasterization is an engine capability (mupdf), see the comment
// in Renderer.h. No standalone drawing backend can rasterize SVG cheaply.
void Renderer::DrawSvgIcon(const char* svgData, int svgLen, RECT rc, RgbaColor color) {
    (void)svgData;
    (void)svgLen;
    (void)rc;
    (void)color;
}

// ── GDIRenderer ──────────────────────────────────────────────────────

static COLORREF ToColorref(RgbaColor c) {
    return RGB(c.r, c.g, c.b);
}

RendererBackendKind GDIRenderer::GetKind() {
    return RendererBackendKind::GDI;
}

bool GDIRenderer::BeginPaint(HWND hwndIn, PAINTSTRUCT* ps) {
    if (!ps || !ps->hdc) {
        return false;
    }
    hwnd = hwndIn;
    hdc = ps->hdc;
    return true;
}

void GDIRenderer::EndPaint() {
    hdc = nullptr;
    hwnd = nullptr;
}

HDC GDIRenderer::GetHDC() {
    return hdc;
}

void GDIRenderer::FillRect(RECT rc, RgbaColor color) {
    if (!hdc) {
        return;
    }
    HBRUSH br = CreateSolidBrush(ToColorref(color));
    if (br) {
        ::FillRect(hdc, &rc, br);
        DeleteObject(br);
    }
}

void GDIRenderer::FillRectF(const RectF& rc, RgbaColor color) {
    RECT r = {(LONG)rc.x, (LONG)rc.y, (LONG)(rc.x + rc.dx), (LONG)(rc.y + rc.dy)};
    FillRect(r, color);
}

void GDIRenderer::DrawRect(RECT rc, RgbaColor color, float strokeWidth) {
    if (!hdc) {
        return;
    }
    HPEN pen = CreatePen(PS_SOLID, (int)std::max(1.0f, strokeWidth), ToColorref(color));
    if (!pen) {
        return;
    }
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Rectangle(hdc, rc.left, rc.top, rc.right, rc.bottom);
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(pen);
}

void GDIRenderer::DrawLine(int x1, int y1, int x2, int y2, RgbaColor color, float strokeWidth) {
    if (!hdc) {
        return;
    }
    HPEN pen = CreatePen(PS_SOLID, (int)std::max(1.0f, strokeWidth), ToColorref(color));
    if (!pen) {
        return;
    }
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    MoveToEx(hdc, x1, y1, nullptr);
    LineTo(hdc, x2, y2);
    SelectObject(hdc, oldPen);
    DeleteObject(pen);
}

void GDIRenderer::DrawText(Str text, RECT rc, RgbaColor color, HFONT font, UINT format) {
    if (!hdc || !text) {
        return;
    }
    TempWStr wtext = ToWStrTemp(text);
    if (!wtext.s) {
        return;
    }
    int prevMode = SetBkMode(hdc, TRANSPARENT);
    COLORREF prevCol = SetTextColor(hdc, ToColorref(color));
    HGDIOBJ prevFont = font ? SelectObject(hdc, font) : nullptr;
    ::DrawTextW(hdc, wtext.s, wtext.len, &rc, format);
    if (prevFont) {
        SelectObject(hdc, prevFont);
    }
    SetTextColor(hdc, prevCol);
    SetBkMode(hdc, prevMode);
}

void GDIRenderer::DrawTextW(WStr text, RECT rc, RgbaColor color, HFONT font, UINT format) {
    if (!hdc || !text) {
        return;
    }
    // WStr may be a substring without a NUL terminator: copy to guarantee it.
    WCHAR* ws = CWStrTemp(text);
    if (!ws) {
        return;
    }
    int prevMode = SetBkMode(hdc, TRANSPARENT);
    COLORREF prevCol = SetTextColor(hdc, ToColorref(color));
    HGDIOBJ prevFont = font ? SelectObject(hdc, font) : nullptr;
    ::DrawTextW(hdc, ws, text.len, &rc, format);
    if (prevFont) {
        SelectObject(hdc, prevFont);
    }
    SetTextColor(hdc, prevCol);
    SetBkMode(hdc, prevMode);
}

void GDIRenderer::DrawBitmap(HBITMAP hbmp, RECT dst, RECT src) {
    if (!hdc || !hbmp) {
        return;
    }
    HDC srcDc = CreateCompatibleDC(hdc);
    if (!srcDc) {
        return;
    }
    HGDIOBJ old = SelectObject(srcDc, hbmp);
    StretchBlt(hdc, dst.left, dst.top, RectDx(dst), RectDy(dst), srcDc, src.left, src.top, RectDx(src), RectDy(src),
               SRCCOPY);
    SelectObject(srcDc, old);
    DeleteDC(srcDc);
}

void GDIRenderer::PushClip(RECT rc) {
    if (!hdc) {
        return;
    }
    savedDc = SaveDC(hdc);
    IntersectClipRect(hdc, rc.left, rc.top, rc.right, rc.bottom);
}

void GDIRenderer::PopClip() {
    if (hdc && savedDc) {
        RestoreDC(hdc, savedDc);
        savedDc = 0;
    }
}

void GDIRenderer::SetTextAntialias(bool enable) {
    // GDI has no per-DC text quality switch: ClearType/aliasing is a property
    // of the font (LOGFONT::lfQuality) chosen when the font is created. This
    // hint is a no-op on the GDI backend; the D2D backend honors it at runtime.
    (void)enable;
}

// ── D2DRenderer ──────────────────────────────────────────────────────

D2DRenderer::D2DRenderer() {
    EnsureD2D1Loaded();
    if (!gFnD2D1CreateFactory) {
        return;
    }
    HRESULT hr =
        gFnD2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), nullptr, (void**)&factory);
    if (FAILED(hr)) {
        factory = nullptr;
    }
}

D2DRenderer::~D2DRenderer() {
    if (rt) {
        rt->Release();
    }
    if (factory) {
        factory->Release();
    }
}

bool D2DRenderer::Init() {
    return factory != nullptr;
}

RendererBackendKind D2DRenderer::GetKind() {
    return RendererBackendKind::D2D;
}

bool D2DRenderer::BeginPaint(HWND hwndIn, PAINTSTRUCT* ps) {
    if (!ps || !ps->hdc || !factory) {
        return false;
    }
    hwnd = hwndIn;
    hdc = ps->hdc;
    if (!rt) {
        D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 0,
            0, D2D1_RENDER_TARGET_USAGE_GDI_COMPATIBLE);
        HRESULT hr = factory->CreateDCRenderTarget(&props, &rt);
        if (FAILED(hr) || !rt) {
            return false;
        }
    }
    // Determine the DC extent: window client rect for real windows, DC clip
    // box / bitmap size for memory DCs (see GpuBackend::GetRenderTarget).
    RECT rc = {};
    if (hwnd) {
        rc = ClientRECT(hwnd);
    } else if (GetClipBox(hdc, &rc) == ERROR || (rc.right <= 0 || rc.bottom <= 0)) {
        int cx = GetDeviceCaps(hdc, HORZRES);
        int cy = GetDeviceCaps(hdc, VERTRES);
        if (cx > 0 && cy > 0) {
            rc.right = cx;
            rc.bottom = cy;
        } else {
            rc.right = 0;
            rc.bottom = 0;
        }
    }
    if (rc.right <= rc.left || rc.bottom <= rc.top) {
        return false;
    }
    HRESULT hr = rt->BindDC(hdc, &rc);
    if (FAILED(hr)) {
        return false;
    }
    rt->BeginDraw();
    return true;
}

void D2DRenderer::EndPaint() {
    if (rt) {
        HRESULT hr = rt->EndDraw();
        if (FAILED(hr)) {
            // D2DERR_RECREATE_TARGET etc.: the caller keeps drawing via GDI on
            // its next paint cycle (BeginPaint will lazily rebind).
            logfa("[Renderer] D2D EndDraw failed HRESULT=0x%08X\n", (unsigned)hr);
        }
    }
    hdc = nullptr;
    hwnd = nullptr;
}

HDC D2DRenderer::GetHDC() {
    return hdc;
}

ID2D1SolidColorBrush* D2DRenderer::CreateBrush(RgbaColor color) {
    if (!rt) {
        return nullptr;
    }
    ID2D1SolidColorBrush* brush = nullptr;
    D2D1_COLOR_F c = D2D1::ColorF(color.r / 255.0f, color.g / 255.0f, color.b / 255.0f, color.a / 255.0f);
    HRESULT hr = rt->CreateSolidColorBrush(c, &brush);
    if (FAILED(hr) || !brush) {
        return nullptr;
    }
    return brush;
}

void D2DRenderer::FillRect(RECT rc, RgbaColor color) {
    if (!rt) {
        return;
    }
    ID2D1SolidColorBrush* brush = CreateBrush(color);
    if (!brush) {
        return;
    }
    rt->FillRectangle(D2D1::RectF((float)rc.left, (float)rc.top, (float)rc.right, (float)rc.bottom), brush);
    brush->Release();
}

void D2DRenderer::FillRectF(const RectF& rc, RgbaColor color) {
    if (!rt) {
        return;
    }
    ID2D1SolidColorBrush* brush = CreateBrush(color);
    if (!brush) {
        return;
    }
    rt->FillRectangle(D2D1::RectF(rc.x, rc.y, rc.x + rc.dx, rc.y + rc.dy), brush);
    brush->Release();
}

void D2DRenderer::DrawRect(RECT rc, RgbaColor color, float strokeWidth) {
    if (!rt) {
        return;
    }
    ID2D1SolidColorBrush* brush = CreateBrush(color);
    if (!brush) {
        return;
    }
    rt->DrawRectangle(D2D1::RectF((float)rc.left, (float)rc.top, (float)rc.right, (float)rc.bottom), brush,
                      strokeWidth);
    brush->Release();
}

void D2DRenderer::DrawLine(int x1, int y1, int x2, int y2, RgbaColor color, float strokeWidth) {
    if (!rt) {
        return;
    }
    ID2D1SolidColorBrush* brush = CreateBrush(color);
    if (!brush) {
        return;
    }
    rt->DrawLine(D2D1::Point2F((float)x1, (float)y1), D2D1::Point2F((float)x2, (float)y2), brush, strokeWidth);
    brush->Release();
}

void D2DRenderer::DrawText(Str text, RECT rc, RgbaColor color, HFONT font, UINT format) {
    if (!rt || !text) {
        return;
    }
    DWriteTextFormat* fmt = DWriteTextCache::GetFormat(font);
    if (!fmt || !fmt->format) {
        return;
    }
    float maxW = (float)(rc.right - rc.left);
    float maxH = (float)(rc.bottom - rc.top);
    IDWriteTextLayout* layout = DWriteTextRenderer::CreateLayout(fmt, text, maxW, maxH);
    if (!layout) {
        return;
    }
    float x = (float)rc.left;
    float y = (float)rc.top;
    if (format & (DT_CENTER | DT_RIGHT | DT_VCENTER)) {
        float lw = 0, lh = 0;
        DWriteTextRenderer::MeasureLayout(layout, &lw, &lh);
        if (format & DT_CENTER) {
            x += (maxW - lw) / 2;
        } else if (format & DT_RIGHT) {
            x += maxW - lw;
        }
        if (format & DT_VCENTER) {
            y += (maxH - lh) / 2;
        }
    }
    DWriteTextRenderer::DrawLayout(rt, layout, x, y, ToColorref(color));
    layout->Release();
}

void D2DRenderer::DrawTextW(WStr text, RECT rc, RgbaColor color, HFONT font, UINT format) {
    TempStr utf8 = ToUtf8Temp(text);
    if (utf8) {
        DrawText(utf8, rc, color, font, format);
    }
}

void D2DRenderer::DrawBitmap(HBITMAP hbmp, RECT dst, RECT src) {
    if (!rt || !hbmp) {
        return;
    }
    BITMAP bm{};
    if (!GetObjectW(hbmp, sizeof(bm), &bm) || (bm.bmBitsPixel != 32 && bm.bmBitsPixel != 24)) {
        // Unusual formats aren't worth the conversion; callers keep 32bpp DIBs.
        return;
    }
    int srcW = RectDx(src) > 0 ? RectDx(src) : bm.bmWidth;
    int srcH = RectDy(src) > 0 ? RectDy(src) : bm.bmHeight;
    int stride = ((srcW * 4) + 3) & ~3;
    u8* pixels = (u8*)AllocZero(srcH * stride, 1);
    if (!pixels) {
        return;
    }
    // Read the source region as 32bpp BGRA, top-down.
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = srcW;
    bi.bmiHeader.biHeight = -srcH;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    HDC tmpDc = CreateCompatibleDC(nullptr);
    HGDIOBJ old = SelectObject(tmpDc, hbmp);
    int lines = GetDIBits(tmpDc, hbmp, 0, srcH, pixels, &bi, DIB_RGB_COLORS);
    SelectObject(tmpDc, old);
    DeleteDC(tmpDc);
    if (lines != srcH) {
        free(pixels);
        return;
    }
    D2D1_BITMAP_PROPERTIES props =
        D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
    ID2D1Bitmap* d2dBmp = nullptr;
    HRESULT hr = rt->CreateBitmap(D2D1::SizeU((UINT32)srcW, (UINT32)srcH), pixels, stride, &props, &d2dBmp);
    free(pixels);
    if (FAILED(hr) || !d2dBmp) {
        return;
    }
    D2D1_RECT_F dstRc = D2D1::RectF((float)dst.left, (float)dst.top, (float)dst.right, (float)dst.bottom);
    D2D1_RECT_F srcRc = D2D1::RectF((float)src.left, (float)src.top, (float)src.right, (float)src.bottom);
    rt->DrawBitmap(d2dBmp, &dstRc, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, &srcRc);
    d2dBmp->Release();
}

void D2DRenderer::PushClip(RECT rc) {
    if (!rt) {
        return;
    }
    rt->PushAxisAlignedClip(D2D1::RectF((float)rc.left, (float)rc.top, (float)rc.right, (float)rc.bottom),
                            D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
}

void D2DRenderer::PopClip() {
    if (rt) {
        rt->PopAxisAlignedClip();
    }
}

void D2DRenderer::SetTextAntialias(bool enable) {
    if (rt) {
        rt->SetTextAntialiasMode(enable ? D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE : D2D1_TEXT_ANTIALIAS_MODE_ALIASED);
    }
}

// ── Factory ─────────────────────────────────────────────────────────

Renderer* CreateRenderer() {
    auto* d2d = new D2DRenderer();
    if (d2d->Init()) {
        gRenderer = d2d;
        gRendererKind = RendererBackendKind::D2D;
        return d2d;
    }
    delete d2d;
    gRenderer = new GDIRenderer();
    gRendererKind = RendererBackendKind::GDI;
    return gRenderer;
}

#else
// Mingw stub — Direct2D/DirectWrite not supported on the Mingw toolchain
Renderer* gRenderer = nullptr;
RendererBackendKind gRendererKind = RendererBackendKind::GDI;

void Renderer::DrawSvgIcon(const char* svgData, int svgLen, RECT rc, RgbaColor color) {}

bool IsD2D1Available() {
    return false;
}

Renderer* CreateRenderer() {
    gRenderer = new GDIRenderer();
    gRendererKind = RendererBackendKind::GDI;
    return gRenderer;
}
#endif
