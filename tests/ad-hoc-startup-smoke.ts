// Headless startup smoke test for the MainWindow lifecycle guard
// (WindowLifecycle.h). Guards drop WM_PAINT/WM_SIZE/... before the window is
// Ready, so this test verifies the window still appears, is visible, has a
// canvas, and can be closed cleanly.
//
// GUI automation is limited on this machine (SendInput dropped, window
// messages work), which is fine: this test only needs the frame to appear.
// Run with: bun tests/ad-hoc-startup-smoke.ts

import { FRAME_CLASS, CANVAS_CLASS, launchSumatra, waitForFrame, findCanvas } from "./win-automation.ts";
import { isWindowVisible, getWindowRect, getWindowText, postMessage, WM_CLOSE, sleep } from "./winapi.ts";
import { runStandalone } from "./util.ts";

export async function testit(): Promise<void> {
  const proc = launchSumatra([]);

  const frame = await waitForFrame(proc.pid, 15000);
  if (!frame) {
    proc.kill();
    throw new Error("frame window did not appear (lifecycle guard blocked startup?)");
  }

  // the frame exists as soon as CreateWindowEx returns, but the lifecycle guard
  // (and ShowMainWindow) runs a moment later — poll until it's actually visible
  const deadline = Date.now() + 10000;
  while (Date.now() < deadline && !isWindowVisible(frame)) {
    await sleep(120);
  }
  if (!isWindowVisible(frame)) {
    proc.kill();
    throw new Error("frame window never became visible");
  }

  const r = getWindowRect(frame);
  if (r.dx < 100 || r.dy < 100) {
    proc.kill();
    throw new Error(`frame window too small: ${r.dx}x${r.dy}`);
  }

  const title = getWindowText(frame);
  if (!title.includes("SumatraPDF")) {
    proc.kill();
    throw new Error(`unexpected frame title: '${title}'`);
  }

  if (!findCanvas(frame)) {
    proc.kill();
    throw new Error("canvas child window missing");
  }

  // close via WM_CLOSE (goes through the lifecycle guard's Destroying state)
  postMessage(frame, WM_CLOSE, 0, 0);
  const exited = await Promise.race([
    proc.exited.then(() => true),
    new Promise<boolean>((resolve) => setTimeout(() => resolve(false), 10000)),
  ]);
  if (!exited) {
    proc.kill();
    throw new Error("app did not exit after WM_CLOSE");
  }
}

if (import.meta.main) {
  await runStandalone(testit);
}
