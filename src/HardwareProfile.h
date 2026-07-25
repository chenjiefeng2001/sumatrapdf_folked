/* Copyright 2024 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

// Hardware profile detection for Fast-Path degradation on low-end machines.
// Called once at startup; results inform animation, blur, gradient, and
// threading decisions throughout the app.

struct HardwareProfile {
    bool isLowEnd;       // overall flag: true when any threshold is crossed
    bool isLowCoreCount; // <= 2 logical processors
    bool isLowMemory;    // <= 4 GB physical RAM
    bool isLowGpu;       // no D2D hardware support, or old GPU

    bool disableAnimations; // all UI transitions (ease-out, fade) become instant
    bool disableMica;       // Win11 mica/acrylic backdrop -> solid color
    bool disableGradients;  // gradient fills become flat colors
    bool disableShadows;    // drop shadows removed
    bool disableBlur;       // blur effects removed
};

// Global singleton, set once during startup (SumatraStartup.cpp / WinMain).
extern HardwareProfile g_hwProfile;

// Perform hardware detection. Safe to call multiple times (idempotent).
// On Windows 7/8/10/11, uses GetSystemInfo / GlobalMemoryStatusEx /
// D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED) to probe.
void DetectHardware();

// Convenience helpers (inline for zero-overhead in hot paths)
inline bool AnimationsEnabled() {
    return !g_hwProfile.disableAnimations;
}
inline bool MicaEnabled() {
    return !g_hwProfile.disableMica;
}
inline bool GradientsEnabled() {
    return !g_hwProfile.disableGradients;
}
inline bool ShadowsEnabled() {
    return !g_hwProfile.disableShadows;
}
inline bool BlurEnabled() {
    return !g_hwProfile.disableBlur;
}
