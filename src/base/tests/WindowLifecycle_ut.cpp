/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// Unit tests for the MainWindow lifecycle state machine (WindowLifecycle.h):
//   1. valid / invalid state transitions
//   2. the set of WndProc messages deferred until the window is Ready
//
// All logic is pure so it runs inside test_util.exe without any GUI.

#include "base/Base.h"
#include "WindowLifecycle.h"

// must be last due to assert() over-write
#include "base/UtAssert.h"

static void LifecycleTransitionsTest() {
    // Uninitialized is the start state
    utassert(IsValidLifecycleTransition(WindowLifecycleState::Uninitialized, WindowLifecycleState::Restoring));
    utassert(IsValidLifecycleTransition(WindowLifecycleState::Uninitialized, WindowLifecycleState::Ready));
    utassert(!IsValidLifecycleTransition(WindowLifecycleState::Uninitialized, WindowLifecycleState::Uninitialized));
    utassert(!IsValidLifecycleTransition(WindowLifecycleState::Uninitialized, WindowLifecycleState::Destroying));

    // Restoring can only complete or abort
    utassert(IsValidLifecycleTransition(WindowLifecycleState::Restoring, WindowLifecycleState::Ready));
    utassert(IsValidLifecycleTransition(WindowLifecycleState::Restoring, WindowLifecycleState::Destroying));
    utassert(!IsValidLifecycleTransition(WindowLifecycleState::Restoring, WindowLifecycleState::Restoring));
    utassert(!IsValidLifecycleTransition(WindowLifecycleState::Restoring, WindowLifecycleState::Uninitialized));

    // Ready can only tear down
    utassert(IsValidLifecycleTransition(WindowLifecycleState::Ready, WindowLifecycleState::Destroying));
    utassert(!IsValidLifecycleTransition(WindowLifecycleState::Ready, WindowLifecycleState::Ready));
    utassert(!IsValidLifecycleTransition(WindowLifecycleState::Ready, WindowLifecycleState::Restoring));
    utassert(!IsValidLifecycleTransition(WindowLifecycleState::Ready, WindowLifecycleState::Uninitialized));

    // Destroying is terminal
    utassert(!IsValidLifecycleTransition(WindowLifecycleState::Destroying, WindowLifecycleState::Destroying));
    utassert(!IsValidLifecycleTransition(WindowLifecycleState::Destroying, WindowLifecycleState::Ready));
    utassert(!IsValidLifecycleTransition(WindowLifecycleState::Destroying, WindowLifecycleState::Uninitialized));
}

static void LifecycleGuardedMessagesTest() {
    // paint / layout / user-input messages are deferred while not Ready
    utassert(IsLifecycleGuardedMessage(WM_PAINT));
    utassert(IsLifecycleGuardedMessage(WM_ERASEBKGND));
    utassert(IsLifecycleGuardedMessage(WM_SIZE));
    utassert(IsLifecycleGuardedMessage(WM_MOVE));
    utassert(IsLifecycleGuardedMessage(WM_COMMAND));
    utassert(IsLifecycleGuardedMessage(WM_INITMENUPOPUP));
    utassert(IsLifecycleGuardedMessage(WM_HOTKEY));

    // messages the WndProc must always handle are not guarded
    utassert(!IsLifecycleGuardedMessage(WM_CREATE));
    utassert(!IsLifecycleGuardedMessage(WM_DESTROY));
    utassert(!IsLifecycleGuardedMessage(WM_CLOSE));
    utassert(!IsLifecycleGuardedMessage(WM_NCDESTROY));
    utassert(!IsLifecycleGuardedMessage(WM_GETMINMAXINFO));
    utassert(!IsLifecycleGuardedMessage(WM_NCHITTEST));
    utassert(!IsLifecycleGuardedMessage(WM_DPICHANGED));
    utassert(!IsLifecycleGuardedMessage(WM_SETREDRAW));
}

void WindowLifecycleTest() {
    LifecycleTransitionsTest();
    LifecycleGuardedMessagesTest();
}
