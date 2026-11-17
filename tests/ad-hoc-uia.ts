// Ad-hoc test for the UI Automation provider (issue #321, "not accessible to
// screen readers").
//
// Narrator / NVDA read a document through UIA: they look for the Document
// element in the *control* / *content* view, get its TextPattern, and then walk
// a text range with ExpandToEnclosingUnit() + Move() to read character by
// character, word by word, line by line. This test does exactly that from a
// real UIA client (tests/ad-hoc-uia.cpp, compiled on demand with cl.exe) and
// checks that:
//   - the Document element is in the control and content views
//   - its TextPattern returns the document text
//   - walking by character / word / line advances instead of repeating
//   - reading the whole document line by line terminates
//   - a tagged PDF exposes its H1/H2 as structure elements with HeadingLevel,
//     and an untagged one exposes no structure at all
//
// It is ad-hoc (not in tests/run-almost-all.ts) because it needs the Visual Studio
// command-line tools to build the client; without cl.exe it skips.

import { existsSync, mkdirSync, readdirSync, rmSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { EXE, ROOT, tmpPath } from "./util";
import {
  moveWindow,
  showWindow,
  sleep,
  waitForChildWindow,
  waitForTopWindow,
  SW_RESTORE,
  killAndWait,
  killProcessesNamed,
} from "./winapi";

const PAGE_LINES = [
  ["Alpha line one", "Bravo line two", "Charlie line three", "Delta line four"],
  ["Echo line five", "Foxtrot line six", "Golf line seven", "Hotel line eight"],
];

// a PDF with the given lines of real text, one page per group
function makeTextPdf(pages: string[][]): Buffer {
  const enc = (s: string) => Buffer.from(s, "latin1");
  const objs: Buffer[] = [];
  const add = (s: string) => {
    objs.push(enc(s));
    return objs.length; // 1-based object number
  };
  add("<< /Type /Catalog /Pages 2 0 R >>"); // 1
  add("PAGES_PLACEHOLDER"); // 2, patched below
  const fontNo = add("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>");

  const pageNos: number[] = [];
  for (const lines of pages) {
    let stream = "BT /F1 18 Tf 72 700 Td 24 TL\n";
    for (const line of lines) {
      stream += `(${line}) Tj T*\n`;
    }
    stream += "ET";
    const contentNo = add(`<< /Length ${stream.length} >>\nstream\n${stream}\nendstream`);
    pageNos.push(
      add(
        `<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] ` +
          `/Resources << /Font << /F1 ${fontNo} 0 R >> >> /Contents ${contentNo} 0 R >>`,
      ),
    );
  }
  const kids = pageNos.map((n) => `${n} 0 R`).join(" ");
  objs[1] = enc(`<< /Type /Pages /Kids [${kids}] /Count ${pageNos.length} >>`);

  const parts: Buffer[] = [enc("%PDF-1.7\n")];
  const offsets: number[] = [];
  let pos = parts[0]!.length;
  objs.forEach((body, i) => {
    offsets.push(pos);
    const obj = Buffer.concat([enc(`${i + 1} 0 obj\n`), body, enc("\nendobj\n")]);
    parts.push(obj);
    pos += obj.length;
  });
  let xref = `xref\n0 ${objs.length + 1}\n0000000000 65535 f \n`;
  for (const off of offsets) {
    xref += `${String(off).padStart(10, "0")} 00000 n \n`;
  }
  parts.push(enc(`${xref}trailer\n<< /Size ${objs.length + 1} /Root 1 0 R >>\nstartxref\n${pos}\n%%EOF\n`));
  return Buffer.concat(parts);
}

// A tagged PDF whose first line is an H1 and whose third is an H2, so the
// structure elements a screen reader navigates by can be checked. Both MCIDs
// have an element in the tree and an entry in the ParentTree: an MCID that is
// used in the content stream but missing from the tree is malformed, and mupdf
// then leaves the previous structure element open over the text that follows.
function makeTaggedPdf(lines: string[], headingLevels: number[]): Buffer {
  const enc = (s: string) => Buffer.from(s, "latin1");
  const objs: Buffer[] = [];
  const add = (s: string) => {
    objs.push(enc(s));
    return objs.length;
  };
  const catalog = add("CATALOG_PLACEHOLDER"); // 1, patched below
  const pagesNo = add("PAGES_PLACEHOLDER"); // 2, patched below
  const fontNo = add("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>");

  // wrap each line in the marked content its structure element claims
  let stream = "BT /F1 18 Tf 72 700 Td 24 TL\n";
  lines.forEach((line, i) => {
    const tag = headingLevels[i] ? `/H${headingLevels[i]}` : "/P";
    stream += `${tag} << /MCID ${i} >> BDC\n(${line}) Tj T*\nEMC\n`;
  });
  stream += "ET";
  const contentNo = add(`<< /Length ${stream.length} >>\nstream\n${stream}\nendstream`);
  const pageNo = add(
    `<< /Type /Page /Parent ${pagesNo} 0 R /MediaBox [0 0 612 792] /StructParents 0 ` +
      `/Resources << /Font << /F1 ${fontNo} 0 R >> >> /Contents ${contentNo} 0 R >>`,
  );

  // one structure element per line, then the ParentTree indexing them by MCID
  const elemNos: number[] = [];
  for (let i = 0; i < lines.length; i++) {
    const tag = headingLevels[i] ? `/H${headingLevels[i]}` : "/P";
    elemNos.push(
      add(
        `<< /Type /StructElem /S ${tag} /P DOCUMENT_OBJ 0 R /Pg ${pageNo} 0 R /K ${i} >>`.replace("DOCUMENT_OBJ", "0"),
      ),
    );
  }
  const parentTreeNo = add(`<< /Nums [0 [${elemNos.map((n) => `${n} 0 R`).join(" ")}]] >>`);
  const structRootNo = add(`<< /Type /StructTreeRoot /ParentTree ${parentTreeNo} 0 R /ParentTreeNextKey 1 >>`);
  const documentNo = add(
    `<< /Type /StructElem /S /Document /P ${structRootNo} 0 R /K [${elemNos.map((n) => `${n} 0 R`).join(" ")}] >>`,
  );
  for (const n of elemNos) {
    objs[n - 1] = enc(objs[n - 1]!.toString("latin1").replace("P 0 0 R", `P ${documentNo} 0 R`));
  }

  objs[pagesNo - 1] = enc(`<< /Type /Pages /Kids [${pageNo} 0 R] /Count 1 >>`);
  objs[catalog - 1] = enc(
    `<< /Type /Catalog /Pages ${pagesNo} 0 R /StructTreeRoot ${structRootNo} 0 R /MarkInfo << /Marked true >> >>`,
  );

  const parts: Buffer[] = [enc("%PDF-1.7\n")];
  const offsets: number[] = [];
  let pos = parts[0]!.length;
  objs.forEach((body, i) => {
    offsets.push(pos);
    const obj = Buffer.concat([enc(`${i + 1} 0 obj\n`), body, enc("\nendobj\n")]);
    parts.push(obj);
    pos += obj.length;
  });
  let xref = `xref\n0 ${objs.length + 1}\n0000000000 65535 f \n`;
  for (const off of offsets) {
    xref += `${String(off).padStart(10, "0")} 00000 n \n`;
  }
  parts.push(enc(`${xref}trailer\n<< /Size ${objs.length + 1} /Root 1 0 R >>\nstartxref\n${pos}\n%%EOF\n`));
  return Buffer.concat(parts);
}

// compile the UIA client; null when the VS command-line tools aren't available
function buildClient(): string | null {
  const src = join(ROOT, "tests", "ad-hoc-uia.cpp");
  const exe = tmpPath("ad-hoc-uia-client.exe");
  const which = Bun.spawnSync(["where", "cl.exe"]);
  if (which.exitCode !== 0) {
    return null;
  }
  const args = ["/nologo", "/EHsc", "/std:c++17", src, `/Fe${exe}`, `/Fo${tmpPath("ad-hoc-uia-client.obj")}`];

  // cl.exe is only usable with the toolset's own include/lib directories. It
  // inherits them from vcvarsall.bat, which a plain shell has not run, so find
  // them and pass them explicitly - otherwise every #include fails.
  const sdkRoot = process.env["WindowsSdkDir"] || findWindowsSdk();
  const msVcRoot = findMsVcRoot();
  if (sdkRoot && msVcRoot) {
    const sdkVer = sdkVersion(sdkRoot);
    const ucrt = join(sdkRoot, "Include", sdkVer, "ucrt");
    const shared = join(sdkRoot, "Include", sdkVer, "shared");
    const um = join(sdkRoot, "Include", sdkVer, "um");
    const winrt = join(sdkRoot, "Include", sdkVer, "winrt");
    for (const dir of [join(msVcRoot, "include"), ucrt, shared, um, winrt]) {
      args.push(`/I${dir}`);
    }
    const libDirs = [
      join(msVcRoot, "lib", "x64"),
      join(sdkRoot, "Lib", sdkVer, "ucrt", "x64"),
      join(sdkRoot, "Lib", sdkVer, "um", "x64"),
    ];
    for (const dir of libDirs) {
      args.push("/link", `/LIBPATH:${dir}`);
    }
  } else {
    console.log("could not locate the MSVC toolset / Windows SDK; hoping INCLUDE is set");
  }

  args.push("/link", "ole32.lib", "oleaut32.lib", "user32.lib", "uiautomationcore.lib");
  const r = Bun.spawnSync(["cl.exe", ...args], { cwd: tmpPath("") });
  if (!existsSync(exe)) {
    throw new Error(`could not build the UIA client:\n${r.stdout.toString()}\n${r.stderr.toString()}`);
  }
  return exe;
}

function findMsVcRoot(): string | null {
  const vswhere = join(process.env["ProgramFiles(x86)"] ?? "", "Microsoft Visual Studio", "Installer", "vswhere.exe");
  if (!existsSync(vswhere)) {
    return null;
  }
  const r = Bun.spawnSync([
    vswhere,
    "-latest",
    "-products",
    "*",
    "-requires",
    "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
    "-property",
    "installationPath",
  ]);
  const inst = r.stdout.toString().trim();
  if (!inst) {
    return null;
  }
  const msVc = join(inst, "VC", "Tools", "MSVC");
  const versions = readdirSync(msVc, { withFileTypes: true })
    .filter((d) => d.isDirectory())
    .map((d) => d.name)
    .sort()
    .reverse();
  return versions.length ? join(msVc, versions[0]) : null;
}

function findWindowsSdk(): string | null {
  const root = join(process.env["ProgramFiles(x86)"] ?? "", "Windows Kits", "10");
  if (!existsSync(root)) {
    return null;
  }
  const versions = readdirSync(join(root, "Include"), { withFileTypes: true })
    .filter((d) => d.isDirectory() && /^\d+\./.test(d.name))
    .map((d) => d.name)
    .sort()
    .reverse();
  for (const v of versions) {
    if (existsSync(join(root, "Include", v, "um", "windows.h"))) {
      return root;
    }
  }
  return null;
}

function sdkVersion(sdkRoot: string): string {
  const versions = readdirSync(join(sdkRoot, "Include"), { withFileTypes: true })
    .filter((d) => d.isDirectory() && /^\d+\./.test(d.name))
    .map((d) => d.name)
    .sort()
    .reverse();
  for (const v of versions) {
    if (existsSync(join(sdkRoot, "Include", v, "um", "windows.h"))) {
      return v;
    }
  }
  return versions[0] ?? "";
}

function parseKeyValues(out: string): Map<string, string> {
  const res = new Map<string, string>();
  for (const line of out.split(/\r?\n/)) {
    const i = line.indexOf("=");
    if (i > 0) {
      res.set(
        line.slice(0, i),
        line
          .slice(i + 1)
          .replace(/\\n/g, "\n")
          .replace(/\\p/g, "|"),
      );
    }
  }
  return res;
}

const SETTINGS = [`RestoreSession = false`, `ShowStartPage = false`, `CheckForUpdates = false`, ``].join("\n");

export async function testit(): Promise<void> {
  const client = buildClient();
  if (!client) {
    console.log("  cl.exe not in PATH, skipping the UI Automation test");
    return;
  }

  const pdf = tmpPath("ad-hoc-uia.pdf");
  writeFileSync(pdf, makeTextPdf(PAGE_LINES));
  const appdata = tmpPath("ad-hoc-uia-appdata");
  rmSync(appdata, { recursive: true, force: true });
  mkdirSync(appdata, { recursive: true });
  writeFileSync(join(appdata, "SumatraPDF-settings.txt"), SETTINGS);

  await killProcessesNamed("SumatraPDF.exe");
  const proc = Bun.spawn([EXE, "-for-testing", "-appdata", appdata, pdf], { stdout: "ignore", stderr: "ignore" });
  try {
    const frame = await waitForTopWindow(proc.pid, "SUMATRA_PDF_FRAME");
    if (!frame) {
      throw new Error("SumatraPDF main window did not appear");
    }
    showWindow(frame, SW_RESTORE);
    moveWindow(frame, 60, 60, 1100, 850);
    const canvas = await waitForChildWindow(frame, "SUMATRA_PDF_CANVAS");
    if (!canvas) {
      throw new Error("could not find the canvas window");
    }
    await sleep(1500);

    const r = Bun.spawnSync([client, String(canvas)]);
    const out = r.stdout.toString() + r.stderr.toString();
    const kv = parseKeyValues(out);
    const need = (k: string) => {
      const v = kv.get(k);
      if (v === undefined) {
        throw new Error(`UIA client did not report ${k}:\n${out}`);
      }
      return v;
    };

    if (need("doc.found") !== "1") {
      throw new Error(`no Document element under the canvas:\n${out}`);
    }
    if (need("doc.hasTextPattern") !== "1") {
      throw new Error(`the Document element has no TextPattern:\n${out}`);
    }
    // a Document outside the control / content views is invisible to screen
    // readers even though it holds all the text
    if (need("doc.isControlElement") !== "1" || need("doc.isContentElement") !== "1") {
      throw new Error(
        `Document element is not in the control/content view ` +
          `(isControlElement=${kv.get("doc.isControlElement")}, isContentElement=${kv.get("doc.isContentElement")})`,
      );
    }
    if (need("canvas.kbFocusable") !== "1") {
      throw new Error("the canvas does not report itself as keyboard focusable");
    }

    const firstLine = PAGE_LINES[0]![0]!;
    if (!need("doc.text").startsWith(firstLine)) {
      throw new Error(`document text does not start with '${firstLine}': '${kv.get("doc.text")}'`);
    }
    // an untagged document has no structure tree, so it must not invent
    // structure elements - a screen reader would offer empty heading entries
    if (kv.get("page.0.nStruct") !== "0") {
      throw new Error(`an untagged PDF reported ${kv.get("page.0.nStruct")} structure elements, expected none`);
    }

    // each Move() must land on new text, or a screen reader repeats one unit
    // forever (that was the bug: Move() reported success without moving)
    for (const [key, steps] of [
      ["walk.char", 4],
      ["walk.word", 3],
      ["walk.line", 3],
    ] as const) {
      const seen: string[] = [];
      for (let i = 0; i <= steps; i++) {
        seen.push(need(`${key}${i}`));
      }
      const distinct = new Set(seen).size;
      if (distinct <= 1) {
        throw new Error(`walking by ${key} never advanced: ${JSON.stringify(seen)}`);
      }
      if (distinct < seen.length) {
        throw new Error(`walking by ${key} repeated itself: ${JSON.stringify(seen)}`);
      }
    }
    // the second page must be reachable by page
    if (!need("walk.page1").startsWith(PAGE_LINES[1]![0]!)) {
      throw new Error(`moving one page did not land on page 2: '${kv.get("walk.page1")}'`);
    }

    // Reading what is under the mouse / finger (Narrator's mouse mode, touch
    // exploration) needs two things: GetBoundingRectangles to say where the
    // text is on screen, and RangeFromPoint to say what text is at a point.
    // Both used to be E_NOTIMPL.
    if (need("point.haveRect") !== "1") {
      throw new Error(
        `no on-screen rectangle for the first word: GetBoundingRectangles gives screen readers ` +
          `nowhere to point (Narrator can't highlight what it reads)`,
      );
    }
    if (need("point.hr") !== "0x00000000") {
      throw new Error(`RangeFromPoint failed with ${kv.get("point.hr")}: reading text under the mouse can't work`);
    }
    const want = need("point.word").trim();
    const got = need("point.wordAtPoint").trim();
    if (!got.includes(want) && !want.includes(got)) {
      throw new Error(`RangeFromPoint over the word '${want}' returned '${got}'`);
    }

    if (need("sayall.terminated") !== "1") {
      throw new Error("reading the document line by line never reached the end");
    }
    const nLines = parseInt(need("sayall.lines"), 10);
    const totalLines = PAGE_LINES.reduce((n, p) => n + p.length, 0);
    if (nLines < totalLines - 1) {
      throw new Error(`read only ${nLines} lines, expected about ${totalLines}`);
    }
    console.log(`  UIA: document readable, ${nLines} lines walked, page/word/char navigation advances ✓`);
    console.log(`  UIA: word under the mouse: '${kv.get("point.word")}' -> '${kv.get("point.wordAtPoint")}' ✓`);
  } finally {
    await killAndWait(proc);
  }

  // A tagged PDF's headings have to be reachable as their own elements, with
  // the level the document says, or a screen reader's heading navigation finds
  // nothing. Run the same client against a tagged fixture.
  const taggedPath = tmpPath("ad-hoc-uia-tagged.pdf");
  const taggedLines = ["Chapter One", "Intro paragraph", "Section A", "Body paragraph"];
  const taggedLevels = [1, 0, 2, 0];
  writeFileSync(taggedPath, makeTaggedPdf(taggedLines, taggedLevels));

  await killProcessesNamed("SumatraPDF.exe");
  const taggedProc = Bun.spawn([EXE, "-for-testing", "-appdata", appdata, taggedPath], {
    stdout: "ignore",
    stderr: "ignore",
  });
  try {
    const frame = await waitForTopWindow(taggedProc.pid, "SUMATRA_PDF_FRAME");
    if (!frame) {
      throw new Error("SumatraPDF main window did not appear for the tagged PDF");
    }
    showWindow(frame, SW_RESTORE);
    moveWindow(frame, 60, 60, 1100, 850);
    const canvas = await waitForChildWindow(frame, "SUMATRA_PDF_CANVAS");
    if (!canvas) {
      throw new Error("could not find the canvas window for the tagged PDF");
    }
    await sleep(1500);

    const r = Bun.spawnSync([client, String(canvas)]);
    const out = r.stdout.toString() + r.stderr.toString();
    const kv = parseKeyValues(out);
    const structBits = [...kv.entries()]
      .filter(([k]) => k.startsWith("struct.") || k.startsWith("page."))
      .map(([k, v]) => `${k}=${v}`)
      .join(" ");
    console.log(`  UIA tagged structure: ${structBits}`);

    // two headings, at the levels the PDF declares, named with their own text
    // UIA_HeadingLevel1 = 80051, HeadingLevel2 = 80052
    if (kv.get("struct.0.0.level") !== "80051") {
      throw new Error(`first heading is not level 1 (80051): got ${kv.get("struct.0.0.level")}`);
    }
    if (kv.get("struct.0.1.level") !== "80052") {
      throw new Error(`second heading is not level 2 (80052): got ${kv.get("struct.0.1.level")}`);
    }
    if (kv.get("struct.0.0.name") !== "Chapter One") {
      throw new Error(`first heading's name is '${kv.get("struct.0.0.name")}', expected 'Chapter One'`);
    }
    if (kv.get("page.0.nStruct") !== "2") {
      throw new Error(`expected 2 structure elements, got ${kv.get("page.0.nStruct")}`);
    }
    console.log("  UIA: tagged headings exposed as elements with levels ?");
  } finally {
    await killAndWait(taggedProc);
  }
}

if (import.meta.main) {
  const { runStandalone } = await import("./util");
  await runStandalone(testit);
}
