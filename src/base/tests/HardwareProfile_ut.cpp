/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// Unit tests for the low-end degradation decision (HardwareProfile.h):
//   1. core / memory / GPU thresholds
//   2. a remote session (RDP) forces the minimal GDI profile
//   3. a third-party VM forces the minimal GDI profile
//
// Only the pure ComputeHardwareProfile() logic is exercised (no Win32 calls),
// so the tests run inside test_util.exe without a display.

#include "base/Base.h"
#include "HardwareProfile.h"

// must be last due to assert() over-write
#include "base/UtAssert.h"

static void NormalMachineTest() {
    HardwareInputs in;
    in.cpuCores = 8;
    in.ramMB = 16384;
    in.gpuAccelAvailable = true;
    HardwareProfile p = ComputeHardwareProfile(in);
    utassert(!p.isLowEnd);
    utassert(!p.isLowCoreCount);
    utassert(!p.isLowMemory);
    utassert(!p.isLowGpu);
    utassert(!p.disableAnimations);
    utassert(!p.disableMica);
    utassert(!p.disableGradients);
    utassert(!p.disableShadows);
    utassert(!p.disableBlur);
}

static void LowCoreCountTest() {
    HardwareInputs in;
    in.cpuCores = 2;
    in.ramMB = 16384;
    in.gpuAccelAvailable = true;
    HardwareProfile p = ComputeHardwareProfile(in);
    utassert(p.isLowEnd);
    utassert(p.isLowCoreCount);
    utassert(p.disableAnimations);
    utassert(p.disableMica);
    utassert(!p.disableGradients); // GPU is fine, so gradients/shadows stay
    utassert(!p.disableShadows);
    utassert(p.disableBlur);
}

static void LowMemoryTest() {
    HardwareInputs in;
    in.cpuCores = 8;
    in.ramMB = 4096;
    in.gpuAccelAvailable = true;
    HardwareProfile p = ComputeHardwareProfile(in);
    utassert(p.isLowEnd);
    utassert(p.isLowMemory);
    utassert(p.disableAnimations);
    utassert(p.disableMica);
    utassert(!p.disableGradients);
    utassert(p.disableBlur);
}

static void LowGpuTest() {
    HardwareInputs in;
    in.cpuCores = 8;
    in.ramMB = 16384;
    in.gpuAccelAvailable = false; // no D2D hardware
    HardwareProfile p = ComputeHardwareProfile(in);
    utassert(p.isLowEnd);
    utassert(p.isLowGpu);
    utassert(p.disableAnimations);
    utassert(p.disableMica);
    utassert(p.disableGradients); // no GPU => flat colors
    utassert(p.disableShadows);
    utassert(p.disableBlur);
}

static void RemoteSessionTest() {
    // RDP must force the minimal GDI mode even on a beefy machine
    HardwareInputs in;
    in.cpuCores = 16;
    in.ramMB = 32768;
    in.gpuAccelAvailable = true;
    in.remoteSession = true;
    HardwareProfile p = ComputeHardwareProfile(in);
    utassert(p.isLowEnd);
    utassert(!p.isLowCoreCount);
    utassert(!p.isLowMemory);
    utassert(!p.isLowGpu);
    utassert(p.disableAnimations);
    utassert(p.disableMica);
    utassert(p.disableGradients);
    utassert(p.disableShadows);
    utassert(p.disableBlur);
}

static void VirtualMachineTest() {
    // third-party VM forces the minimal GDI mode as well
    HardwareInputs in;
    in.cpuCores = 16;
    in.ramMB = 32768;
    in.gpuAccelAvailable = true;
    in.virtualMachine = true;
    HardwareProfile p = ComputeHardwareProfile(in);
    utassert(p.isLowEnd);
    utassert(p.disableAnimations);
    utassert(p.disableMica);
    utassert(p.disableGradients);
    utassert(p.disableShadows);
    utassert(p.disableBlur);
}

void HardwareProfileTest() {
    NormalMachineTest();
    LowCoreCountTest();
    LowMemoryTest();
    LowGpuTest();
    RemoteSessionTest();
    VirtualMachineTest();
}
