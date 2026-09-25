/* Copyright 2024 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

#include "base/Base.h"
#include "base/Timer.h"
#include "base/Win.h"
#include "gui/UIModels.h"
#include "Settings.h"
#include "DocController.h"
#include "DisplayMode.h"
#include "EngineBase.h"
#include "DisplayModel.h"
#include "MainWindow.h"
#include "HardwareProfile.h"
#include "gui/win/Animation.h"

#include "OverscrollEffect.h"

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
    lastTickUs = TimeGetUs();
    springAnim->active = true;

    // Low-end hardware fast-path: snap back to zero instead of animating.
    if (!AnimationsEnabled()) {
        offsetY = 0;
        springOffset = 0;
        lastTickUs = 0;
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

    i64 now = TimeGetUs();
    double dtMs = lastTickUs > 0 ? (double)(now - lastTickUs) / 1000.0 : 1.0;
    lastTickUs = now;
    dtMs = std::clamp(dtMs, 1.0, 32.0);
    springAnim->elapsedMs += (int)dtMs;
    float t = (float)springAnim->elapsedMs / (float)springAnim->durationMs;
    if (t >= 1.0f) {
        t = 1.0f;
        springOffset = springAnim->to;
        lastTickUs = 0;
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

OverscrollState::~OverscrollState() {
    delete springAnim;
}
