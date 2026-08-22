/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// Unit tests for the interaction-modernization math:
//   1. pointer-velocity EMA estimation (PointerInput.h)
//   2. fine-grained wheel-delta accumulation (PointerInput.h)
//   3. inertial-scrolling decay / clamping / unit conversion (InertiaScrolling.h)
//   4. overscroll stretch clamping (OverscrollEffect.h)
//   5. POINTER_WHEEL_INFO struct layout mirrors the Win32 ABI (PointerInput.h)
//
// Only the pure, inline helpers are exercised — the tests run inside
// test_util.exe without any GUI or window messages.

#include "base/Base.h"
#include "PointerInput.h"
#include "InertiaScrolling.h"
#include "OverscrollEffect.h"

#include <stddef.h> // offsetof

// must be last due to assert() over-write
#include "base/UtAssert.h"

static void PointerEmaAlphaTest() {
    // short frames are capped at weight 1.0 (the sample fully replaces the estimate)
    utassert(PointerEmaAlpha(16.0) == 1.0);
    utassert(PointerEmaAlpha(8.0) == 1.0);
    // exactly 32ms → 0.5
    utassert(PointerEmaAlpha(32.0) == 0.5);
    // long frames trust the sample less (jitter/coasting are common there)
    utassert(fabs(PointerEmaAlpha(1000.0) - 0.016) < 1e-9);
    // non-positive dt is clamped to 1.0 (avoid divide-by-zero later)
    utassert(PointerEmaAlpha(0.0) == 1.0);
    utassert(PointerEmaAlpha(-5.0) == 1.0);
}

static void PointerBlendVelocityTest() {
    // degenerate dt leaves the previous estimate untouched
    double v = 3.0;
    utassert(PointerBlendVelocity(v, 100.0, 0.0) == 3.0);
    utassert(PointerBlendVelocity(v, 100.0, 0.05) == 3.0);

    // first sample from 0: v = (dx/dt) * alpha
    // dt=100ms → alpha = min(1, 16/100) = 0.16
    double v1 = PointerBlendVelocity(0.0, 100.0, 100.0);
    utassert(fabs(v1 - 0.16) < 1e-9);

    // dt <= 16ms → alpha = 1 → sample replaces estimate entirely
    double v2 = PointerBlendVelocity(50.0, 10.0, 10.0);
    utassert(v2 == 1.0); // 10px / 10ms

    // moving opposite to the estimate pulls it back toward the new sample
    double v3 = PointerBlendVelocity(10.0, -10.0, 32.0); // alpha = 0.5
    utassert(fabs(v3 - (10.0 * 0.5 + (-10.0 / 32.0) * 0.5)) < 1e-9);
}

static void VelocityUnitConversionTest() {
    // 1 px/ms == 16 px per 16ms tick
    utassert(PointerPxPerMsToPxPerTick(1.0) == 16.0);
    utassert(PointerPxPerMsToPxPerTick(-1.0) == -16.0);
    utassert(InertiaPxPerMsToPxPerTick(1.0) == 16.0);
    utassert(InertiaPxPerMsToPxPerTick(10.0) == 160.0);
    // both converters must agree (same conversion factor)
    utassert(PointerPxPerMsToPxPerTick(3.5) == InertiaPxPerMsToPxPerTick(3.5));
}

static void WheelDeltaAccumulatorTest() {
    WheelDeltaAccumulator acc;

    // one full notch → exactly one line step
    utassert(acc.AddDelta(WHEEL_DELTA, WHEEL_DELTA) == 1);
    utassert(acc.accum == 0);

    // sub-notch deltas accumulate until a full step is reached
    utassert(acc.AddDelta(30, WHEEL_DELTA) == 0);
    utassert(acc.AddDelta(90, WHEEL_DELTA) == 1);
    utassert(acc.accum == 0);

    // negative direction
    utassert(acc.AddDelta(-30, WHEEL_DELTA) == 0);
    utassert(acc.AddDelta(-90, WHEEL_DELTA) == -1);
    utassert(acc.accum == 0);

    // multiple steps in one message
    utassert(acc.AddDelta(3 * WHEEL_DELTA, WHEEL_DELTA) == 3);

    // leftover stays for the next sample
    utassert(acc.AddDelta(40, WHEEL_DELTA) == 0);
    utassert(acc.accum == 40);

    acc.Reset();
    utassert(acc.accum == 0);
    utassert(acc.AddDelta(WHEEL_DELTA, WHEEL_DELTA) == 1);
}

static void InertiaClampTest() {
    utassert(InertiaClampVelocity(0.0) == 0.0);
    utassert(InertiaClampVelocity(50.0) == 50.0);
    utassert(InertiaClampVelocity(-50.0) == -50.0);
    // runaway values are clamped to the ±200 px/tick bound
    utassert(InertiaClampVelocity(1000.0) == kInertiaMaxVelocity);
    utassert(InertiaClampVelocity(-1000.0) == -kInertiaMaxVelocity);
    // custom bound
    utassert(InertiaClampVelocity(150.0, 100.0) == 100.0);
    utassert(InertiaClampVelocity(-150.0, 100.0) == -100.0);
}

static void InertiaDecayTest() {
    // one tick: v *= friction
    double vx = 100.0, vy = 0.0;
    utassert(InertiaAdvanceVelocity(vx, vy));
    utassert(fabs(vx - 100.0 * kInertiaDefaultFriction) < 1e-9);
    utassert(vy == 0.0);

    // a lone fast axis keeps the inertia alive
    vx = 0.4, vy = 50.0; // |vx| < threshold already
    utassert(InertiaAdvanceVelocity(vx, vy));
    utassert(fabs(vy - 50.0 * 0.92) < 1e-9);

    // exponential decay: each tick multiplies the velocity
    vx = 100.0, vy = 100.0;
    double expected = 100.0;
    for (int i = 0; i < 10; i++) {
        utassert(InertiaAdvanceVelocity(vx, vy));
        expected *= 0.92;
    }
    utassert(fabs(vx - expected) < 1e-9);
    utassert(fabs(vy - expected) < 1e-9);

    // decay terminates: eventually both axes drop below the threshold
    vx = 10.0, vy = 0.0;
    int ticks = 0;
    while (InertiaAdvanceVelocity(vx, vy)) {
        ticks++;
        utassert(ticks < 1000); // must terminate
    }
    utassert(fabs(vx) < kInertiaStopThreshold);
    utassert(fabs(vy) < kInertiaStopThreshold);
    // 10 * 0.92^n >= 0.5 for n <= 35 (n=35 → 0.540, n=36 → 0.497), so the
    // loop body runs exactly 35 times before the 36th call reports "stop"
    utassert(ticks == 35);

    // an already-still state reports "stop"
    vx = 0.0, vy = 0.0;
    utassert(!InertiaAdvanceVelocity(vx, vy));
}

static void OverscrollClampTest() {
    utassert(OverscrollClampOffset(0, 50, 120) == 50);
    utassert(OverscrollClampOffset(0, -50, 120) == -50);

    // clamped at the stretch limit in both directions
    utassert(OverscrollClampOffset(100, 50, 120) == 120);
    utassert(OverscrollClampOffset(-100, -50, 120) == -120);
    // accumulating past the limit still clamps
    utassert(OverscrollClampOffset(120, 50, 120) == 120);
    utassert(OverscrollClampOffset(-120, -50, 120) == -120);

    // bouncing back from a stretch is allowed
    utassert(OverscrollClampOffset(120, -50, 120) == 70);
    utassert(OverscrollClampOffset(-120, 50, 120) == -70);

    // zero delta is identity
    utassert(OverscrollClampOffset(42, 0, 120) == 42);
}

// The mirror structs must match the Win32 POINTER_INFO / POINTER_WHEEL_INFO
// ABI, otherwise GetPointerFrameInfo would scribble over the stack. Layout is
// checked for both the shipped x64 build and the 32-bit CI build: HANDLE /
// HWND are pointer-sized, so every field after sourceDevice shifts.
static void PointerWheelInfoLayoutTest() {
#ifdef _WIN64
    // x64: HANDLE / HWND are 8 bytes
    constexpr int kInfoSize = 96;
    constexpr int kPtPixelOfs = 32;
    constexpr int kPerfCountOfs = 80;
    constexpr int kBtnChangeOfs = 88;
    constexpr int kWheelSize = 112;
    constexpr int kUtDistanceOfs = 96;
    constexpr int kRotationOfs = 100;
    constexpr int kDeltaOfs = 104;
#else
    // x86: HANDLE / HWND are 4 bytes
    constexpr int kInfoSize = 88;
    constexpr int kPtPixelOfs = 24;
    constexpr int kPerfCountOfs = 72;
    constexpr int kBtnChangeOfs = 80;
    constexpr int kWheelSize = 104;
    constexpr int kUtDistanceOfs = 88;
    constexpr int kRotationOfs = 92;
    constexpr int kDeltaOfs = 96;
#endif

    utassert(sizeof(PointerInfoMin) == kInfoSize);
    utassert(offsetof(PointerInfoMin, pointerId) == 4);
    utassert(offsetof(PointerInfoMin, sourceDevice) == 16);
    utassert(offsetof(PointerInfoMin, ptPixelLocation) == kPtPixelOfs);
    utassert(offsetof(PointerInfoMin, performanceCount) == kPerfCountOfs);
    utassert(offsetof(PointerInfoMin, buttonChangeType) == kBtnChangeOfs);

    utassert(sizeof(PointerWheelInfoMin) == kWheelSize);
    utassert(offsetof(PointerWheelInfoMin, utDistance) == kUtDistanceOfs);
    utassert(offsetof(PointerWheelInfoMin, rotation) == kRotationOfs);
    utassert(offsetof(PointerWheelInfoMin, delta) == kDeltaOfs);

    // utDistance is the field we read for the wheel delta
    PointerWheelInfoMin w = {};
    w.utDistance = 30;
    utassert((INT32)w.utDistance == 30);
}

void InputScrollingTest() {
    PointerEmaAlphaTest();
    PointerBlendVelocityTest();
    VelocityUnitConversionTest();
    WheelDeltaAccumulatorTest();
    InertiaClampTest();
    InertiaDecayTest();
    OverscrollClampTest();
    PointerWheelInfoLayoutTest();
}
