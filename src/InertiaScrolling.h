/* Copyright 2024 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

// Inertial scrolling with exponential-decay friction. After the user lifts
// their finger / stops scrolling, the content continues moving at a decaying
// velocity until it drops below the threshold, producing a natural feel.

struct MainWindow;

struct InertiaScrollState {
    double velocityX = 0;     // pixels per tick (16ms)
    double velocityY = 0;
    double friction = 0.92;   // multiplied each tick (0.90–0.95 typical)
    double threshold = 0.5;   // stop when |v| < threshold
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
