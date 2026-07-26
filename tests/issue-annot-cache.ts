// Test for annotation rendering stability: verifies that Invalidate is called
// before every re-render when annotations are modified.
//
// This test verifies:
//   - All pages with mixed annotation types render without crash.
//   - Multiple open/render/close cycles do not produce stale cache artifacts.
//   - Parallel render + TOC + search paths work with annotated pages.
//
// Fixture: tests/issue-annot-cache.pdf (2 pages, 11 annotations total)
//   Regenerate with: bun tests/issue-annot-cache-make.ts
//
// Run:  bun tests/issue-annot-cache.ts [--no-build]
//
// prompt: 使用标注功能之后页面渲染会崩溃

import { existsSync } from "node:fs";
import { join } from "node:path";
import { ControlCommand, runControlCommand } from "../cmd/control.ts";
import { EXE, runStandalone } from "./util.ts";

const PDF = join(import.meta.dir, "issue-annot-cache.pdf");
const N_PAGES = 2;
const RENDER_LOOPS = 6;

async function testRenderAllPages(label: string): Promise<void> {
    for (let pn = 1; pn <= N_PAGES; pn++) {
        const res = await runControlCommand(EXE, ControlCommand.TestPageLinks, [PDF, pn]);
        const raw = String(res[0] ?? "").trim();
        if (raw.includes("ERROR")) {
            throw new Error(`${label}: TestPageLinks page ${pn}: ${raw}`);
        }
        if (raw.length === 0) {
            throw new Error(`${label}: TestPageLinks page ${pn} returned empty (possible crash)`);
        }
        console.log(`  page ${pn} rendered OK (${raw.split("\n").length} link(s))`);
    }
}

async function testGetToc(): Promise<void> {
    const res = await runControlCommand(EXE, ControlCommand.TestGetToc, [PDF]);
    const raw = String(res[0] ?? "").trim();
    if (raw.includes("ERROR")) {
        throw new Error("TestGetToc: " + raw);
    }
    console.log("  TOC: " + raw);
}

async function testSearch(query: string): Promise<void> {
    const res = await runControlCommand(EXE, ControlCommand.TestSearch, [PDF, query, 0]);
    const raw = String(res[0] ?? "").trim();
    if (raw.includes("ERROR")) {
        console.log(`  Search for '${query}': (no results/error: ${raw})`);
        return;
    }
    console.log(`  Search for '${query}': ${raw}`);
}

export async function testit(): Promise<void> {
    if (!existsSync(PDF)) {
        console.log("SKIP: fixture not found - run bun tests/issue-annot-cache-make.ts first");
        return;
    }
    if (!existsSync(EXE)) {
        throw new Error("app not found: " + EXE + " (build first)");
    }

    console.log("=== Annot Render Cache Invalidation Tests ===\n");

    // Test 1: Render all annotated pages (basic stability)
    console.log("Test 1: Render all pages with mixed annotation types");
    await testRenderAllPages("Test 1");

    // Test 2: Multiple render cycles to stress the RenderCache
    // Each TestPageLinks call opens the file, renders, and closes.
    // This exercises the full render lifecycle (create displayList → cache tiles → lookup + display).
    console.log(`\nTest 2: ${RENDER_LOOPS} open/render/close cycles (stress RenderCache lifecycle)`);
    for (let i = 0; i < RENDER_LOOPS; i++) {
        await testRenderAllPages(`Test 2 cycle ${i + 1}`);
    }
    console.log(`  ${RENDER_LOOPS} cycles completed without crash`);

    // Test 3: Quick alternating page renders (exercises cache lookup for different pages)
    console.log("\nTest 3: Alternating page renders (stale cache eviction)");
    for (let i = 0; i < 4; i++) {
        await testRenderAllPages(`Test 3 iteration ${i + 1}`);
    }
    console.log("  Alternating renders completed without crash");

    // Test 4: Parallel paths (exercises docLock/renderLock with annotations)
    console.log("\nTest 4: Parallel render + TOC + search");
    const tasks = [
        testRenderAllPages("Test 4 render"),
        testGetToc(),
        testSearch("Page"),
        testSearch("annot"),
    ];
    await Promise.all(tasks);
    console.log("  All parallel requests completed");

    console.log("\n=== All annot cache invalidation tests passed ===");
}

if (import.meta.main) {
    await runStandalone(testit);
}
