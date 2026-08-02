/* Copyright 2024 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

// WM_POINTER high-precision input handling. Supports pixel-precise scrolling
// from precision touchpads, touch, and pen input. Falls back to legacy
// WM_MOUSEWHEEL when WM_POINTER is unavailable (Win7, Wine).

// Call once at app startup to enable WM_POINTER on Win8+.
void EnablePointerInput();

// Returns true if WM_POINTER is available and enabled.
bool IsPointerInputEnabled();

// --- pure math helpers (unit-testable, no Win32) ---

// EMA weight for one velocity sample of dtMs milliseconds. Longer frames get
// a higher weight (up to 1.0) so the estimate converges quickly.
static inline double PointerEmaAlpha(double dtMs) {
    if (dtMs <= 0) {
        return 1.0;
    }
    return std::min(1.0, 16.0 / dtMs);
}

// Blended velocity for one axis: prevV * (1-a) + (dx/dt) * a.
static inline double PointerBlendVelocity(double prevV, double dx, double dtMs) {
    if (dtMs <= 0.1) {
        return prevV;
    }
    double a = PointerEmaAlpha(dtMs);
    return prevV * (1.0 - a) + (dx / dtMs) * a;
}

// The inertial scroller works in "pixels per 16ms tick", the tracker in
// "pixels per ms" — convert between the two.
static inline double PointerPxPerMsToPxPerTick(double vPerMs, double msPerTick = 16.0) {
    return vPerMs * msPerTick;
}

// Accumulates fine-grained wheel deltas (precision touchpads can report a
// fraction of WHEEL_DELTA per message) and reports how many full line-scroll
// steps are owed. Mirrors the accumulation logic in Canvas::CanvasOnMouseWheel.
struct WheelDeltaAccumulator {
    int accum = 0;

    // Feed a signed wheel delta (WHEEL_DELTA = 120 per notch). Returns the
    // number of line-scroll steps owed (positive = down/right, negative =
    // up/left); the leftover sub-step amount is retained for the next sample.
    int AddDelta(int delta, int lineStep) {
        accum += delta;
        int steps = accum / lineStep;
        accum -= steps * lineStep;
        return steps;
    }

    void Reset() { accum = 0; }
};

// Minimal mirror of the winuser.h POINTER_INFO / POINTER_WHEEL_INFO structs.
// The Windows SDK shipped with this tree predates them, so we define our own
// (the layout is verified against the ABI in the unit test).
#pragma pack(push, 8)
struct PointerInfoMin {
    DWORD pointerType;
    UINT32 pointerId;
    UINT32 frameId;
    DWORD pointerFlags;
    HANDLE sourceDevice;
    HWND hwndTarget;
    POINT ptPixelLocation;
    POINT ptHimetricLocation;
    POINT ptPixelLocationRaw;
    POINT ptHimetricLocationRaw;
    DWORD dwTime;
    UINT32 historyCount;
    INT32 inputData;
    DWORD dwKeyStates;
    UINT64 performanceCount;
    DWORD buttonChangeType;
};
struct PointerWheelInfoMin {
    PointerInfoMin pointerInfo;
    UINT32 utDistance; // scrolling distance, WHEEL_DELTA = 120 per notch
    UINT32 rotation;   // wheel rotation angle (0.1 degree units)
    INT32 delta;       // delta (deprecated, prefer utDistance)
};
#pragma pack(pop)

// Extracts the signed wheel delta from a WM_POINTERWHEEL message's pointer id.
// The API is resolved dynamically so this also works on Win8/8.1. Returns false
// when GetPointerFrameInfo isn't available (caller should let DefWindowProc
// promote the message to WM_MOUSEWHEEL).
bool GetPointerWheelDelta(UINT32 pointerId, INT32* deltaOut);

// State for tracking velocity during pointer wheel or pan gestures.
// Used by the inertial scrolling system to produce smooth deceleration.
struct PointerVelocityTracker {
    double velocityX = 0; // current horizontal velocity (pixels / ms)
    double velocityY = 0; // current vertical velocity
    double lastPosX = 0;  // last pointer position (sub-pixel)
    double lastPosY = 0;
    LARGE_INTEGER lastTime{}; // timestamp of last sample

    // Initialize with current timestamp
    void Init();

    // Feed a new sample. dt = elapsed ms since last sample; dx/dy = pixel delta.
    void AddSample(double x, double y, LARGE_INTEGER now);

    // Compute instantaneous velocity from latest sample.
    void GetVelocity(double* outVx, double* outVy) const;
};
