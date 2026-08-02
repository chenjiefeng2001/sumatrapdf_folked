/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// Lifecycle state machine for a MainWindow. The frame WndProc consults the
// current state and defers paint/layout/user-input messages that could act on
// a half-initialized MainWindow during startup or session restore (the startup
// crash tracked as issue #1 in the UI modernization report).
//
// Transitions (see IsValidLifecycleTransition):
//   Uninitialized -> Restoring (session restore) or Ready (normal startup)
//   Restoring     -> Ready (window shown, layout complete) or Destroying
//   Ready         -> Destroying
//   Destroying    -> terminal

#ifndef WINDOWLIFECYCLE_H
#define WINDOWLIFECYCLE_H

enum class WindowLifecycleState {
    Uninitialized,
    Restoring,
    Ready,
    Destroying
};

// True when `to` is a legal successor of `from`. Invalid transitions are
// logged by MainWindow::SetLifecycleState but still applied so the window can
// keep working (the guard only acts on the state, it never blocks it).
inline bool IsValidLifecycleTransition(WindowLifecycleState from, WindowLifecycleState to) {
    switch (from) {
        case WindowLifecycleState::Uninitialized:
            return to == WindowLifecycleState::Restoring || to == WindowLifecycleState::Ready;
        case WindowLifecycleState::Restoring:
            return to == WindowLifecycleState::Ready || to == WindowLifecycleState::Destroying;
        case WindowLifecycleState::Ready:
            return to == WindowLifecycleState::Destroying;
        case WindowLifecycleState::Destroying:
            return false;
    }
    return false;
}

// Frame messages that must not be acted on while the window is not Ready yet:
// the initial layout is done explicitly (RelayoutFrame), painting would target
// an invisible window, and WM_COMMAND/WM_HOTKEY cannot originate from real user
// input before the window is shown. DefWindowProc handles all of them safely.
inline bool IsLifecycleGuardedMessage(unsigned msg) {
    return msg == WM_PAINT || msg == WM_ERASEBKGND || msg == WM_SIZE || msg == WM_MOVE || msg == WM_COMMAND ||
           msg == WM_INITMENUPOPUP || msg == WM_HOTKEY;
}

#endif // WINDOWLIFECYCLE_H
