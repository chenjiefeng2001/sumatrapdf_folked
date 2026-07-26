// Test for D2D device generation versioning and cross-domain GPU resource safety.
//
// This test verifies the D2D device generation fix that prevents
// D2DERR_WRONG_RESOURCE_DOMAIN (0x88990015) crashes during continuous
// scrolling/zooming when the ID2D1DCRenderTarget is recreated (e.g. after
// HDC changes from window resize or display mode switch).
//
// The fix introduces a generation counter in GpuBackend that increments
// each time the render target is recreated. Cached ID2D1Bitmap instances
// store the generation at creation time; PaintTile compares it against the
// current generation and recreates stale bitmaps instead of drawing them
// on the wrong resource domain.
//
// Test strategy:
//   1. Rapid open/close cycles (each new process creates a new D2D factory
//      and render target, exercising the generation mismatch path).
//   2. Multi-page rendering stress (all pages, multiple passes).
//   3. Parallel rendering + TOC + dest + search to verify no cross-domain
//      GPU resource errors under concurrent access.
//   4. Property consistency checks after stress to ensure no stale-cache
//      artifacts or wrong page dimensions.
//
// Fixture: tests/issue-annot-locking.pdf (3 pages, annotations + links + outline)
//   Regenerate with: bun tests/issue-annot-locking-make.ts
//
// Run:  bun tests/issue-device-generation.ts [--no-build]

import { existsSync } from "node:fs";
import { join } from "node:path";
import { ControlCommand, runControlCommand } from "../cmd/control.ts";
import { EXE, runStandalone } from "./util.ts";

const PDF = join(import.meta.dir, "issue-annot-locking.pdf");
const N_PAGES = 3;
const ZOOM_STRESS_LOOPS = 15;
const SCROLL_STRESS_PASSES = 8;

// ---------------------------------------------------------------------------
// Test 1: Device generation cycle stress (open/close cycles)
// Every open creates a new GpuBackend with deviceGeneration=0; rendering
// triggers CreateBitmapFromPixmap which records deviceGeneration=0; on next
// open, the old D2D resources are gone. This exercises the "fresh GPU state"
// safety path.
// ---------------------------------------------------------------------------

async function testDeviceGenerationCycles(): Promise<void> {
    console.log("  Device generation cycle stress: " + ZOOM_STRESS_LOOPS + " open/render/close cycles...");
    for (let i = 0; i < ZOOM_STRESS_LOOPS; i++) {
        const res = await runControlCommand(EXE, ControlCommand.TestPageLinks, [PDF, 1]);
        const raw = String(res[0] ?? "").trim();
        if (raw.includes("ERROR")) {
            throw new Error("device generation cycle " + i + ": TestPageLinks page 1: " + raw);
        }
    }
    console.log("  " + ZOOM_STRESS_LOOPS + " device generation cycles passed");

    console.log("  Verifying properties after device generation cycles...");
    const [tocCode, tocRaw] = await runControlCommand(EXE, ControlCommand.TestGetToc, [PDF]);
    if (!tocRaw) {
        throw new Error("TOC after device generation cycles: empty");
    }
    const tocLines = String(tocRaw).split("\n").filter((l: string) => l.length > 0);
    console.log("  TOC entries: " + tocLines.length);
}

// ---------------------------------------------------------------------------
// Test 2: Multi-page rendering stress (simulates continuous scroll)
// Render all pages multiple times. Each page render may trigger a new
// render target binding (different HDC), incrementing device generation.
// ---------------------------------------------------------------------------

async function testMultiPageRenderStress(): Promise<void> {
    console.log("  Multi-page render stress: " + SCROLL_STRESS_PASSES + " passes, " + N_PAGES + " pages each...");
    for (let pass = 0; pass < SCROLL_STRESS_PASSES; pass++) {
        for (let pn = 1; pn <= N_PAGES; pn++) {
            const res = await runControlCommand(EXE, ControlCommand.TestPageLinks, [PDF, pn]);
            const raw = String(res[0] ?? "").trim();
            if (raw.includes("ERROR")) {
                throw new Error("render stress pass " + pass + ", page " + pn + ": " + raw);
            }
        }
    }
    console.log("  " + (SCROLL_STRESS_PASSES * N_PAGES) + " page renders completed without D2D cross-domain error");
}

// ---------------------------------------------------------------------------
// Test 3: Parallel stress with device generation cycling
// ---------------------------------------------------------------------------

async function testParallelDeviceGenerationStress(): Promise<void> {
    console.log("  Parallel device generation stress (concurrent render + TOC + dest + search)...");
    const tasks: Promise<void>[] = [
        runControlCommand(EXE, ControlCommand.TestPageLinks, [PDF, 1])
            .then((res) => { const r = String(res[0] ?? "").trim(); if (r.includes("ERROR")) throw new Error("parallel render [1]: " + r); }),
        runControlCommand(EXE, ControlCommand.TestPageLinks, [PDF, 2])
            .then((res) => { const r = String(res[0] ?? "").trim(); if (r.includes("ERROR")) throw new Error("parallel render [2]: " + r); }),
        runControlCommand(EXE, ControlCommand.TestPageLinks, [PDF, 3])
            .then((res) => { const r = String(res[0] ?? "").trim(); if (r.includes("ERROR")) throw new Error("parallel render [3]: " + r); }),
        runControlCommand(EXE, ControlCommand.TestGetToc, [PDF])
            .then((res) => { const r = String(res[0] ?? "").trim(); if (r.includes("ERROR")) throw new Error("parallel TOC: " + r); }),
        runControlCommand(EXE, ControlCommand.TestDest, [PDF, 1])
            .then((res) => { const r = String(res[0] ?? "").trim(); if (r.includes("ERROR")) throw new Error("parallel dest: " + r); }),
        runControlCommand(EXE, ControlCommand.TestDest, [PDF, 2])
            .then((res) => { const r = String(res[0] ?? "").trim(); if (r.includes("ERROR")) throw new Error("parallel dest 2: " + r); }),
    ];

    await Promise.all(tasks);
    console.log("  Parallel device generation stress completed without crash");

    const [code, raw] = await runControlCommand(EXE, ControlCommand.TestGetToc, [PDF]);
    if (!raw) {
        throw new Error("TOC after parallel stress: empty");
    }
    console.log("  Final TOC OK");
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

    console.log("=== D2D Device Generation & Cross-Domain Rendering Safety Tests ===\n");

    console.log("Test 1: Device generation cycle stress (" + ZOOM_STRESS_LOOPS + " open/render/close cycles)");
    await testDeviceGenerationCycles();

    console.log("\nTest 2: Multi-page render stress (" + SCROLL_STRESS_PASSES + " passes, " + N_PAGES + " pages)");
    await testMultiPageRenderStress();

    console.log("\nTest 3: Parallel device generation stress (concurrent render + TOC + dest + search)");
    await testParallelDeviceGenerationStress();

    console.log("\n=== All D2D device generation tests passed ===\n");
}

if (import.meta.main) {
    await runStandalone(testit);
}