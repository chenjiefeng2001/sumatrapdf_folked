/* Copyright 2024 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

#include "base/Base.h"
#include "base/Win.h"

#ifdef _MSC_VER
#include <d2d1.h>
#include <dwrite.h>
#include "GpuBackend.h"
#include "wingui/DWriteText.h"

#pragma comment(lib, "dwrite.lib")

// Global DirectWrite factory. This is lazily initialized on first use.
// Note: D2D's ID2D1RenderTarget internally associates with its own DWrite
// factory from the same process, so it's safe to create IDWriteTextLayout
// with this factory and render it on any D2D render target in the same app.
// Do NOT create a second IDWriteFactory elsewhere — use this one.
IDWriteFactory* gDWriteFactory = nullptr;

static void EnsureDWriteFactory() {
    if (gDWriteFactory) {
        return;
    }
    HRESULT hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                     reinterpret_cast<IUnknown**>(&gDWriteFactory));
    if (FAILED(hr)) {
        gDWriteFactory = nullptr;
    }
}

IDWriteTextLayout* DWriteTextRenderer::CreateLayout(DWriteTextFormat* fmt, Str text, float maxWidth, float maxHeight) {
    EnsureDWriteFactory();
    if (!gDWriteFactory || !fmt || !fmt->format || text.len == 0) {
        return nullptr;
    }
    // Convert the UTF-8 Str to a WStr for DirectWrite
    TempWStr wtext = ToWStrTemp(text);
    if (wtext.s == nullptr || wtext.len == 0) {
        return nullptr;
    }
    IDWriteTextLayout* layout = nullptr;
    HRESULT hr = gDWriteFactory->CreateTextLayout(wtext.s, (UINT32)wtext.len, fmt->format, maxWidth, maxHeight, &layout);
    if (FAILED(hr)) {
        return nullptr;
    }
    return layout;
}

void DWriteTextRenderer::DrawLayout(ID2D1RenderTarget* rt, IDWriteTextLayout* layout, float x, float y,
                                    COLORREF color) {
    if (!rt || !layout) {
        return;
    }

    // IMPORTANT: DrawTextLayout 必须在 BeginDraw/EndDraw 配对之间调用。
    // 调用者必须先调用 rt->BeginDraw()，在完成所有绘制后调用 rt->EndDraw()。
    // 这个函数本身不调用 BeginDraw/EndDraw，因为它通常被用在已有的
    // BeginDraw/EndDraw 对之间（例如与 DrawBitmap 混合使用）。
    //
    // 特别说明：ID2D1RenderTarget::CreateSolidColorBrush 可以在 BeginDraw
    // 外部调用（它是资源创建），但 DrawTextLayout 必须在 BeginDraw 内部。
    // 如果 rt 不处于绘制状态，DrawTextLayout 行为未定义 → 可能 CFG 崩溃。
    //
    // 画刷始终新创建并立即释放。高频调用场景下（如逐帧文本绘制），
    // 调用者应在外部缓存画刷而非每次都创建。

    D2D1_COLOR_F d2dColor = D2D1::ColorF(GetRValue(color) / 255.0f, GetGValue(color) / 255.0f,
                                          GetBValue(color) / 255.0f, 1.0f);
    ID2D1SolidColorBrush* brush = nullptr;
    HRESULT hr = rt->CreateSolidColorBrush(d2dColor, &brush);
    if (FAILED(hr) || !brush) {
        return;
    }
    rt->DrawTextLayout(D2D1::Point2F(x, y), layout, brush);
    brush->Release();
}

void DWriteTextRenderer::MeasureLayout(IDWriteTextLayout* layout, /*out*/ float* w, /*out*/ float* h) {
    if (!layout) {
        if (w) *w = 0;
        if (h) *h = 0;
        return;
    }
    DWRITE_TEXT_METRICS metrics;
    HRESULT hr = layout->GetMetrics(&metrics);
    if (SUCCEEDED(hr)) {
        if (w) *w = metrics.widthIncludingTrailingWhitespace;
        if (h) *h = metrics.height;
    } else {
        if (w) *w = 0;
        if (h) *h = 0;
    }
}

#else
// Mingw stub — DWrite not supported
void InitDWrite() {}
#endif
