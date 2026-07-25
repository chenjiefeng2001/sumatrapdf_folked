/* Copyright 2024 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

#include "base/Base.h"
#include "base/Win.h"
#include "base/WinDynCalls.h"

#include "HardwareProfile.h"

#include "base/Log.h"

HardwareProfile g_hwProfile = {false, false, false, false, false, false, false, false, false};

void DetectHardware() {
    // Reset to defaults
    g_hwProfile = {false, false, false, false, false, false, false, false, false};

    // 1. CPU core count
    SYSTEM_INFO sysInfo;
    GetSystemInfo(&sysInfo);
    g_hwProfile.isLowCoreCount = sysInfo.dwNumberOfProcessors <= 2;

    // 2. Physical RAM
    MEMORYSTATUSEX memInfo;
    memInfo.dwLength = sizeof(MEMORYSTATUSEX);
    GlobalMemoryStatusEx(&memInfo);
    u64 ramMB = memInfo.ullTotalPhys / (1024 * 1024);
    g_hwProfile.isLowMemory = ramMB <= 4096;

    // 3. GPU capability via D2D factory probe (lightweight: try to create,
    //    if it fails or returns software WARP, treat as low GPU).
    //    We don't keep the factory; just probe and release.
    {
        HMODULE d2d1 = LoadLibraryW(L"d2d1.dll");
        if (d2d1) {
            typedef HRESULT(WINAPI * D2D1CreateFactoryFn)(void*, REFIID, void*, void**);
            D2D1CreateFactoryFn fnCreate = (D2D1CreateFactoryFn)GetProcAddress(d2d1, "D2D1CreateFactory");
            if (fnCreate) {
                IUnknown* factory = nullptr;
                // D2D1_FACTORY_TYPE_SINGLE_THREADED = 0
                // IID_ID2D1Factory
                IID iid = {0x06152247, 0x6f50, 0x465a, {0x92, 0x45, 0x11, 0x8b, 0xfd, 0x3b, 0x60, 0x07}};
                HRESULT hr = fnCreate((void*)0, iid, nullptr, (void**)&factory);
                if (SUCCEEDED(hr) && factory) {
                    // Check if the factory is hardware-accelerated by examining
                    // the factory's rendering mode. We use a simpler heuristic:
                    // if D2D1CreateFactory succeeded, assume hardware exists;
                    // we only flag low GPU if creation itself failed.
                    factory->Release();
                } else {
                    // D2D1CreateFactory failed => no D2D at all (very old system)
                    g_hwProfile.isLowGpu = true;
                }
            } else {
                g_hwProfile.isLowGpu = true;
            }
            FreeLibrary(d2d1);
        } else {
            // No d2d1.dll => pre-Win7 or stripped Win7
            g_hwProfile.isLowGpu = true;
        }
    }

    // 4. Composite decision
    g_hwProfile.isLowEnd = g_hwProfile.isLowCoreCount || g_hwProfile.isLowMemory || g_hwProfile.isLowGpu;

    // 5. Set degradation flags based on profile
    if (g_hwProfile.isLowEnd) {
        g_hwProfile.disableAnimations = true;
        g_hwProfile.disableMica = true;
        g_hwProfile.disableGradients = g_hwProfile.isLowGpu;
        g_hwProfile.disableShadows = g_hwProfile.isLowGpu;
        g_hwProfile.disableBlur = true;
    }

    // Log the result for diagnostics
    logf("HardwareProfile: lowEnd=%d", (int)g_hwProfile.isLowEnd);
    logf("  cores=%lu", sysInfo.dwNumberOfProcessors);
    logf("  ram=%lluMB lowGpu=%d", ramMB, (int)g_hwProfile.isLowGpu);
    logf("  anim=%d mica=%d", (int)g_hwProfile.disableAnimations, (int)g_hwProfile.disableMica);
    logf("  grad=%d shadows=%d blur=%d", (int)g_hwProfile.disableGradients, (int)g_hwProfile.disableShadows,
         (int)g_hwProfile.disableBlur);
}
