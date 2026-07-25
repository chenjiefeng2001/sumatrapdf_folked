/* Copyright 2024 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

#include "base/Base.h"
#include "base/Win.h"
#include "base/WinDynCalls.h"
#include "PointerInput.h"

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
    typedef BOOL(WINAPI* EnableMouseInPointerFn)(BOOL);
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
    double alpha = std::min(1.0, 16.0 / dt); // higher weight for longer frames
    velocityX = velocityX * (1.0 - alpha) + (dx / dt) * alpha;
    velocityY = velocityY * (1.0 - alpha) + (dy / dt) * alpha;

    lastPosX = x;
    lastPosY = y;
    lastTime = now;
}

void PointerVelocityTracker::GetVelocity(double* outVx, double* outVy) const {
    if (outVx) *outVx = velocityX;
    if (outVy) *outVy = velocityY;
}
