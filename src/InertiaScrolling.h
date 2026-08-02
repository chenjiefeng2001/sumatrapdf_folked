/* Copyright 2024 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

// Inertial scrolling with exponential-decay friction. After the user lifts
// their finger / stops scrolling, the content continues moving at a decaying
// velocity until it drops below the threshold, producing a natural feel.

struct MainWindow;

// --- pure math helpers (unit-testable, no Win32) ---

// Nominal frame period of the inertia timer (~60 fps).
static constexpr double kInertiaMsPerTick = 16.0;
// Upper bound for a single axis velocity (pixels per tick), prevents runaway.
static constexpr double kInertiaMaxVelocity = 200.0;
// Default exponential-decay factor applied per tick.
static constexpr double kInertiaDefaultFriction = 0.92;
// Stop once both axes drop below this speed (pixels per tick).
static constexpr double kInertiaStopThreshold = 0.5;

static inline double InertiaClampVelocity(double v, double maxV = kInertiaMaxVelocity) {
    return std::clamp(v, -maxV, maxV);
}

// The velocity tracker produces pixels-per-ms; the inertial scroller works in
// pixels per (nominal) 16ms tick.
static inline double InertiaPxPerMsToPxPerTick(double vPerMs, double msPerTick = kInertiaMsPerTick) {
    return vPerMs * msPerTick;
}

// Advance one tick of exponential decay. Returns false when both axes have
// dropped below threshold, i.e. the caller should stop the inertia.
static inline bool InertiaAdvanceVelocity(double& vx, double& vy, double friction = kInertiaDefaultFriction,
                                          double threshold = kInertiaStopThreshold) {
    vx *= friction;
    vy *= friction;
    return !(fabs(vx) < threshold && fabs(vy) < threshold);
}

struct InertiaScrollState {
    double velocityX = 0; // pixels per tick (16ms)
    double velocityY = 0;
    double friction = 0.92; // multiplied each tick (0.90–0.95 typical)
    double threshold = 0.5; // stop when |v| < threshold
    bool active = false;
    MainWindow* win = nullptr;

    // Launch inertial scrolling with the given initial velocity.
    // Called when the user releases a touch/mouse wheel or pointer pan ends.
    void Start(MainWindow* w, double vx, double vy);

    // Tick — called from a timer (~60 fps). Returns true if still active.
    // Moves the document by the current velocity and applies friction.
    bool Tick();

    // Stop immediately.
    void Stop();
};
