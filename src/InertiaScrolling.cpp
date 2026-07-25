/* Copyright 2024 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

#include "base/Base.h"
#include "base/Win.h"
#include "wingui/UIModels.h"
#include "Settings.h"
#include "DocController.h"
#include "DisplayMode.h"
#include "EngineBase.h"
#include "DisplayModel.h"
#include "MainWindow.h"
#include "InertiaScrolling.h"

void InertiaScrollState::Start(MainWindow* w, double vx, double vy) {
    velocityX = vx;
    velocityY = vy;
    friction = 0.92;
    threshold = 0.5;
    active = true;
    win = w;

    // Clamp to sane maximum to avoid runaway
    double maxVel = 200.0;
    velocityX = std::clamp(velocityX, -maxVel, maxVel);
    velocityY = std::clamp(velocityY, -maxVel, maxVel);
}

bool InertiaScrollState::Tick() {
    if (!active || !win) {
        return false;
    }

    // Apply friction
    velocityX *= friction;
    velocityY *= friction;

    // Check threshold
    if (fabs(velocityX) < threshold && fabs(velocityY) < threshold) {
        Stop();
        return false;
    }

    // Apply movement (velocity is in pixels per 16ms tick)
    int dx = (int)round(velocityX);
    int dy = (int)round(velocityY);

    if (dx != 0 || dy != 0) {
        win->MoveDocBy(dx, dy);
    }

    return true;
}

void InertiaScrollState::Stop() {
    velocityX = 0;
    velocityY = 0;
    active = false;
}
