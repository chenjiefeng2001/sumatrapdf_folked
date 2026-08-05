// Ad-hoc integration test for the elastic notification layout
// (NotificationWnd::Layout with keepWidth=false on parent resize):
//
//   1. launch the real app, open a PDF and trigger the zoom notification
//      (CmdZoomActualSize). It's the ideal probe: a plain notification whose
//      text is NOT re-populated by ZoomChanged during a resize (unlike the
//      page-info overlay), and zoom is fixed at 100% so resizing is inert.
//   2. replace its message with a long sentence via WM_SETTEXT
//   3. resize once so RelayoutNotifications re-lays it out with the long text
//   4. shrink the window  -> the notification must shrink to the canvas width
//      (and get taller as the long text wraps)
//   5. widen the window again -> the notification must re-expand
//
// Run directly: bun tests/ad-hoc-notif-elastic.ts
// Also registered in tests/before-release.ts (needs a GUI desktop session).

import { join } from "node:path";
import { ROOT, cmdId, runStandalone } from "./util.ts";
import { launchSumatra, waitForFrame, findCanvas, sendCommand } from "./win-automation.ts";
import * as win from "./winapi.ts";

const kNotifClass = "SumatraWgDefaultWinClass";

function findNotification(canvas: number): number {
    let found = 0;
    win.enumChildWindows(canvas, (hwnd) => {
        if (found) {
            return false;
        }
        if (!win.isWindowVisible(hwnd)) {
            return true;
        }
        if (win.getClassName(hwnd) === kNotifClass) {
            found = hwnd;
            return false;
        }
        return true;
    });
    return found;
}

async function waitForNotification(canvas: number, timeoutMs = 5000): Promise<number> {
    const deadline = Date.now() + timeoutMs;
    while (Date.now() < deadline) {
        const hwnd = findNotification(canvas);
        if (hwnd) {
            return hwnd;
        }
        await win.sleep(100);
    }
    return 0;
}

function width(r: win.Rect): number {
    return r.right - r.left;
}

function height(r: win.Rect): number {
    return r.bottom - r.top;
}

export async function testit(): Promise<void> {
    const pdf = join(ROOT, "tests", "issue-5404.pdf");
    const proc = launchSumatra([pdf]);
    try {
        const frame = await waitForFrame(proc.pid);
        if (!frame) {
            throw new Error("main frame window not found");
        }
        const canvas = findCanvas(frame);
        if (!canvas) {
            throw new Error("canvas window not found");
        }

        // The zoom notification is the ideal elastic-layout probe: it is a plain
        // notification whose text is NOT overwritten by resize (unlike the
        // page-info overlay, which ZoomChanged re-populates). Its 2s auto-hide
        // is plenty: WM_SIZE/RelayoutNotifications run synchronously inside
        // MoveWindow, so 3 quick resizes fit comfortably within the timeout.
        // Zooming to 100% also fixes the zoom so resize doesn't change it.
        sendCommand(frame, cmdId("CmdZoomActualSize"));
        const notif = await waitForNotification(canvas);
        if (!notif) {
            throw new Error("notification window not found");
        }
        console.log(`notif hwnd: 0x${notif.toString(16)}`);
        await win.sleep(250); // let the slide-in finish

        // replace the message with a long sentence so re-wrapping is meaningful
        win.sendText(notif, "This is a deliberately long notification message about file loading progress that wraps to multiple lines when the window gets narrower");
        await win.sleep(300);
        const notifText = win.getWindowText(notif);
        console.log(`notif text after WM_SETTEXT: "${notifText}"`);

        const fr = win.getWindowRect(frame);

        // 0) resize once so the notification adopts the long message (WM_SETTEXT
        // alone doesn't trigger a re-layout, RelayoutNotifications does)
        win.moveWindow(frame, fr.left, fr.top, 1100, height(fr));
        await win.sleep(250);
        const r0 = win.getWindowRect(notif);
        console.log(`wide baseline: notif ${width(r0)}px x ${height(r0)}px`);
        if (width(r0) <= 200) {
            throw new Error(`long message not adopted (notification ${width(r0)}px)`);
        }

        // 1) shrink the window: notification must not exceed the canvas width
        // and should get narrower (the long text re-wraps)
        win.moveWindow(frame, fr.left, fr.top, 320, height(fr));
        await win.sleep(250);
        const r2 = win.getWindowRect(notif);
        const fr2 = win.getWindowRect(frame);
        const client2 = win.getClientRect(frame);
        console.log(`narrowed: notif ${width(r2)}px x ${height(r2)}px, frame ${width(fr2)}px, canvas ${client2.right}px`);
        if (width(r2) > client2.right) {
            throw new Error(`notification (${width(r2)}px) wider than canvas (${client2.right}px) after shrinking`);
        }
        if (width(r2) >= width(r0)) {
            throw new Error(`notification did not shrink (${width(r2)}px >= ${width(r0)}px) after the window got narrower`);
        }

        // 2) widen again: notification must re-expand (elastic width both ways)
        win.moveWindow(frame, fr.left, fr.top, 1100, height(fr));
        await win.sleep(250);
        const r3 = win.getWindowRect(notif);
        console.log(`widened: notif ${width(r3)}px x ${height(r3)}px`);
        if (width(r3) <= width(r2)) {
            throw new Error(`notification did not re-expand (${width(r3)}px <= ${width(r2)}px) after the window got wider`);
        }
        console.log("elastic notification width OK (shrinks to fit, re-expands when wider)");
    } finally {
        proc.kill();
    }
}

if (import.meta.main) {
    await runStandalone(testit);
}
