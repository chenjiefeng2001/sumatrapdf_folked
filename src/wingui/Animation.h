/* Copyright 2024 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

#ifndef SUMATRA_WINGUI_ANIMATION_H
#define SUMATRA_WINGUI_ANIMATION_H

// Lightweight animation framework. Drives property transitions via a per-window
// timer. Typical usage: animate a float from A to B over 200ms with ease-out.
//
// The caller owns the animated value; the framework updates it inline.
// Avoids pulling in a heavy timeline/clock — just a linked list of active
// transitions driven by WM_TIMER.

// Easing function for smooth transitions
enum class Easing {
    Linear,
    EaseOutQuad,   // fast start, slow end (most UI work)
    EaseInOutQuad, // slow start, fast middle, slow end
};

// A single animated property. Embed in the owning struct (e.g. MainWindow).
// The framework manages the linked list; the caller only calls Animate().
struct AnimProp {
    float* target = nullptr; // pointer to the value being animated
    float from = 0;
    float to = 0;
    float current = 0;
    Easing easing = Easing::EaseOutQuad;
    int durationMs = 200;           // animation duration
    int elapsedMs = 0;              // time since start
    bool active = false;
    AnimProp* next = nullptr;       // linked list (internal)

    // Start or restart the animation.
    void Animate(float* val, float toVal, int durMs = 200, Easing ease = Easing::EaseOutQuad);
};

// Animation manager. One per HWND that needs animations.
struct AnimationManager {
    AnimProp* head = nullptr;
    HWND hwnd = nullptr;
    UINT_PTR timerId = 0;
    bool hasActive = false;

    static constexpr UINT_PTR kAnimTimerID = 11;

    explicit AnimationManager(HWND hwnd) : hwnd(hwnd) {}
    ~AnimationManager();

    // Tick all active animations (call from WM_TIMER). Returns the number of
    // still-active animations after the tick.
    int Tick();
};

#endif // SUMATRA_WINGUI_ANIMATION_H

// Interpolate between two ints using eased t in [0,1]
int EaseValue(Easing easing, float t, int from, int to);
