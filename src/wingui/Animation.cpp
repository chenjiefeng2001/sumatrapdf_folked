/* Copyright 2024 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

#include "base/Base.h"
#include "base/Win.h"
#include "wingui/Animation.h"

static float ApplyEasing(Easing easing, float t) {
    switch (easing) {
        case Easing::Linear:
            return t;
        case Easing::EaseOutQuad:
            return t * (2.0f - t);
        case Easing::EaseInOutQuad:
            return t < 0.5f ? 2.0f * t * t : -1.0f + (4.0f - 2.0f * t) * t;
    }
    return t;
}

int EaseValue(Easing easing, float t, int from, int to) {
    float eased = ApplyEasing(easing, t);
    return (int)(from + (to - from) * eased);
}

void AnimProp::Animate(float* val, float toVal, int durMs, Easing ease) {
    from = current = *val;
    to = toVal;
    durationMs = durMs;
    easing = ease;
    elapsedMs = 0;
    active = true;
    target = val;
}

AnimationManager::~AnimationManager() {
    if (timerId && hwnd) {
        KillTimer(hwnd, timerId);
    }
    // No need to free AnimProp entries — they are embedded in their owners.
    head = nullptr;
}

int AnimationManager::Tick() {
    hasActive = false;
    int count = 0;
    AnimProp* prop = head;
    while (prop) {
        if (prop->active) {
            prop->elapsedMs += 16; // assume ~60 fps timer tick
            float t = (float)prop->elapsedMs / (float)prop->durationMs;
            if (t >= 1.0f) {
                t = 1.0f;
                prop->current = prop->to;
                prop->active = false;
            } else {
                prop->current = prop->from + (prop->to - prop->from) * ApplyEasing(prop->easing, t);
            }
            if (prop->target) {
                *prop->target = prop->current;
            }
            count++;
        }
        prop = prop->next;
    }
    hasActive = count > 0;
    // If nothing is active, kill the timer
    if (!hasActive && timerId && hwnd) {
        KillTimer(hwnd, timerId);
        timerId = 0;
    } else if (hasActive && !timerId && hwnd) {
        timerId = SetTimer(hwnd, kAnimTimerID, 16, nullptr); // ~60 fps
    }
    return count;
}
