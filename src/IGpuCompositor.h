/* Copyright 2024 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

// GPU compositing interface. Each backend (D2D, SDL3, sokol_gfx) implements
// this so the render cache can draw tiles without knowing which backend is
// active. The backend is selected at compile time via the build system; only
// one implementation is linked.

#pragma once

#include "wingui/Layout.h" // for Size

struct Pixmap;

struct IGpuCompositor {
    virtual ~IGpuCompositor() = default;

    // Create the backend. Returns null if init fails (no GPU, missing driver, etc.).
    // The returned pointer is owned by the caller and should be kept as a singleton.
    static IGpuCompositor* Create();

    // Upload a CPU-rendered Pixmap to a GPU texture. The returned texture is
    // owned by the Pixmap (d2dBitmap field on D2D, userdata on other backends)
    // and must be freed in FreePixmapNativeBitmap. Returns true on success.
    virtual bool UploadPixmap(Pixmap* pixmap) = 0;

    // Draw a previously uploaded pixmap onto the given HDC (device context).
    // bounds is the destination rectangle on screen (in device pixels).
    // Returns true if drawn via GPU, false if caller should fall back to GDI.
    virtual bool Draw(HDC hdc, const Pixmap* pixmap, const Rect& bounds) = 0;

    // Return true if the backend is available and usable.
    virtual bool IsAvailable() const = 0;
};

// Global singleton. Set by the backend implementation's Create().
extern IGpuCompositor* gGpuCompositor;