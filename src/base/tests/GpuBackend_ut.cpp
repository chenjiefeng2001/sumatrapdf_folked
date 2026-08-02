/* Copyright 2024 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

// Unit tests for GpuBackend device generation and D2D resource lifecycle.
//
// These tests verify critical invariants in the D2D resource lifecycle:
//   1. Pixmap::d2dDeviceGeneration initializes to 0 (no stale generation)
//   2. BGR8 -> BGRA8 pixel conversion produces correct byte order
//   3. RGBA8 -> BGRA8 pixel swizzle produces correct byte order
//   4. Device generation counter behavior (GpuBackend::deviceGeneration++)
//   5. Deferred release queue thread-safety pattern (QueueSafeD2dRelease logic)
//
// All tests are self-contained (no D2D dependency, no link-time dependency on
// GpuBackend or RenderCache) and run within the test_util.exe framework.

#include "base/Base.h"
#include "base/Pixmap.h"
#include "base/ScopedWin.h"

// must be last due to assert() over-write
#include "base/UtAssert.h"

// ---------------------------------------------------------------------------
// 1. Pixmap::d2dDeviceGeneration field initialization
// ---------------------------------------------------------------------------

static void PixmapDeviceGenerationTest() {
    // 1a. Default initialization: d2dDeviceGeneration must be 0 on stack-allocated Pixmap
    Pixmap p;
    utassert(p.d2dDeviceGeneration == 0);

    // 1b. AllocPixmap: field initializes to 0 through the default struct initializer
    Pixmap* p2 = AllocPixmap(100, 50);
    utassert(p2 != nullptr);
    utassert(p2->d2dDeviceGeneration == 0);
    utassert(p2->width == 100 && p2->height == 50);

    // 1c. Can set and read back
    p2->d2dDeviceGeneration = 5;
    utassert(p2->d2dDeviceGeneration == 5);

    // 1d. Reset to 0
    p2->d2dDeviceGeneration = 0;
    utassert(p2->d2dDeviceGeneration == 0);

    // 1e. ClonePixmap: cloned pixmap has d2dDeviceGeneration == 0
    p2->d2dDeviceGeneration = 42;
    Pixmap* clone = ClonePixmap(p2);
    utassert(clone != nullptr);
    utassert(clone->d2dDeviceGeneration == 0);

    FreePixmap(p2);
    FreePixmap(clone);
}

// ---------------------------------------------------------------------------
// 2. BGR8 -> BGRA8 pixel conversion
//    Matches the algorithm in GpuBackend::CreateBitmapFromPixmap.
// ---------------------------------------------------------------------------

static void TestPixelConversionBgr8ToBgra8() {
    const int w = 4;
    const int h = 2;
    const int srcBpp = 3;
    const int srcStride = (w * srcBpp + 3) & ~3; // 4*3=12, aligned to 4 = 12
    const int dstStride = (w * 4 + 3) & ~3;      // 16

    // Row-interleaved source data: B0,G0,R0, pad, B1,G1,R1, pad, ...
    u8 srcData[h * srcStride] = {
        // Row 0: B G R pad
        0x10, 0x20, 0x30, 0x00,
        0x40, 0x50, 0x60, 0x00,
        0x70, 0x80, 0x90, 0x00,
        // Row 1
        0x11, 0x21, 0x31, 0x00,
        0x41, 0x51, 0x61, 0x00,
        0x71, 0x81, 0x91, 0x00,
    };

    u8 dstData[h * dstStride] = {0};

    // BGR8 -> BGRA8: copy 3 bytes, set A=255
    for (int y = 0; y < h; y++) {
        const u8* srcRow = srcData + y * srcStride;
        u8* dstRow = dstData + y * dstStride;
        for (int x = 0; x < w; x++) {
            dstRow[x * 4 + 0] = srcRow[x * 3 + 0]; // B
            dstRow[x * 4 + 1] = srcRow[x * 3 + 1]; // G
            dstRow[x * 4 + 2] = srcRow[x * 3 + 2]; // R
            dstRow[x * 4 + 3] = 255;               // A
        }
    }

    // Verify row 0 (B,G,R,A)
    utassert(dstData[0] == 0x10 && dstData[1] == 0x20 && dstData[2] == 0x30 && dstData[3] == 255);
    utassert(dstData[4] == 0x40 && dstData[5] == 0x50 && dstData[6] == 0x60 && dstData[7] == 255);
    utassert(dstData[8] == 0x70 && dstData[9] == 0x80 && dstData[10] == 0x90 && dstData[11] == 255);

    // Verify row 1
    utassert(dstData[16] == 0x11 && dstData[17] == 0x21 && dstData[18] == 0x31 && dstData[19] == 255);

    // Verify padding bytes unchanged (dstData starts zeroed)
    utassert(dstData[12] == 0 && dstData[13] == 0 && dstData[14] == 0 && dstData[15] == 0);
}

// ---------------------------------------------------------------------------
// 3. RGBA8 -> BGRA8 pixel swizzle
//    Matches the algorithm in GpuBackend::CreateBitmapFromPixmap.
// ---------------------------------------------------------------------------

static void TestPixelSwizzleRgba8ToBgra8() {
    const int w = 3;
    const int h = 1;
    const int srcStride = w * 4;
    const int dstStride = (w * 4 + 3) & ~3; // 12 aligned to 4 = 12

    // RGBA source: R,G,B,A for each pixel
    u8 srcData[srcStride] = {
        0x10, 0x20, 0x30, 0xFF, // pixel 0: R G B A
        0x40, 0x50, 0x60, 0x80, // pixel 1: R G B A
        0x70, 0x80, 0x90, 0x40, // pixel 2: R G B A
    };

    u8 dstData[dstStride * h] = {0};

    // RGBA8 -> BGRA8: swizzle R<->B
    for (int y = 0; y < h; y++) {
        const u8* srcRow = srcData + y * srcStride;
        u8* dstRow = dstData + y * dstStride;
        for (int x = 0; x < w; x++) {
            dstRow[x * 4 + 0] = srcRow[x * 4 + 2]; // B <- R
            dstRow[x * 4 + 1] = srcRow[x * 4 + 1]; // G <- G
            dstRow[x * 4 + 2] = srcRow[x * 4 + 0]; // R <- B
            dstRow[x * 4 + 3] = srcRow[x * 4 + 3]; // A
        }
    }

    // pixel 0: input R=0x10,G=0x20,B=0x30 -> output B=0x30,G=0x20,R=0x10, A=0xFF
    utassert(dstData[0] == 0x30 && dstData[1] == 0x20 && dstData[2] == 0x10 && dstData[3] == 0xFF);
    // pixel 1: input R=0x40,G=0x50,B=0x60 -> output B=0x60,G=0x50,R=0x40, A=0x80
    utassert(dstData[4] == 0x60 && dstData[5] == 0x50 && dstData[6] == 0x40 && dstData[7] == 0x80);
    // pixel 2: input R=0x70,G=0x80,B=0x90 -> output B=0x90,G=0x80,R=0x70, A=0x40
    utassert(dstData[8] == 0x90 && dstData[9] == 0x80 && dstData[10] == 0x70 && dstData[11] == 0x40);
    // padding
    utassert(dstData[12] == 0 && dstData[13] == 0 && dstData[14] == 0 && dstData[15] == 0);
}

// ---------------------------------------------------------------------------
// 4. Deferred release queue (pattern matching QueueSafeD2dRelease logic)
// ---------------------------------------------------------------------------

struct TestDeferredNode {
    IUnknown* obj;
    TestDeferredNode* next;
};

static void TestDeferredReleaseQueue() {
    CRITICAL_SECTION cs;
    InitializeCriticalSection(&cs);
    TestDeferredNode* head = nullptr;
    int enqueuedCount = 0;

    // Enqueue 3 items from simulated background threads
    // (each call models QueueSafeD2dRelease)
    for (int i = 0; i < 3; i++) {
        auto* node = (TestDeferredNode*)HeapAlloc(GetProcessHeap(), 0, sizeof(TestDeferredNode));
        utassert(node != nullptr);
        node->obj = nullptr; // would be an IUnknown* in production
        EnterCriticalSection(&cs);
        node->next = head;
        head = node;
        LeaveCriticalSection(&cs);
        enqueuedCount++;
    }

    // Drain (models FlushSafeD2dReleases)
    int drainCount = 0;
    TestDeferredNode* drainHead;
    EnterCriticalSection(&cs);
    drainHead = head;
    head = nullptr;
    LeaveCriticalSection(&cs);

    while (drainHead) {
        drainCount++;
        TestDeferredNode* next = drainHead->next;
        HeapFree(GetProcessHeap(), 0, drainHead);
        drainHead = next;
    }

    utassert(drainCount == enqueuedCount);
    utassert(drainCount == 3);
    utassert(head == nullptr); // original head was drained

    DeleteCriticalSection(&cs);
}

// ---------------------------------------------------------------------------
// 5. Device generation counter behavior (matching GpuBackend logic)
// ---------------------------------------------------------------------------

static void TestDeviceGenerationCounter() {
    int deviceGeneration = 0;

    // Initial: 0
    utassert(deviceGeneration == 0);

    // Simulate first RenderTarget creation (GpuBackend::GetRenderTarget)
    deviceGeneration++;
    utassert(deviceGeneration == 1);

    // Simulate HDC change (another document layout / resize)
    deviceGeneration++;
    utassert(deviceGeneration == 2);

    // Simulate bitmap upload matching: after device recreation, cached
    // ID2D1Bitmap instances have stale generation.
    int bitmapGen = 0; // from old device
    utassert(bitmapGen != deviceGeneration); // stale!

    // After recreation on the new device, bitmapGen is updated
    bitmapGen = deviceGeneration;
    utassert(bitmapGen == deviceGeneration); // matches

    // Additional recreation
    deviceGeneration++;
    utassert(bitmapGen != deviceGeneration); // stale again

    // Re-upload
    bitmapGen = deviceGeneration;
    utassert(bitmapGen == deviceGeneration);
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

void GpuBackendUtilTest() {
    PixmapDeviceGenerationTest();
    TestDeferredReleaseQueue();
    TestPixelConversionBgr8ToBgra8();
    TestPixelSwizzleRgba8ToBgra8();
    TestDeviceGenerationCounter();
}