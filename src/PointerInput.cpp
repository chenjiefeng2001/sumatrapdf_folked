/* Copyright 2024 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

#include "base/Base.h"
#include "base/Win.h"
#include "base/WinDynCalls.h"
#include "PointerInput.h"

// Pointer input type values (from winuser.h). The SDK shipped with this tree
// predates POINTER_INPUT_TYPE, so define the ones we need ourselves.
#define SUMATRA_PT_TOUCH 2
#define SUMATRA_PT_PEN 3
#define SUMATRA_PT_MOUSE 4
#define SUMATRA_PT_TOUCHPAD 5

typedef BOOL(WINAPI* Sig_GetPointerFrameInfo)(UINT32 pointerId, UINT32* pointerCount, PointerInfoMin* pointerInfo);

static bool gPointerInputEnabled = false;
static bool gPointerInputChecked = false;

void EnablePointerInput() {
    if (gPointerInputChecked) {
        return;
    }
    gPointerInputChecked = true;

    // WM_POINTER is available on Windows 8 and later.
    // EnableMouseInPointer(true) makes the system deliver WM_POINTER* messages
    // instead of WM_MOUSEMOVE/WM_MOUSEWHEEL etc.
    OSVERSIONINFOEX ver = {};
    if (!GetOsVersion(ver)) {
        return;
    }
    // Build number >= 9200 = Win8
    if (ver.dwBuildNumber < 9200) {
        return;
    }

    // Dynamically load EnableMouseInPointer from user32.dll
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32) {
        return;
    }
    typedef BOOL(WINAPI * EnableMouseInPointerFn)(BOOL);
    auto fnEnableMouseInPointer = (EnableMouseInPointerFn)GetProcAddress(user32, "EnableMouseInPointer");
    if (!fnEnableMouseInPointer) {
        return;
    }

    BOOL ok = fnEnableMouseInPointer(TRUE);
    gPointerInputEnabled = (ok == TRUE);
}

bool IsPointerInputEnabled() {
    return gPointerInputEnabled;
}

static Sig_GetPointerFrameInfo gFnGetPointerFrameInfo = nullptr;
static bool gPointerFrameInfoChecked = false;

static void EnsurePointerFrameInfoLoaded() {
    if (gPointerFrameInfoChecked) {
        return;
    }
    gPointerFrameInfoChecked = true;
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        gFnGetPointerFrameInfo = (Sig_GetPointerFrameInfo)GetProcAddress(user32, "GetPointerFrameInfo");
    }
}

bool GetPointerWheelDelta(UINT32 pointerId, INT32* deltaOut) {
    if (!deltaOut) {
        return false;
    }
    EnsurePointerFrameInfoLoaded();
    if (!gFnGetPointerFrameInfo) {
        return false;
    }

    PointerWheelInfoMin wheelInfo = {};
    UINT32 pointerCount = 1;
    if (!gFnGetPointerFrameInfo(pointerId, &pointerCount, &wheelInfo.pointerInfo)) {
        return false;
    }
    // WM_POINTERWHEEL comes from the mouse itself (PT_MOUSE) or a precision
    // touchpad (PT_TOUCHPAD, Win10 1703+)
    if (pointerCount < 1) {
        return false;
    }
    if (wheelInfo.pointerInfo.pointerType != SUMATRA_PT_MOUSE &&
        wheelInfo.pointerInfo.pointerType != SUMATRA_PT_TOUCHPAD) {
        return false;
    }
    *deltaOut = (INT32)wheelInfo.utDistance;
    return true;
}

void PointerVelocityTracker::Init() {
    velocityX = 0;
    velocityY = 0;
    lastPosX = 0;
    lastPosY = 0;
    QueryPerformanceCounter(&lastTime);
}

void PointerVelocityTracker::AddSample(double x, double y, LARGE_INTEGER now) {
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);

    double dt = (double)(now.QuadPart - lastTime.QuadPart) / (double)freq.QuadPart;
    // Convert dt from seconds to milliseconds
    dt *= 1000.0;

    if (dt < 0.1) {
        // Ignore samples that are too close (avoids divide-by-zero and jitter).
        return;
    }

    if (lastTime.QuadPart == 0) {
        // First sample — just store position
        lastPosX = x;
        lastPosY = y;
        lastTime = now;
        return;
    }

    double dx = x - lastPosX;
    double dy = y - lastPosY;

    // Exponential moving average for velocity smoothing
    velocityX = PointerBlendVelocity(velocityX, dx, dt);
    velocityY = PointerBlendVelocity(velocityY, dy, dt);

    lastPosX = x;
    lastPosY = y;
    lastTime = now;
}

void PointerVelocityTracker::GetVelocity(double* outVx, double* outVy) const {
    if (outVx) *outVx = velocityX;
    if (outVy) *outVy = velocityY;
}
