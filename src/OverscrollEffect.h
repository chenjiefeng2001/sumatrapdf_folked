/* Copyright 2024 the SumatraPDF project authors (see AUTHORS body).
   License: Simplified BSD (see COPYING.BSD) */

// Overscroll effect: when the user scrolls past the document boundary, a
// visual stretch/bounce-back provides tactile feedback (like on macOS/iOS
// and modern browsers).

struct AnimProp;
struct MainWindow;

struct OverscrollState {
    // Accumulated overscroll offset (negative = past top, positive = past bottom)
    int offsetY = 0;

    // Maximum stretch distance before bounce-back kicks in
    int maxStretch = 120;

    // AnimProp for spring-back (dynamically owned so header stays lightweight)
    AnimProp* springAnim = nullptr;

    bool HasOverscroll() const { return offsetY != 0; }

    // Apply an additional overscroll delta. Returns true if there was a
    // boundary condition (i.e. we are past the scroll limit).
    bool ApplyDelta(int dy, MainWindow* win);

    // Animate the overscroll back to zero.
    void Release(MainWindow* win);

    // Get the current visual offset to apply when rendering.
    int GetVisualOffset() const { return offsetY; }
};
