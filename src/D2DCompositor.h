/* Copyright 2024 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

// Direct2D implementation of IGpuCompositor.
// Only compiles with MSVC (D2D1 headers not available on MingW).

#ifdef _MSC_VER

#include <d2d1.h>
#include <d2d1helper.h>
#pragma comment(lib, "d2d1.lib")

#include "IGpuCompositor.h"

struct D2DCompositor : IGpuCompositor {
    D2DCompositor();
    ~D2DCompositor() override;

    static IGpuCompositor* Create();

    bool UploadPixmap(Pixmap* pixmap) override;
    bool Draw(HDC hdc, const Pixmap* pixmap, const Rect& bounds) override;
    bool IsAvailable() const override { return isAvailable; }

  private:
    // Get a per-HDC D2D render target.
    ID2D1DCRenderTarget* GetRenderTarget(HDC hdc);

    ID2D1Factory* factory = nullptr;
    ID2D1DCRenderTarget* cachedRT = nullptr;
    HDC cachedHDC = nullptr;
    bool isAvailable = false;
};

#endif // _MSC_VER