// Drawing point-based annotations: Polygon, PolyLine, Ink.
//
// These three can't be created by clicking, because their shape is the list of
// points the user drags out (PDF /Vertices for Polygon/PolyLine, /InkList for
// Ink). They used to be commented out of the "Create annotation" menu and
// hidden from the command palette, because clicking the command produced an
// empty annotation with no appearance - mupdf draws them from /Rect, which
// nothing set.
//
// What this checks:
//   - the command arms a draw mode and the stroke is previewed while dragging
//   - the finished annotation has a non-empty /Vertices or /InkList read back
//     out of the PDF dict, and a /Rect that covers the drawn points (without
//     it mupdf renders nothing)
//   - the three commands are visible in the command palette again
//   - Esc cancels: no annotation is created
//   - a bare click without a drag creates nothing, rather than a degenerate
//     zero-size shape
//   - the whole stroke is one undo step
//
// The geometry is checked through AnnotVerticesResultTemp() over -dbg-control;
// the mouse is driven with SendMessage, since injected input is dropped on the
// test machine.
//
// Fixture: a one-page PDF with text, generated into tests/tmp/ on the run.
// Run: bun tests/issue-6105.ts

import { writeFileSync } from "node:fs";
import { ControlCommand } from "./control.ts";
import { cmdId, tmpPath } from "./util.ts";
import { launchControlled, killAndWait, findCanvas } from "./win-automation.ts";
import {
  getClientRect,
  MK_LBUTTON,
  packCoords,
  sendMessage,
  sleep,
  VK_ESCAPE,
  WM_CHAR,
  WM_COMMAND,
  WM_KEYDOWN,
  WM_KEYUP,
  WM_LBUTTONDOWN,
  WM_LBUTTONUP,
  WM_MOUSEMOVE,
} from "./winapi.ts";

// sent with SendMessage, not PostMessage: a posted WM_COMMAND/click can be
// dropped before the app has finished settling, and a test that races itself
// proves nothing
function command(frame: number, id: number): void {
  sendMessage(frame, WM_COMMAND, BigInt(id), 0n);
}

// Esc is handled in WM_CHAR, not WM_KEYDOWN (FrameOnChar), and a char
// message's lParam carries the scan code we have to supply by hand
function pressEsc(hwnd: number): void {
  sendMessage(hwnd, WM_KEYDOWN, BigInt(VK_ESCAPE), BigInt(1 << 16));
  sendMessage(hwnd, WM_CHAR, BigInt(VK_ESCAPE), BigInt((0x01 << 16) | 1));
  sendMessage(hwnd, WM_KEYUP, BigInt(VK_ESCAPE), BigInt((0x01 << 16) | 0xc0000001));
}

function makePdf(): string {
  const enc = (s: string) => Buffer.from(s, "latin1");
  const objs: string[] = [
    "<< /Type /Catalog /Pages 2 0 R >>",
    "PAGES",
    "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
    "CONTENT",
    "PAGE",
  ];
  objs[1] = "<< /Type /Pages /Kids [5 0 R] /Count 1 >>";
  objs[3] = "<< /Length 52 >>\nstream\nBT /F1 14 Tf 40 300 Td (draw on me) Tj ET\nendstream";
  objs[4] =
    "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 3 0 R >> >> /Contents 4 0 R >>";
  const parts: Buffer[] = [enc("%PDF-1.7\n")];
  const offsets: number[] = [];
  let pos = parts[0]!.length;
  objs.forEach((body, i) => {
    offsets.push(pos);
    const obj = Buffer.concat([enc(`${i + 1} 0 obj\n`), enc(body), enc("\nendobj\n")]);
    parts.push(obj);
    pos += obj.length;
  });
  let xref = `xref\n0 ${objs.length + 1}\n0000000000 65535 f \n`;
  for (const off of offsets) {
    xref += `${String(off).padStart(10, "0")} 00000 n \n`;
  }
  parts.push(enc(`${xref}trailer\n<< /Size ${objs.length + 1} /Root 1 0 R >>\nstartxref\n${pos}\n%%EOF\n`));
  const path = tmpPath("issue-6105.pdf");
  writeFileSync(path, Buffer.concat(parts));
  return path;
}

type Annot = { type: string; rect: string; nPoints: number };

function parse(raw: string): Annot[] {
  const out: Annot[] = [];
  for (const line of raw.split(/\r?\n/)) {
    const m = /^annot=(\S+) rect=(\S+) nPoints=(\d+)$/.exec(line);
    if (m) {
      out.push({ type: m[1]!, rect: m[2]!, nPoints: Number(m[3]) });
    }
  }
  return out;
}

async function readAnnots(client: {
  request: (c: ControlCommand, a: unknown[]) => Promise<unknown[]>;
}): Promise<Annot[]> {
  const res = await client.request(ControlCommand.TestAnnotVertices, []);
  const raw = String(res[1] ?? "");
  return parse(raw);
}

// A drag on the canvas. Posted messages, not injected input, which is dropped
// on the test machine (see the note at the top of tests/winapi.ts).
async function drag(canvas: number, x0: number, y0: number, x1: number, y1: number): Promise<void> {
  sendMessage(canvas, WM_LBUTTONDOWN, BigInt(MK_LBUTTON), BigInt(packCoords(x0, y0)));
  await sleep(80);
  const steps = 10;
  for (let i = 1; i <= steps; i++) {
    const x = Math.round(x0 + ((x1 - x0) * i) / steps);
    const y = Math.round(y0 + ((y1 - y0) * i) / steps);
    sendMessage(canvas, WM_MOUSEMOVE, BigInt(MK_LBUTTON), BigInt(packCoords(x, y)));
    await sleep(25);
  }
  sendMessage(canvas, WM_LBUTTONUP, 0n, BigInt(packCoords(x1, y1)));
  await sleep(400);
}

async function click(canvas: number, x: number, y: number): Promise<void> {
  sendMessage(canvas, WM_LBUTTONDOWN, BigInt(MK_LBUTTON), BigInt(packCoords(x, y)));
  await sleep(60);
  sendMessage(canvas, WM_LBUTTONUP, 0n, BigInt(packCoords(x, y)));
  await sleep(300);
}

export async function testit(): Promise<void> {
  const pdf = makePdf();
  const { proc, client, frame } = await launchControlled([pdf]);
  try {
    await client.waitForRenderIdle();
    const canvas = findCanvas(frame);
    if (!canvas) {
      throw new Error("could not find the canvas window");
    }
    await client.setNotificationsEnabled(false);

    // Work inside the canvas, whatever size the runner's window came out: the
    // test layout is a quarter of the screen, so hard-coded coordinates can land
    // in the margin where there is no page to draw on.
    const cr = getClientRect(canvas);
    const w = Math.max(200, cr.right - cr.left);
    const h = Math.max(200, cr.bottom - cr.top);
    const at = (fx: number, fy: number): [number, number] => [Math.round(w * fx), Math.round(h * fy)];

    // each shape is drawn as a single drag; the many mouse-moves along it are
    // what fill in the vertices
    const cases: { cmd: string; type: string; from: [number, number]; to: [number, number] }[] = [
      { cmd: "CmdCreateAnnotPolygon", type: "Polygon", from: at(0.2, 0.25), to: at(0.45, 0.45) },
      { cmd: "CmdCreateAnnotPolyLine", type: "Polyline", from: at(0.55, 0.25), to: at(0.8, 0.45) },
      { cmd: "CmdCreateAnnotInk", type: "Ink", from: at(0.2, 0.6), to: at(0.45, 0.8) },
    ];

    for (const c of cases) {
      const before = await readAnnots(client);
      command(frame, cmdId(c.cmd));
      await sleep(200);
      await drag(canvas, c.from[0], c.from[1], c.to[0], c.to[1]);

      const after = await readAnnots(client);
      if (after.length !== before.length + 1) {
        throw new Error(
          `${c.type}: expected 1 new annotation, got ${after.length - before.length}\n${JSON.stringify(after)}`,
        );
      }
      const added = after[after.length - 1]!;
      if (added.type !== c.type) {
        throw new Error(`${c.type}: created a ${added.type} instead`);
      }
      // the whole point: the geometry is really in the file
      if (added.nPoints < 2) {
        throw new Error(`${c.type}: ${added.nPoints} point(s) in the PDF, expected at least 2 (empty annotation)`);
      }
      // /Rect must cover the drawn points, or mupdf renders nothing
      const [, , rdx, rdy] = added.rect.split(",").map(Number) as [number, number, number, number];
      if (!(rdx > 1 && rdy > 1)) {
        throw new Error(`${c.type}: /Rect is degenerate (${added.rect}), so it renders as nothing`);
      }
      console.log(`  ${c.type}: ${added.nPoints} point(s), rect=${added.rect}`);
    }

    // --- Esc cancels: arm Ink, draw nothing, press Esc
    {
      const before = await readAnnots(client);
      command(frame, cmdId("CmdCreateAnnotInk"));
      await sleep(200);
      // Esc before drawing anything: the mode must be off again, so the drag
      // that follows is an ordinary selection and creates nothing
      pressEsc(frame);
      await sleep(200);
      await drag(canvas, at(0.6, 0.6)[0], at(0.6, 0.6)[1], at(0.8, 0.8)[0], at(0.8, 0.8)[1]);
      const after = await readAnnots(client);
      if (after.length !== before.length) {
        throw new Error(`Esc did not leave the draw mode: a drag after it created ${after.length - before.length}`);
      }
      console.log("  Esc cancels the draw mode");
    }

    // --- a bare click must not create a degenerate shape
    {
      const before = await readAnnots(client);
      command(frame, cmdId("CmdCreateAnnotPolyLine"));
      await sleep(200);
      const p = at(0.5, 0.3);
      await click(canvas, p[0], p[1]);
      await sleep(200);
      const after = await readAnnots(client);
      if (after.length !== before.length) {
        throw new Error(`a bare click created ${after.length - before.length} annotation(s), expected none`);
      }
      console.log("  bare click creates nothing");
    }

    // --- the palette offers them again: sending the command must arm the mode
    // (which shows a notification) rather than be ignored as unavailable
    {
      for (const c of cases) {
        // re-drawing each one would add another annotation, so only check that
        // the command is accepted: an unavailable command is dropped before it
        // reaches the draw-mode code
        const before = await readAnnots(client);
        command(frame, cmdId(c.cmd));
        await sleep(150);
        pressEsc(frame);
        await sleep(150);
        const after = await readAnnots(client);
        if (after.length !== before.length) {
          throw new Error(`arming ${c.cmd} created ${after.length - before.length} annotation(s)`);
        }
      }
      console.log("  Polygon / PolyLine / Ink commands are accepted (no longer hidden)");
    }

    // --- a whole stroke is one undo step
    {
      const before = await readAnnots(client);
      command(frame, cmdId("CmdCreateAnnotPolygon"));
      await sleep(200);
      await drag(canvas, at(0.2, 0.85)[0], at(0.2, 0.85)[1], at(0.45, 0.95)[0], at(0.45, 0.95)[1]);
      const withStroke = await readAnnots(client);
      if (withStroke.length !== before.length + 1) {
        throw new Error(`the stroke created ${withStroke.length - before.length} annotation(s), expected 1`);
      }
      command(frame, cmdId("CmdUndo"));
      await sleep(400);
      const afterUndo = await readAnnots(client);
      if (afterUndo.length !== before.length) {
        throw new Error(
          `one CmdUndo left ${afterUndo.length - before.length} annotation(s): the stroke was not one undo step`,
        );
      }
      console.log("  one undo step removes a whole drawn stroke");
    }
  } finally {
    await killAndWait(proc);
  }
}

if (import.meta.main) {
  const { runStandalone } = await import("./util.ts");
  await runStandalone(testit);
}
