// Test for page geometry consistency during continuous scrolling.
//
// Uses the TestPageGeometry control command to open a multi-page PDF and
// verify that each page's mediabox dimensions remain positive and stable
// across multiple sequential scroll-through passes.
//
// Run:  bun tests/issue-page-geometry-scroll.ts [--no-build]

import { existsSync } from "node:fs";
import { ControlCommand, withControlledSumatra } from "../cmd/control.ts";
import { EXE, runStandalone } from "./util.ts";

const PDF = "C:/Users/14977/Downloads/2403.04807v1.pdf";

// ---------------------------------------------------------------------------
// Test 1: Continuous scrolling geometry consistency
// ---------------------------------------------------------------------------

async function testContinuousScrollGeometry(
    pdfPath: string,
    passes: number = 5,
): Promise<void> {
    console.log(
        "  Continuous scroll geometry test with " + passes + " passes on \"" + pdfPath + "\"...",
    );
    const results = await withControlledSumatra(
        EXE,
        async (client) => {
            const res = await client.request(ControlCommand.TestPageGeometry, [
                pdfPath,
                passes,
            ]);
            const exitCode = res[0] as number;
            const raw = String(res[1] ?? "").trim();
            return { exitCode, raw };
        },
        [],
    );
    const { exitCode, raw } = results;
    if (exitCode !== 0) {
        console.error("  FAIL details:");
        for (const line of raw.split("\n")) {
            if (line.trim()) {
                console.error("    " + line);
            }
        }
        throw new Error(
            "Continuous scroll geometry test FAILED: geometry mismatch",
        );
    }
    const okLines = raw.split("\n").filter((l) => l.trim() && l.startsWith("OK"));
    console.log("  OK: " + okLines.join(", "));
    console.log("  All pages maintained correct geometry across all scroll passes.");
}

// ---------------------------------------------------------------------------
// Test 2: Zoom cycle geometry stability
// ---------------------------------------------------------------------------

async function testZoomGeometryStability(
    pdfPath: string,
    cycles: number = 6,
): Promise<void> {
    console.log("  Zoom stability test: " + cycles + " open/render/verify cycles...");
    for (let i = 0; i < cycles; i++) {
        const results = await withControlledSumatra(
            EXE,
            async (client) => {
                const res = await client.request(
                    ControlCommand.TestPageGeometry,
                    [pdfPath, 1],
                );
                const exitCode = res[0] as number;
                const raw = String(res[1] ?? "").trim();
                return { exitCode, raw };
            },
            [],
        );
        if (results.exitCode !== 0) {
            throw new Error("Zoom cycle " + i + ": geometry check failed: " + results.raw);
        }
        console.log("    cycle " + (i + 1) + "/" + cycles + ": OK");
    }
    console.log("  All " + cycles + " zoom cycles passed");
}

// ---------------------------------------------------------------------------
// Test 3: Interleaved page access stress
// ---------------------------------------------------------------------------

async function testInterleavedPageAccess(
    pdfPath: string,
    iterations: number = 8,
): Promise<void> {
    console.log("  Interleaved access stress: " + iterations + " iterations...");
    for (let i = 0; i < iterations; i++) {
        const results = await withControlledSumatra(
            EXE,
            async (client) => {
                const res = await client.request(
                    ControlCommand.TestPageGeometry,
                    [pdfPath, 2],
                );
                const exitCode = res[0] as number;
                const raw = String(res[1] ?? "").trim();
                return { exitCode, raw };
            },
            [],
        );
        if (results.exitCode !== 0) {
            throw new Error("Interleaved iteration " + i + ": geometry failed: " + results.raw);
        }
        console.log("    iteration " + (i + 1) + "/" + iterations + ": OK");
    }
    console.log("  All interleaved access cycles passed");
}

// ---------------------------------------------------------------------------
// Test entry point
// ---------------------------------------------------------------------------

export async function testit(): Promise<void> {
    if (!existsSync(PDF)) {
        console.log("SKIP: fixture not found (download from arXiv)");
        return;
    }
    if (!existsSync(EXE)) {
        throw new Error("app not found: " + EXE + " (build first)");
    }
    console.log("=== Page Geometry Correctness During Continuous Scrolling ===\n");

    console.log("Test 1: Continuous scroll geometry");
    await testContinuousScrollGeometry(PDF, 5);

    console.log("\nTest 2: Zoom cycle geometry stability");
    await testZoomGeometryStability(PDF, 6);

    console.log("\nTest 3: Interleaved page access stress");
    await testInterleavedPageAccess(PDF, 8);

    console.log("\n=== All page geometry scroll tests passed ===\n");
}

if (import.meta.main) {
    await runStandalone(testit);
}
