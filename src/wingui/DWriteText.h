/* Copyright 2024 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

// DirectWrite text rendering wrapper. Provides ClearType text rendering with
// modern typographic features (ligatures, variable fonts, emoji fallback) as an
// alternative to GDI's ExtTextOut.

struct IDWriteFactory;
struct IDWriteTextFormat;
struct IDWriteTextLayout;
struct ID2D1RenderTarget;

// A pre-built DirectWrite text format object cached by font/weight/style.
// Create via DWriteTextCache::GetFormat() and reuse across paint cycles.
struct DWriteTextFormat {
    IDWriteTextFormat* format = nullptr;

    // Format is owned by DWriteTextCache; do not Release().
};

// Cache key extracted from an HFONT: family name, size in DIPs, weight
// (DWRITE_FONT_WEIGHT: 400 normal, 700 bold) and style (DWRITE_FONT_STYLE).
// Sized to fit any font family name (LF_FACESIZE = 32 WCHARs).
struct DWriteFormatKey {
    WCHAR family[32] = {};
    float size = 0;
    int weight = 0;
    int style = 0;
};

// Pure logic: whether two keys identify the same text format. Unit-tested
// headlessly in Renderer_ut.cpp.
static inline bool DWriteCacheKeysEqual(const DWriteFormatKey& a, const DWriteFormatKey& b) {
    if (a.size != b.size || a.weight != b.weight || a.style != b.style) {
        return false;
    }
    return wcsncmp(a.family, b.family, dimof(a.family)) == 0;
}

// Lazily built IDWriteTextFormat cache keyed by HFONT properties. Formats are
// owned by the cache and must not be Released by callers. UI thread only.
struct DWriteTextCache {
    // Get (and lazily create) a cached text format for the given HFONT.
    // Returns nullptr when DirectWrite is unavailable or the font is invalid.
    static DWriteTextFormat* GetFormat(HFONT font);

    // Drop all cached formats (releases the IDWriteTextFormat objects). Call
    // on shutdown so the factory can be torn down.
    static void Clear();
};

// Layout-and-render helper: creates an IDWriteTextLayout from a format + string,
// measures it, and draws it onto an ID2D1RenderTarget.
struct DWriteTextRenderer {
    // Create a text layout from the cached format.
    static IDWriteTextLayout* CreateLayout(DWriteTextFormat* fmt, Str text, float maxWidth, float maxHeight);

    // Draw the layout onto a D2D render target at (x, y).
    static void DrawLayout(ID2D1RenderTarget* rt, IDWriteTextLayout* layout, float x, float y, COLORREF color);

    // Measure the layout's actual size (may be less than max).
    static void MeasureLayout(IDWriteTextLayout* layout, /*out*/ float* w, /*out*/ float* h);
};

// Global DirectWrite factory access (lazily initialized by D2DRenderer).
extern IDWriteFactory* gDWriteFactory;
