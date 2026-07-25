/* Copyright 2024 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

// WM_POINTER high-precision input handling. Supports pixel-precise scrolling
// from precision touchpads, touch, and pen input. Falls back to legacy
// WM_MOUSEWHEEL when WM_POINTER is unavailable (Win7, Wine).

// Call once at app startup to enable WM_POINTER on Win8+.
void EnablePointerInput();

// Returns true if WM_POINTER is available and enabled.
bool IsPointerInputEnabled();

// State for tracking velocity during pointer wheel or pan gestures.
// Used by the inertial scrolling system to produce smooth deceleration.
struct PointerVelocityTracker {
    double velocityX = 0;      // current horizontal velocity (pixels / ms)
    double velocityY = 0;      // current vertical velocity
    double lastPosX = 0;       // last pointer position (sub-pixel)
    double lastPosY = 0;
    LARGE_INTEGER lastTime{};  // timestamp of last sample

    // Initialize with current timestamp
    void Init();

    // Feed a new sample. dt = elapsed ms since last sample; dx/dy = pixel delta.
    void AddSample(double x, double y, LARGE_INTEGER now);

    // Compute instantaneous velocity from latest sample.
    void GetVelocity(double* outVx, double* outVy) const;
};
