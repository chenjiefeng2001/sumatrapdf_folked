/* Copyright 2024 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

#include "base/Base.h"
#include "base/Win.h"
#include "base/WinDynCalls.h"
#include <intrin.h>

#include "HardwareProfile.h"

#include "base/Log.h"

// third-party hypervisor vendor strings reported by CPUID leaf 0
// "Microsoft Hv" is deliberately NOT treated as a VM: Windows 11 enables
// VBS/Hyper-V by default on bare metal, so it's not a reliable VM indicator.
static bool VendorEquals(const char* vendor, const char* brand) {
    return memcmp(vendor, brand, strlen(brand)) == 0;
}

static bool IsThirdPartyVirtualMachine() {
    int cpuInfo[4] = {};
    __cpuid(cpuInfo, 0);
    char vendor[13];
    memcpy(vendor, &cpuInfo[1], 4);
    memcpy(vendor + 4, &cpuInfo[3], 4);
    memcpy(vendor + 8, &cpuInfo[2], 4);
    vendor[12] = 0;
    return VendorEquals(vendor, "VMwareVMware") || VendorEquals(vendor, "VBoxVBoxVBox") ||
           VendorEquals(vendor, "KVMKVMKVM") || VendorEquals(vendor, "XenVMMXenVMM") ||
           VendorEquals(vendor, "TCGTCGTCGTCG") || VendorEquals(vendor, "lrpepyh vr");
}

HardwareProfile g_hwProfile = {false, false, false, false, false, false, false, false, false};

void DetectHardware() {
    HardwareInputs in{};

    // 1. CPU core count
    SYSTEM_INFO sysInfo;
    GetSystemInfo(&sysInfo);
    in.cpuCores = (int)sysInfo.dwNumberOfProcessors;

    // 2. Physical RAM
    MEMORYSTATUSEX memInfo;
    memInfo.dwLength = sizeof(MEMORYSTATUSEX);
    GlobalMemoryStatusEx(&memInfo);
    in.ramMB = memInfo.ullTotalPhys / (1024 * 1024);

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
                    in.gpuAccelAvailable = true;
                    factory->Release();
                }
            }
            FreeLibrary(d2d1);
        }
    }

    // 4. Remote session (RDP / RemoteFX): renderer may be software-only, DWM
    //    effects can black-screen -> force the minimal GDI mode.
    in.remoteSession = GetSystemMetrics(SM_REMOTESESSION) != 0;

    // 5. Third-party hypervisor (VMware / VirtualBox / KVM / Xen / QEMU):
    //    same forced degradation, the guest GPU is usually a virtual device.
    in.virtualMachine = IsThirdPartyVirtualMachine();

    // 6. Composite decision + degradation flags (pure, unit-tested logic)
    g_hwProfile = ComputeHardwareProfile(in);

    // Log the result for diagnostics
    logf("HardwareProfile: lowEnd=%d", (int)g_hwProfile.isLowEnd);
    logf("  cores=%d ram=%lluMB gpu=%d remote=%d vm=%d", in.cpuCores, in.ramMB, (int)in.gpuAccelAvailable,
         (int)in.remoteSession, (int)in.virtualMachine);
    logf("  anim=%d mica=%d", (int)g_hwProfile.disableAnimations, (int)g_hwProfile.disableMica);
    logf("  grad=%d shadows=%d blur=%d", (int)g_hwProfile.disableGradients, (int)g_hwProfile.disableShadows,
         (int)g_hwProfile.disableBlur);
}
