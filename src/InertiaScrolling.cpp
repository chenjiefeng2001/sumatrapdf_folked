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
#include "OverscrollEffect.h"
#include "InertiaScrolling.h"

void InertiaScrollState::Start(MainWindow* w, double vx, double vy) {
    velocityX = InertiaClampVelocity(vx);
    velocityY = InertiaClampVelocity(vy);
    friction = 0.92;
    threshold = 0.5;
    lastTickUs = TimeGetUs();
    active = true;
    win = w;
}

bool InertiaScrollState::Tick() {
    if (!active || !win) {
        return false;
    }

    i64 now = TimeGetUs();
    double dtMs = lastTickUs > 0 ? (double)(now - lastTickUs) / 1000.0 : kInertiaMsPerTick;
    lastTickUs = now;
    dtMs = std::clamp(dtMs, 1.0, 32.0);
    double elapsedTicks = dtMs / kInertiaMsPerTick;
    double elapsedFriction = pow(friction, elapsedTicks);

    // Apply friction and check the stop threshold
    bool stillMoving = InertiaAdvanceVelocity(velocityX, velocityY, elapsedFriction, threshold);
    if (!stillMoving) {
        Stop();
        return false;
    }

    int dx = (int)round(velocityX * elapsedTicks);
    int dy = (int)round(velocityY * elapsedTicks);

    // Clamp movement to the document boundaries so the doc doesn't get shoved
    // against the edge on every tick. The clipped leftover is handed to the
    // overscroll effect (elastic stretch).
    int overscrollDy = 0;
    if (dx != 0 || dy != 0) {
        DisplayModel* dm = win->AsFixed();
        if (dm) {
            if (dx > 0 && !dm->CanScrollRight()) {
                dx = 0;
            }
            if (dx < 0 && !dm->CanScrollLeft()) {
                dx = 0;
            }
            if (dy != 0) {
                Rect vp = dm->GetViewPort();
                Size cs = dm->GetCanvasSize();
                if (dy < 0 && vp.y <= 0) {
                    overscrollDy = dy;
                    dy = 0;
                }
                if (dy > 0 && vp.y + vp.dy >= cs.dy) {
                    overscrollDy = dy;
                    dy = 0;
                }
            }
        }
        if (dx != 0 || dy != 0) {
            win->MoveDocBy(dx, dy);
        }
    }

    // We hit a boundary while still moving fast: transfer the momentum to the
    // overscroll effect and stop the inertia (a per-tick timer would otherwise
    // keep running until the velocity decays below the threshold).
    if (overscrollDy != 0) {
        if (win->overscroll) {
            win->overscroll->ApplyDelta(overscrollDy, win);
            win->overscroll->Release(win);
        }
        Stop();
        return false;
    }

    return true;
}

void InertiaScrollState::Stop() {
    velocityX = 0;
    velocityY = 0;
    lastTickUs = 0;
    active = false;
}
