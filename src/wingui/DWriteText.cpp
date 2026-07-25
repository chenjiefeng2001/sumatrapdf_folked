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

IDWriteFactory* gDWriteFactory = nullptr;

void InitDWrite() {
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
