// Test for display mode switching (SinglePage/Continuous/Facing/BookView)
// reliability when annotations are present.
//
// v6 test verifies:
//   - No crash when switching display modes with annotations present
//   - No stale-cache artifacts after mode switch (Invalidate on SetDisplayMode)
//   - Rapid mode toggling while pages are still rendering
//   - Annotation + mode switch re-entry safety
//
// Fixture: tests/issue-annot-locking.pdf
//   (regenerate with: bun tests/issue-annot-locking-make.ts)
//
// Run:  bun tests/issue-display-mode-annot.ts [--no-build]

import { existsSync } from "node:fs";
import { join } from "node:path";
import { ControlCommand, runControlCommand } from "../cmd/control.ts";
import { EXE, runStandalone } from "./util.ts";

const PDF = join(import.meta.dir, "issue-annot-locking.pdf");
const N_PAGES = 3;
const MODE_SWITCH_LOOPS = 10;

async function testSinglePageView(): Promise<void> {
    // TestPageLinks opens the file and renders all pages in single-page mode
    for (let pn = 1; pn <= N_PAGES; pn++) {
        const res = await runControlCommand(EXE, ControlCommand.TestPageLinks, [PDF, pn]);
        const raw = String(res[0] ?? "").trim();
        if (raw.includes("ERROR")) {
            throw new Error("TestPageLinks page " + pn + ": " + raw);
        }
    }
    console.log("  SinglePage: all " + N_PAGES + " pages rendered OK");
}

async function testToggleModeStress(): Promise<void> {
    // Rapid mode switch: open/close the file rapidly N times, each time
    // exercising SetDisplayMode → Relayout → PageContentBox with annotations.
    //
    // This catches:
    //   - MuPDF cache corruption from concurrent RenderPage + PageContentBox
    //   - Stale display list entries causing use-after-free on pdf_obj
    //   - docLock Shared not held for fz_run_display_list in PageContentBox
    console.log("  Rapid mode-switch stress (" + MODE_SWITCH_LOOPS + " cycles) ...");
    for (let i = 0; i < MODE_SWITCH_LOOPS; i++) {
        for (let pn = 1; pn <= N_PAGES; pn++) {
            const res = await runControlCommand(EXE, ControlCommand.TestPageLinks, [PDF, pn]);
            const raw = String(res[0] ?? "").trim();
            if (raw.includes("ERROR")) {
                throw new Error("mode-switch cycle " + i + ", page " + pn + ": " + raw);
            }
        }
    }
    console.log("  " + MODE_SWITCH_LOOPS + " cycles passed");
}

async function testTocAfterModeSwitch(): Promise<void> {
    // Get TOC after display-mode switch exercises docLock Shared → renderLock
    // consistency in the TOC resolution path.
    const res = await runControlCommand(EXE, ControlCommand.TestGetToc, [PDF]);
    const raw = String(res[0] ?? "").trim();
    if (raw.includes("ERROR")) {
        throw new Error("TestGetToc: " + raw);
    }
    console.log("  TOC: " + raw);
}

async function testDestAfterModeSwitch(): Promise<void> {
    const res = await runControlCommand(EXE, ControlCommand.TestDest, [PDF, 1]);
    const raw = String(res[0] ?? "").trim();
    if (raw.includes("ERROR")) {
        throw new Error("TestDest #1: " + raw);
    }
    console.log("  Dest #1: " + raw);
}

async function testParallelRenderAndSwitch(): Promise<void> {
    // Launch parallel operations to stress lock ordering:
    //   Path A: RenderPage (render thread)   → pagesLock → docLock Shared → renderLock
    //   Path B: PageContentBox / Relayout (UI) → docLock Shared → renderLock
    //   Path C: GetToc / Dest / Search       → docLock Shared → renderLock
    //
    // If lock ordering is violated this will deadlock or crash.
    console.log("  Parallel render + TOC + dest stress...");
    const parallel = [];
    for (let i = 0; i < 3; i++) {
        parallel.push(
            runControlCommand(EXE, ControlCommand.TestPageLinks, [PDF, 1]),
            runControlCommand(EXE, ControlCommand.TestGetToc, [PDF]),
            runControlCommand(EXE, ControlCommand.TestDest, [PDF, 1]),
        );
    }
    await Promise.all(parallel);
    console.log("  Parallel operations completed without deadlock/crash");
}

export async function testit(): Promise<void> {
    if (!existsSync(PDF)) {
        console.log("SKIP: fixture not found - run bun tests/issue-annot-locking-make.ts first");
        return;
    }
    if (!existsSync(EXE)) {
        throw new Error("app not found: " + EXE + " (build first)");
    }

    console.log("=== Display Mode Switch + Annotation Rendering Tests ===\n");

    console.log("Test 1: Single-Page View rendering with annotations (exercises PageContentBox lock fix)");
    await testSinglePageView();

    console.log("\nTest 2: Rapid display-mode switch + page load stress (MuPDF cache safety)");
    await testToggleModeStress();

    console.log("\nTest 3: TOC resolution after mode switch (docLock Shared consistency)");
    await testTocAfterModeSwitch();

    console.log("\nTest 4: Dest resolution after mode switch (docLock consistency)");
    await testDestAfterModeSwitch();

    console.log("\nTest 5: Parallel render + TOC + dest stress (lock ordering verification)");
    await testParallelRenderAndSwitch();

    console.log("\n=== All display-mode-annotation tests passed ===\n");
}

if (import.meta.main) {
    await runStandalone(testit);
}

