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

// Raw environment inputs for the low-end decision. Kept plain (no Win32
// handles) so ComputeHardwareProfile is pure and unit-testable.
struct HardwareInputs {
    int cpuCores = 0;               // logical processors (SYSTEM_INFO.dwNumberOfProcessors)
    u64 ramMB = 0;                  // physical RAM in MB (GlobalMemoryStatusEx)
    bool gpuAccelAvailable = false; // D2D1CreateFactory probe succeeded
    bool remoteSession = false;     // RDP / RemoteFX (GetSystemMetrics(SM_REMOTESESSION))
    bool virtualMachine = false;    // third-party hypervisor detected via CPUID
};

// Compute the degradation profile from raw environment inputs. Pure decision
// logic (no Win32 calls) so it can be unit-tested with synthetic inputs.
//
// A remote session or third-party VM forces the minimal GDI mode: the renderer
// may be software-only there, and DWM effects can produce black windows, so all
// effects (animation/mica/gradients/shadows/blur) are disabled regardless of
// the machine's raw specs.
inline HardwareProfile ComputeHardwareProfile(const HardwareInputs& in) {
    HardwareProfile p{};
    p.isLowCoreCount = in.cpuCores <= 2;
    p.isLowMemory = in.ramMB <= 4096;
    p.isLowGpu = !in.gpuAccelAvailable;
    bool remoteDegrade = in.remoteSession || in.virtualMachine;
    p.isLowEnd = p.isLowCoreCount || p.isLowMemory || p.isLowGpu || remoteDegrade;
    if (remoteDegrade) {
        p.disableAnimations = p.disableMica = p.disableGradients = p.disableShadows = p.disableBlur = true;
    } else if (p.isLowEnd) {
        p.disableAnimations = true;
        p.disableMica = true;
        p.disableGradients = p.isLowGpu;
        p.disableShadows = p.isLowGpu;
        p.disableBlur = true;
    }
    return p;
}

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
