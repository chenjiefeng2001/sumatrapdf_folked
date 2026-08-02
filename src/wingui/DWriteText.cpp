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
    HRESULT hr =
        gDWriteFactory->CreateTextLayout(wtext.s, (UINT32)wtext.len, fmt->format, maxWidth, maxHeight, &layout);
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

    D2D1_COLOR_F d2dColor =
        D2D1::ColorF(GetRValue(color) / 255.0f, GetGValue(color) / 255.0f, GetBValue(color) / 255.0f, 1.0f);
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

// ── DWriteTextCache ──────────────────────────────────────────────────
// Fixed-capacity cache of IDWriteTextFormat objects keyed by HFONT-derived
// properties (family/size/weight/style). The UI uses only a handful of fonts
// (default GUI font, menu font, bold variants) so a small fixed array with a
// linear scan is simpler and allocation-free. Oldest entry is evicted when the
// cache fills up.

struct DWriteCacheEntry {
    DWriteFormatKey key = {};
    DWriteTextFormat fmt = {};
};

static DWriteCacheEntry gCache[16];
static int gCacheCount = 0;

static void KeyFromHfont(HFONT font, /*out*/ DWriteFormatKey* key) {
    LOGFONTW lf{};
    if (font) {
        GetObjectW(font, sizeof(lf), &lf);
    } else {
        // Default GUI font (matches GetDefaultGuiFont): Segoe UI, 14 px.
        wcscpy_s(lf.lfFaceName, L"Segoe UI");
        lf.lfHeight = -14;
        lf.lfWeight = FW_NORMAL;
    }
    // Copy the face name, truncating at LF_FACESIZE (can't happen in practice).
    wcsncpy_s(key->family, dimof(key->family), lf.lfFaceName, _TRUNCATE);
    key->family[dimof(key->family) - 1] = 0;
    // |lfHeight| is in pixels; treat it as DIPs at the standard 96 DPI so the
    // size stays consistent for the cache key. DPI-aware sizing can be added
    // later without breaking the key structure.
    key->size = lf.lfHeight ? (float)std::abs(lf.lfHeight) : 14.0f;
    key->weight = lf.lfWeight ? (int)lf.lfWeight : (int)FW_NORMAL;
    key->style = lf.lfItalic ? (int)DWRITE_FONT_STYLE_ITALIC : (int)DWRITE_FONT_STYLE_NORMAL;
}

DWriteTextFormat* DWriteTextCache::GetFormat(HFONT font) {
    EnsureDWriteFactory();
    if (!gDWriteFactory) {
        return nullptr;
    }
    DWriteFormatKey key{};
    KeyFromHfont(font, &key);
    if (key.family[0] == 0 || key.size <= 0) {
        return nullptr;
    }
    for (int i = 0; i < gCacheCount; i++) {
        if (DWriteCacheKeysEqual(gCache[i].key, key)) {
            return &gCache[i].fmt;
        }
    }
    if (gCacheCount >= (int)dimof(gCache)) {
        // Cache full: evict the oldest entry.
        gCache[0].fmt.format->Release();
        gCache[0].fmt.format = nullptr;
        for (int i = 1; i < gCacheCount; i++) {
            gCache[i - 1] = gCache[i];
        }
        gCacheCount--;
    }
    DWriteCacheEntry* entry = &gCache[gCacheCount];
    entry->key = key;
    HRESULT hr = gDWriteFactory->CreateTextFormat(key.family, nullptr, (DWRITE_FONT_WEIGHT)key.weight,
                                                  (DWRITE_FONT_STYLE)key.style, DWRITE_FONT_STRETCH_NORMAL, key.size,
                                                  L"", &entry->fmt.format);
    if (FAILED(hr) || !entry->fmt.format) {
        entry->fmt.format = nullptr;
        return nullptr;
    }
    gCacheCount++;
    return &entry->fmt;
}

void DWriteTextCache::Clear() {
    for (int i = 0; i < gCacheCount; i++) {
        if (gCache[i].fmt.format) {
            gCache[i].fmt.format->Release();
            gCache[i].fmt.format = nullptr;
        }
        gCache[i] = {};
    }
    gCacheCount = 0;
}

#else
// Mingw stub — DWrite not supported
void InitDWrite() {}
#endif
