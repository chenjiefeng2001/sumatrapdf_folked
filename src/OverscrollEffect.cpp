/* Copyright 2024 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

#include "base/Base.h"
#include "base/Win.h"
#include "gui/UIModels.h"
#include "Settings.h"
#include "DocController.h"
#include "DisplayMode.h"
#include "EngineBase.h"
#include "DisplayModel.h"
#include "MainWindow.h"
#include "HardwareProfile.h"
#include "OverscrollEffect.h"
#include "gui/win/Animation.h"

bool OverscrollState::ApplyDelta(int dy, MainWindow* win) {
    if (dy == 0) {
        return false;
    }

    offsetY = OverscrollClampOffset(offsetY, dy, maxStretch);

    // Schedule repaint to show the overscroll visual
    if (win) {
        ScheduleRepaint(win, 0);
    }

    return true;
}

void OverscrollState::Release(MainWindow* win) {
    if (offsetY == 0 || !win) {
        return;
    }

    // Lazily create the AnimProp for the spring-back animation
    if (!springAnim) {
        springAnim = new AnimProp();
    }

    springOffset = (float)offsetY;
    springAnim->from = springOffset;
    springAnim->to = 0.0f;
    springAnim->current = springOffset;
    springAnim->durationMs = 300;
    springAnim->easing = Easing::EaseOutQuad;
    springAnim->elapsedMs = 0;
    springAnim->active = true;

    // Low-end hardware fast-path: snap back to zero instead of animating.
    if (!AnimationsEnabled()) {
        offsetY = 0;
        springOffset = 0;
        springAnim->active = false;
        ScheduleRepaint(win, 0);
        return;
    }

    // Drive the animation from a timer (ticked in the canvas WndProc).
    SetTimer(win->hwndCanvas, kOverscrollTimerID, USER_TIMER_MINIMUM, nullptr);
    ScheduleRepaint(win, 0);
}

bool OverscrollState::TickSpring(MainWindow* win) {
    if (!springAnim || !springAnim->active) {
        return false;
    }

    springAnim->elapsedMs += 16; // assume ~60 fps timer tick
    float t = (float)springAnim->elapsedMs / (float)springAnim->durationMs;
    if (t >= 1.0f) {
        t = 1.0f;
        springOffset = springAnim->to;
        springAnim->active = false;
    } else {
        // EaseOutQuad (matches wingui/Animation.cpp)
        float eased = t * (2.0f - t);
        springOffset = springAnim->from + (springAnim->to - springAnim->from) * eased;
    }
    offsetY = (int)springOffset;

    if (win) {
        ScheduleRepaint(win, 0);
    }
    return springAnim->active;
}
