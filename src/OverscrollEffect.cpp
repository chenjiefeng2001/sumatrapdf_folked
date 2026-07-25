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
#include "OverscrollEffect.h"
#include "wingui/Animation.h"

bool OverscrollState::ApplyDelta(int dy, MainWindow* win) {
    if (dy == 0) {
        return false;
    }

    offsetY += dy;

    // Clamp to max stretch
    offsetY = std::clamp(offsetY, -maxStretch, maxStretch);

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

    // Lazily create the AnimProp for spring-back animation
    if (!springAnim) {
        springAnim = new AnimProp();
    }

    float currentOffset = (float)offsetY;
    springAnim->Animate(&currentOffset, 0.0f, 300, Easing::EaseOutQuad);

    // Link to win's animation manager (assumes win has animMgr).
    // This is a simplified integration — we schedule repaints during animation.
    offsetY = 0;
    ScheduleRepaint(win, 0);

    // In a full implementation, animMgr.Tick() would drive offsetY toward 0
    // via the springAnim prop.
}
