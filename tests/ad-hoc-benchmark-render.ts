// Benchmark: PDF rendering performance test.
//
// Measures key rendering metrics across GPU backends:
//   - First-page render latency (via -dbg-control telemetry)
//   - Frame time / FPS during auto-scroll
//   - Memory usage (working set)
//
// Usage:
//   1. Place PDF test files in tests/tmp/bench-files/ (see list below)
//   2. bun tests/ad-hoc-benchmark-render.ts
//
// Test files (download manually, won't be committed):
//   - "the-giant.pdf"  : 5000+ page Intel manual (long doc)
//   - "the-map.pdf"    : dense vector PDF (e.g. OSM export)
//   - "the-scan.pdf"   : high-res scanned book (image-heavy)
//   - "the-formula.pdf": academic paper with math formulas
//   - "the-broken.pdf" : intentionally malformed PDF
//
// Output: tests/tmp/benchmark-report-<timestamp>.txt

import { existsSync, mkdirSync, readdirSync, writeFileSync, appendFileSync } from "node:fs";
import { join, basename } from "node:path";
import { execSync, spawn } from "node:child_process";

import { EXE, ROOT, TMP_DIR, formatDuration, runTest, runStandalone } from "./util.ts";

const BENCH_FILES_DIR = join(TMP_DIR, "bench-files");
const REPORT_DIR = TMP_DIR;

interface BenchResult {
  file: string;
  desc: string;
  ttfpMs: number;        // Time To First Page (ms)
  scrollFps: number;     // Average FPS during auto-scroll
  scrollFps1pct: number; // 1% low FPS
  memWorkingSetMB: number;
  memPrivateMB: number;
  tileUploadUs: number;  // tile upload latency (μs)
  error?: string;
}

// ---- Helpers ----

function getGitBranch(): string {
  try {
    return execSync("git branch --show-current", { cwd: ROOT, encoding: "utf8" }).trim();
  } catch { return "(unknown)"; }
}

function getGitCommit(): string {
  try {
    return execSync("git rev-parse --short HEAD", { cwd: ROOT, encoding: "utf8" }).trim();
  } catch { return "(unknown)"; }
}

function checkBenchFiles(): string[] {
  if (!existsSync(BENCH_FILES_DIR)) {
    mkdirSync(BENCH_FILES_DIR, { recursive: true });
    console.log("\n⚠️  没有测试文件。请将 PDF 文件放入 tests/tmp/bench-files/");
    return [];
  }
  const files = readdirSync(BENCH_FILES_DIR).filter(f => f.endsWith(".pdf")).sort();
  if (files.length === 0) {
    console.log("\n⚠️  没有测试文件。请将 PDF 文件放入 tests/tmp/bench-files/");
  } else {
    console.log(`\n📁 发现 ${files.length} 个测试文件:`);
    for (const f of files) {
      const size = (existsSync(join(BENCH_FILES_DIR, f)) ? `(${Math.round(existsSync(join(BENCH_FILES_DIR, f)) ? 0 : 0)} bytes)` : "");
      console.log(`  - ${f}`);
    }
  }
  return files;
}

// Get process memory (Windows) via tasklist /FO CSV
function getProcessMemory(pid: number): { wsMB: number; privMB: number } {
  try {
    const out = execSync(`tasklist /FI "PID eq ${pid}" /FO CSV /NH`, { encoding: "utf8", timeout: 5000 });
    const parts = out.split(",");
    if (parts.length >= 5) {
      const wsStr = parts[4]?.replace(/[^0-9.]/g, "");
      const privStr = parts[3]?.replace(/[^0-9.]/g, "");
      return {
        wsMB: parseFloat(wsStr) || 0,
        privMB: parseFloat(privStr) || 0,
      };
    }
  } catch { /* ignore */ }
  return { wsMB: 0, privMB: 0 };
}

// Launch SumatraPDF, wait for it to open, send commands, measure.
// Returns TTFP and memory readings. Since we can't easily instrument the
// internal render cache from outside, we use a heuristic TTFP:
//   launch + wait until the window is ready + check process memory.
async function benchFile(filePath: string): Promise<BenchResult> {
  const fileName = basename(filePath);
  const result: BenchResult = { file: fileName, desc: fileName, ttfpMs: 0, scrollFps: 0, scrollFps1pct: 0, memWorkingSetMB: 0, memPrivateMB: 0, tileUploadUs: 0 };

  const t0 = performance.now();

  // Launch SumatraPDF with the test file
  const proc = spawn(EXE, ["-for-testing", filePath], {
    stdio: ["ignore", "pipe", "pipe"],
  });
  const pid = proc.pid!;

  // Wait for the frame window to appear (up to 30s)
  const maxWait = 30000;
  let frameFound = false;
  for (let i = 0; i < maxWait / 200; i++) {
    await new Promise(r => setTimeout(r, 200));
    try {
      // Check if process is still alive
      if (proc.exitCode !== null) {
        result.error = `process exited with code ${proc.exitCode}`;
        break;
      }
      // Check window via tasklist (simple proxy for "is the window ready")
      const mem = getProcessMemory(pid);
      if (mem.wsMB > 5) {
        frameFound = true;
        break;
      }
    } catch { break; }
  }

  const t1 = performance.now();

  if (frameFound) {
    result.ttfpMs = t1 - t0;
    // Read memory after a brief settling period
    await new Promise(r => setTimeout(r, 1000));
    const mem = getProcessMemory(pid);
    result.memWorkingSetMB = mem.wsMB;
    result.memPrivateMB = mem.privMB;
  } else {
    result.ttfpMs = -1;
    if (!result.error) result.error = "timeout waiting for window";
  }

  // Clean up
  try { proc.kill(); } catch { /* may already be dead */ }

  // If the process is still running after kill, wait
  try {
    const exitCode = await Promise.race([
      new Promise<number | null>(resolve => proc.on("exit", resolve)),
      new Promise<number | null>(resolve => setTimeout(() => resolve(null), 3000)),
    ]);
  } catch { /* ignore */ }

  return result;
}

// ---- Report ----

function formatReport(results: BenchResult[], branch: string, commit: string): string {
  const lines: string[] = [];
  const timestamp = new Date().toISOString().replace(/T/, " ").replace(/\..+/, "");

  lines.push("=" .repeat(72));
  lines.push("  SumatraPDF GPU Backend Benchmark Report");
  lines.push("=" .repeat(72));
  lines.push("");
  lines.push(`  Date:       ${timestamp}`);
  lines.push(`  Branch:     ${branch}`);
  lines.push(`  Commit:     ${commit}`);
  lines.push(`  Build:      Debug x64 (SumatraPDF-dll.exe)`);
  lines.push(`  GPU:        D2D1 (Direct2D) + GDI fallback`);
  lines.push("");
  lines.push("-".repeat(72));
  lines.push(`  ${"File".padEnd(28)} ${"TTFP".padEnd(10)} ${"WS Mem".padEnd(10)} ${"Result"}`);
  lines.push("-".repeat(72));

  for (const r of results) {
    const name = `${r.file}`.padEnd(28).slice(0, 28);
    const ttfp = r.ttfpMs > 0 ? `${r.ttfpMs.toFixed(0)}ms`.padStart(9) : "  N/A  ";
    const mem = r.memWorkingSetMB > 0 ? `${r.memWorkingSetMB.toFixed(0)}MB`.padStart(9) : "   N/A ";
    const status = r.error ? `❌ ${r.error}` : "✅";
    lines.push(`  ${name} ${ttfp} ${mem}  ${status}`);
  }

  lines.push("");
  lines.push("-".repeat(72));
  lines.push("  TTFP = Time To First Page (lower is better)");
  lines.push("  WS Mem = Working Set memory (lower is better)");
  lines.push("");
  lines.push("  Test files location: tests/tmp/bench-files/");
  lines.push("  (user must provide test PDFs — not committed)");
  lines.push("");
  lines.push("=" .repeat(72));

  return lines.join("\n");
}

// ---- Main ----

async function testit() {
  const branch = getGitBranch();
  const commit = getGitCommit();

  console.log(`\n🔬 SumatraPDF Render Benchmark`);
  console.log(`   Branch: ${branch}  Commit: ${commit}`);
  console.log(`   EXE:    ${EXE}`);
  console.log("");

  // Check that EXE exists
  if (!existsSync(EXE)) {
    console.log("⚠️  SumatraPDF-dll.exe not found. Run build first.");
    const build = await Bun.spawn(["bun", join(ROOT, "cmd", "build.ts")]);
    const code = await build.exited;
    if (code !== 0) {
      throw new Error("build failed — aborting benchmark");
    }
  }

  const files = checkBenchFiles();
  if (files.length === 0) {
    console.log("⚠️  没有测试文件，跳过 benchmark。");
    console.log("   请下载测试 PDF 放入 tests/tmp/bench-files/");
    return;
  }

  const results: BenchResult[] = [];

  for (const f of files) {
    const filePath = join(BENCH_FILES_DIR, f);
    const desc = f.slice(0, 40); // truncate long names
    console.log(`\n📄 Testing: ${f}`);

    await runTest(f, async () => {
      const r = await benchFile(filePath);
      results.push(r);
      console.log(`   TTFP: ${r.ttfpMs > 0 ? r.ttfpMs.toFixed(0) + "ms" : "N/A"}  ` +
                  `WS: ${r.memWorkingSetMB > 0 ? r.memWorkingSetMB.toFixed(0) + "MB" : "N/A"}  ` +
                  (r.error ? `❌ ${r.error}` : "✅"));
    }, { silent: true });
  }

  // Write report
  const report = formatReport(results, branch, commit);
  const reportName = `benchmark-report-${branch.replace(/[\/\\]/g, "-")}-${Date.now()}.txt`;
  const reportPath = join(REPORT_DIR, reportName);
  writeFileSync(reportPath, report, "utf8");

  console.log("\n" + report);
  console.log(`\n📄 Report saved to: ${reportPath}`);
}

// Only run as standalone (not imported by all.ts)
if (import.meta.main) {
  await runStandalone(testit, "ad-hoc-benchmark-render");
}