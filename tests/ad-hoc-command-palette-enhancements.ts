// Ad-hoc test for the Command Palette enhancements:
//   - live result count ("N results") next to the navigation hints
//   - a "×" clear button next to the query box (visible only when non-empty,
//     click empties the query and restores the full list)
//   - category glyphs on group headers (rendered; verified indirectly that the
//     palette survives typing/selection without crashing)
//
// This is GUI automation (no -dbg-control hook for the palette), so it lives
// as an ad-hoc test, not in tests/all.ts. Run directly:
//   bun tests/ad-hoc-command-palette-enhancements.ts [--no-build]
//
// Reverting the enhancements (removing UpdateResultCount/ClearQuery wiring in
// CommandPalette.cpp / CommandPaletteFilter.cpp) makes this throw.

import { writeFileSync } from "node:fs";
import { runStandalone, tmpPath } from "./util.ts";
import { launchSumatra, waitForFrame, sendCommand, pressEscape, clickAt } from "./win-automation.ts";
import {
  sleep,
  moveWindow,
  enumWindows,
  enumChildWindows,
  getWindowPid,
  getClassName,
  findChildWindow,
  getWindowText,
  sendChars,
  isWindowVisible,
  isWindow,
} from "./winapi.ts";

const CmdCommandPalette = 370;

function makeMinimalPdf(): Buffer {
  const enc = (s: string) => Buffer.from(s, "latin1");
  const body: Record<number, Buffer> = {};
  body[1] = enc("<< /Type /Catalog /Pages 2 0 R >>");
  body[2] = enc("<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
  body[3] = enc("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] >>");
  const maxN = 3;
  const parts: Buffer[] = [enc("%PDF-1.7\n%\xe2\xe3\xcf\xd3\n")];
  const offsets: Record<number, number> = {};
  let pos = parts[0].length;
  for (let n = 1; n <= maxN; n++) {
    offsets[n] = pos;
    const obj = Buffer.concat([enc(`${n} 0 obj\n`), body[n], enc("\nendobj\n")]);
    parts.push(obj);
    pos += obj.length;
  }
  let xref = `xref\n0 ${maxN + 1}\n0000000000 65535 f \n`;
  for (let n = 1; n <= maxN; n++) {
    xref += `${String(offsets[n]).padStart(10, "0")} 00000 n \n`;
  }
  parts.push(enc(`${xref}trailer\n<< /Size ${maxN + 1} /Root 1 0 R >>\nstartxref\n${pos}\n%%EOF\n`));
  return Buffer.concat(parts);
}

function topWindows(pid: number): number[] {
  const out: number[] = [];
  enumWindows((h) => {
    if (getWindowPid(h) === pid) {
      out.push(h);
    }
    return true;
  });
  return out;
}

function findPalette(pid: number, before: Set<number>): number {
  return topWindows(pid).find((h) => !before.has(h) && findChildWindow(h, "Edit")) ?? 0;
}

// first child Static whose window text contains `needle`
function findStaticWithText(parent: number, needle: string): number {
  let found = 0;
  enumChildWindows(parent, (h) => {
    if (getClassName(h) === "Static" && getWindowText(h).includes(needle)) {
      found = h;
      return false;
    }
    return true;
  });
  return found;
}

function parseResults(text: string): number {
  const m = /(\d+)\s*results/.exec(text);
  if (!m) {
    return -1;
  }
  return parseInt(m[1], 10);
}

export async function testit(): Promise<void> {
  const pdfPath = tmpPath("cp-enhancements.pdf");
  writeFileSync(pdfPath, makeMinimalPdf());

  const proc = launchSumatra([pdfPath]);
  try {
    const frame = await waitForFrame(proc.pid);
    if (!frame) {
      throw new Error("SumatraPDF frame window didn't appear");
    }
    moveWindow(frame, 60, 20, 1100, 780);
    await sleep(1200);

    // --- open the Command Palette (">" commands mode) ---
    const before = new Set(topWindows(proc.pid));
    sendCommand(frame, CmdCommandPalette);
    await sleep(900);
    const palette = findPalette(proc.pid, before);
    if (!palette) {
      throw new Error("Command Palette window didn't open");
    }
    const edit = findChildWindow(palette, "Edit");
    if (!edit) {
      throw new Error("palette edit box not found");
    }
    // CmdCommandPalette (Ctrl+K) opens the palette with an empty query, which
    // means default commands mode (all commands shown); an explicit ">" prefix
    // is only needed after typing.
    if (getWindowText(edit) !== "") {
      throw new Error(`unexpected initial query: "${getWindowText(edit)}"`);
    }

    // --- enhancement 1: live result count ---
    const info = findStaticWithText(palette, "results");
    if (!info) {
      throw new Error("result-count static not found");
    }
    const initialCount = parseResults(getWindowText(info));
    if (initialCount <= 0) {
      throw new Error(`unexpected initial result count: ${initialCount}`);
    }

    // --- enhancement 2: clear button ---
    const clearBtn = findStaticWithText(palette, "×");
    if (!clearBtn) {
      throw new Error("clear button not found");
    }
    if (isWindowVisible(clearBtn)) {
      throw new Error("clear button should be hidden while the query is empty");
    }

    // type a query (keep the ">" prefix) -> result set shrinks, count updates,
    // clear button appears (WM_CHAR posts, so wait for the app to process them)
    sendChars(edit, ">Op");
    await sleep(500);
    const narrowedCount = parseResults(getWindowText(info));
    if (narrowedCount <= 0 || narrowedCount >= initialCount) {
      throw new Error(`expected narrower result set, got ${narrowedCount} (was ${initialCount})`);
    }
    if (!isWindowVisible(clearBtn)) {
      throw new Error("clear button should be visible while typing");
    }

    // click the clear button -> query empties, full list returns, button hides
    await clickAt(clearBtn, 2, 2);
    await sleep(600);
    if (getWindowText(edit) !== "") {
      throw new Error(`clear button did not empty the query ("${getWindowText(edit)}")`);
    }
    const resetCount = parseResults(getWindowText(info));
    if (resetCount !== initialCount) {
      throw new Error(`expected count to reset to ${initialCount}, got ${resetCount}`);
    }
    if (isWindowVisible(clearBtn)) {
      throw new Error("clear button should be hidden after the query is emptied");
    }

    // typing again re-shows the clear button
    sendChars(edit, ">Op");
    await sleep(500);
    if (!isWindowVisible(clearBtn)) {
      throw new Error("clear button should reappear when typing resumes");
    }

    // --- close with Esc (twice: the query is non-empty, so the first Esc
    // clears it, the second closes the palette) ---
    await pressEscape(edit);
    await sleep(400);
    if (getWindowText(edit) !== "") {
      throw new Error(`first Esc should clear the query, got "${getWindowText(edit)}"`);
    }
    await pressEscape(edit);
    await sleep(1200);
    // the frame and other top-level windows of the process can also have Edit
    // children (e.g. the find bar), so check the palette hwnd directly
    if (isWindow(palette)) {
      throw new Error("Command Palette didn't close on Esc");
    }
  } finally {
    proc.kill();
  }
}

if (import.meta.main) {
  await runStandalone(testit);
}
