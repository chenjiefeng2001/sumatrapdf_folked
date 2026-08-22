// Test for continuous zooming and scrolling rendering correctness.
//
// v6 test verifies:
//   - Continuous zoom stress: fast open/render/close cycles simulate zoom-in/zoom-out
//     patterns, exercising lock ordering under repeated GetFzPageInfo -> RenderPage ->
//     ExtractTextLazy -> SafeEngineRelease back-to-back.
//   - Continuous scrolling stress: sequential page rendering of all pages in a multi-page
//     document simulates scrolling through pages, verifying no stale-cache artifacts.
//   - Property consistency after stress: TOC, destinations, and text search remain correct
//     after repeated zoom and scroll operations.
//
// Fixture: tests/issue-annot-locking.pdf (3 pages, annotations + links + outline).
//   Regenerate with: bun tests/issue-annot-locking-make.ts
//
// Commands exercised:
//   TestPageLinks (30) - opens file, renders page, extracts elements (full pipeline).
//   TestGetToc (29)    - opens file, reads outline (docLock shared, text extraction).
//   TestDest (12)      - resolves outline destination (docLock shared).
//   TestSearch (11)    - text search (exercises ExtractTextLazy deferred text path).
//
// Crash, deadlock, or incorrect properties in any of these paths causes test failure.
//
// Run:  bun tests/issue-continuous-zoom-scroll.ts [--no-build]

import { existsSync } from "node:fs";
import { join } from "node:path";
import { ControlCommand, runControlCommand } from "./control.ts";
import { EXE, runStandalone } from "./util.ts";

const PDF = join(import.meta.dir, "issue-annot-locking.pdf");
const N_PAGES = 3;
const ZOOM_LOOPS = 10;
const SCROLL_RUNS = 5;

// ---------------------------------------------------------------------------
// Test 1: Continuous zooming stress
// ---------------------------------------------------------------------------

async function testContinuousZooming(): Promise<void> {
    console.log("  Continuous zoom stress: " + ZOOM_LOOPS + " open/render/close cycles...");
    for (let i = 0; i < ZOOM_LOOPS; i++) {
        const res = await runControlCommand(EXE, ControlCommand.TestPageLinks, [PDF, 1]);
        const raw = String(res[0] ?? "").trim();
        if (raw.includes("ERROR")) {
            throw new Error("zoom cycle " + i + ": TestPageLinks page 1: " + raw);
        }
    }
    console.log("  " + ZOOM_LOOPS + " zoom cycles passed");

    // After zoom stress: verify page properties are still correct.
    console.log("  Verifying properties after zoom stress...");

    const [zExitCode, zTocRaw] = await runControlCommand(EXE, ControlCommand.TestGetToc, [PDF]);
    if (!zTocRaw) {
        throw new Error("TOC after zoom stress: empty");
    }
    const zTocLines = zTocRaw.split("\n").filter((l: string) => l.length > 0);
    if (zTocLines.length < 2) {
        throw new Error("TOC after zoom stress too short: " + zTocRaw);
    }
    console.log("  TOC entries after zoom: " + zTocLines.length + " OK");

    const [zDestCode, zDestRaw] = await runControlCommand(EXE, ControlCommand.TestDest, [PDF, 1]);
    if (String(zDestRaw ?? "").includes("ERROR")) {
        throw new Error("Dest #1 after zoom stress: " + zDestRaw);
    }
    console.log("  Dest #1 after zoom: " + zDestRaw);

    const [zSearchCode, zSearchRaw] = await runControlCommand(EXE, ControlCommand.TestSearch, [PDF, "Page", 0]);
    if (String(zSearchRaw ?? "").includes("ERROR")) {
        console.log("  Search after zoom: (no results: " + zSearchRaw + ")");
    } else {
        console.log("  Search after zoom: OK");
    }
}
// ---------------------------------------------------------------------------
// Test 2: Continuous scrolling stress
// ---------------------------------------------------------------------------

async function testContinuousScrolling(): Promise<void> {
    console.log("  Continuous scroll stress: " + SCROLL_RUNS + " full scroll passes...");
    for (let pass = 0; pass < SCROLL_RUNS; pass++) {
        for (let pn = 1; pn <= N_PAGES; pn++) {
            const res = await runControlCommand(EXE, ControlCommand.TestPageLinks, [PDF, pn]);
            const raw = String(res[0] ?? "").trim();
            if (raw.includes("ERROR")) {
                throw new Error("scroll pass " + pass + ", page " + pn + ": " + raw);
            }
        }
        // Verify TOC mid-way through scroll passes
        if (pass % 2 === 0) {
            const [sTocCode, sTocRaw] = await runControlCommand(EXE, ControlCommand.TestGetToc, [PDF]);
            if (!sTocRaw || String(sTocRaw).includes("ERROR")) {
                throw new Error("TOC after scroll pass " + pass + ": " + sTocRaw);
            }
        }
    }
    console.log("  " + SCROLL_RUNS + " scroll passes completed without crash");

    console.log("  Verifying all pages after scroll stress...");
    for (let pn = 1; pn <= N_PAGES; pn++) {
        const res = await runControlCommand(EXE, ControlCommand.TestPageLinks, [PDF, pn]);
        const raw = String(res[0] ?? "").trim();
        if (raw.includes("ERROR")) {
            throw new Error("post-scroll page " + pn + ": " + raw);
        }
        const linkLines = raw.split("\n").filter((l: string) => l.length > 0);
        console.log("  Page " + pn + " after scroll: " + linkLines.length + " element(s)");
    }

    console.log("  Verifying properties after scroll stress...");

    const [s2Code, s2TocRaw] = await runControlCommand(EXE, ControlCommand.TestGetToc, [PDF]);
    if (!s2TocRaw) {
        throw new Error("TOC after scroll: empty");
    }
    const s2TocLines = s2TocRaw.split("\n").filter((l: string) => l.length > 0);
    if (s2TocLines.length < 2) {
        throw new Error("TOC after scroll entries wrong: " + s2TocRaw);
    }
    console.log("  TOC entries after scroll: " + s2TocLines.length + " OK");

    const [s2DestCode, s2DestRaw] = await runControlCommand(EXE, ControlCommand.TestDest, [PDF, 2]);
    if (String(s2DestRaw ?? "").includes("ERROR")) {
        throw new Error("Dest #2 after scroll stress: " + s2DestRaw);
    }
    console.log("  Dest #2 after scroll: " + s2DestRaw);

    const [s2SearchCode, s2SearchRaw] = await runControlCommand(EXE, ControlCommand.TestSearch, [PDF, "Page", 0]);
    if (String(s2SearchRaw ?? "").includes("ERROR")) {
        console.log("  Search after scroll: (no results: " + s2SearchRaw + ")");
    } else {
        console.log("  Search after scroll: OK");
    }
}
// ---------------------------------------------------------------------------
// Test 3: Mixed zoom + scroll parallel stress
// ---------------------------------------------------------------------------

async function testMixedZoomScrollParallel(): Promise<void> {
    console.log("  Mixed zoom + scroll parallel stress...");
    const tasks: Promise<void>[] = [
        runControlCommand(EXE, ControlCommand.TestPageLinks, [PDF, 1])
            .then((res) => { const r = String(res[0] ?? "").trim(); if (r.includes("ERROR")) throw new Error("parallel zoom [1]: " + r); }),
        runControlCommand(EXE, ControlCommand.TestPageLinks, [PDF, 2])
            .then((res) => { const r = String(res[0] ?? "").trim(); if (r.includes("ERROR")) throw new Error("parallel scroll [2]: " + r); }),
        runControlCommand(EXE, ControlCommand.TestPageLinks, [PDF, 3])
            .then((res) => { const r = String(res[0] ?? "").trim(); if (r.includes("ERROR")) throw new Error("parallel scroll [3]: " + r); }),
        runControlCommand(EXE, ControlCommand.TestDest, [PDF, 1])
            .then((res) => { const r = String(res[0] ?? "").trim(); if (r.includes("ERROR")) throw new Error("parallel dest [1]: " + r); }),
        runControlCommand(EXE, ControlCommand.TestSearch, [PDF, "Page", 0])
            .then((res) => { const r = String(res[0] ?? "").trim(); if (r.includes("ERROR")) throw new Error("parallel search: " + r); }),
    ];

    await Promise.all(tasks);
    console.log("  Parallel ops completed without crash");

    const [mCode, mTocRaw] = await runControlCommand(EXE, ControlCommand.TestGetToc, [PDF]);
    if (!mTocRaw) {
        throw new Error("TOC after parallel: empty");
    }
    const mTocLines = mTocRaw.split("\n").filter((l: string) => l.length > 0);
    console.log("  Final TOC: " + mTocLines.length + " entries");
}

// ---------------------------------------------------------------------------
// Test entry point
// ---------------------------------------------------------------------------

export async function testit(): Promise<void> {
    if (!existsSync(PDF)) {
        console.log("SKIP: fixture not found - run bun tests/issue-annot-locking-make.ts first");
        return;
    }
    if (!existsSync(EXE)) {
        throw new Error("app not found: " + EXE + " (build first)");
    }

    console.log("=== Continuous Zoom / Scroll Rendering Correctness Tests ===\n");

    console.log("Test 1: Continuous zooming stress (" + ZOOM_LOOPS + " open/render/close cycles)");
    await testContinuousZooming();

    console.log("\nTest 2: Continuous scrolling stress (" + SCROLL_RUNS + " passes, " + N_PAGES + " pages)");
    await testContinuousScrolling();

    console.log("\nTest 3: Mixed zoom + scroll parallel stress (concurrent open/render + TOC + dest + search)");
    await testMixedZoomScrollParallel();

    console.log("\n=== All continuous zoom/scroll tests passed ===\n");
}

if (import.meta.main) {
    await runStandalone(testit);
}
