import { mkdirSync, rmSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { cmdId, EXE, tmpPath } from "./util.ts";
import { enumWindows, getClassName, getWindowPid, getWindowText, getWindowTextFull } from "./winapi.ts";
import { FRAME_CLASS, killAndWait, sendCommandSync } from "./win-automation.ts";

const APPDATA = tmpPath("ad-hoc-multi-pdf-windows-appdata");

function makePdf(): Uint8Array {
  const header = "%PDF-1.4\n";
  const objs = [
    "<< /Type /Catalog /Pages 2 0 R >>",
    "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
    "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 200] >>",
  ];
  let body = "";
  const offsets: number[] = [];
  let pos = header.length;
  for (let i = 0; i < objs.length; i++) {
    offsets.push(pos);
    const s = `${i + 1} 0 obj\n${objs[i]}\nendobj\n`;
    body += s;
    pos += s.length;
  }
  const xrefPos = pos;
  let xref = `xref\n0 ${objs.length + 1}\n0000000000 65535 f \n`;
  for (const off of offsets) xref += `${off.toString().padStart(10, "0")} 00000 n \n`;
  xref += `trailer\n<< /Size ${objs.length + 1} /Root 1 0 R >>\nstartxref\n${xrefPos}\n%%EOF\n`;
  return new TextEncoder().encode(header + body + xref);
}

function frames(): number[] {
  const out: number[] = [];
  enumWindows((h) => {
    if (getClassName(h) === FRAME_CLASS) out.push(h);
    return true;
  });
  return out;
}

function snapshot(label: string): void {
  const fs = frames();
  console.log(`\n[${label}] frame windows: ${fs.length}`);
  for (const h of fs) {
    console.log(`  hwnd=0x${h.toString(16)} pid=${getWindowPid(h)} title="${getWindowText(h)}"`);
  }
}

const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));

async function main() {
  // kill any stale dev instances that could interfere with reuse
  await killStale();

  rmSync(APPDATA, { recursive: true, force: true });
  mkdirSync(APPDATA, { recursive: true });
  const useTabs = process.env.USE_TABS !== "0";
  writeFileSync(
    join(APPDATA, "SumatraPDF-settings.txt"),
    ["RestoreSession = false", "ReuseInstance = true", `UseTabs = ${useTabs}`, "CheckForUpdates = false", ""].join(
      "\n",
    ),
  );
  console.log(`=== UseTabs = ${useTabs} ===`);

  const pdfPaths: string[] = [];
  for (let i = 1; i <= 3; i++) {
    const p = tmpPath(`ad-hoc-multi-pdf-windows-${i}.pdf`);
    writeFileSync(p, makePdf());
    pdfPaths.push(p);
  }

  const fast = process.env.FAST === "1";
  const multiArg = process.env.MULTIARG === "1";
  const procs: Bun.Subprocess[] = [];
  try {
    if (multiArg) {
      const proc = Bun.spawn([EXE, "-appdata", APPDATA, ...pdfPaths], { stdout: "ignore", stderr: "ignore" });
      procs.push(proc);
      await sleep(3000);
      snapshot("after 1 launch with 3 file args");
    }
    for (let i = 0; i < pdfPaths.length && !multiArg; i++) {
      const proc = Bun.spawn([EXE, "-appdata", APPDATA, pdfPaths[i]], { stdout: "ignore", stderr: "ignore" });
      procs.push(proc);
      if (fast) continue;
      // wait until a frame window title mentions this pdf's basename
      const want = pdfPaths[i].split(/[\\/]/).pop()!;
      let ok = false;
      for (let t = 0; t < 100; t++) {
        await sleep(100);
        for (const h of frames()) {
          if (getWindowTextFull(h).includes(want)) {
            ok = true;
            break;
          }
        }
        if (ok) break;
      }
      console.log(
        `launch ${i + 1} (${want}): title-visible=${ok}, procs-alive=${procs.filter((p) => !p.killed).length}`,
      );
      snapshot(`after launch ${i + 1}`);
    }
    if (fast) {
      await sleep(3000);
      snapshot("after 3 rapid launches (3s settle)");
    }

    // Verify the single window really holds one tab per PDF by cycling tabs.
    const fs = frames();
    if (fs.length === 1) {
      const seen = new Set<string>();
      for (let i = 0; i < pdfPaths.length + 1; i++) {
        seen.add(getWindowText(fs[0]));
        sendCommandSync(fs[0], cmdId("CmdPrevTab"));
        await sleep(200);
      }
      console.log(`\n[tabs] titles seen while cycling: ${[...seen].join(" | ")}`);
      const allPresent = pdfPaths.every((p) => {
        const name = p.split(/[\\/]/).pop()!;
        return [...seen].some((t) => t.includes(name));
      });
      console.log(`[tabs] all ${pdfPaths.length} PDFs present as tabs: ${allPresent}`);
    }
  } finally {
    for (const p of procs) {
      await killAndWait(p);
    }
    await killStale();
  }
}

async function killStale() {
  try {
    const { killProcessesNamed } = await import("./win-automation.ts");
    await killProcessesNamed("SumatraPDF.exe");
  } catch {
    // ignore
  }
}

await main();
