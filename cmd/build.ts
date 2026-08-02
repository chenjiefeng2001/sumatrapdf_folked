import { join } from "node:path";
import { detectVisualStudio, runLogged } from "./util";
import { clearDirPreserveSettings } from "./clean";
import { enforceExeSizeLimit, kDefaultExeSizeLimitMb } from "./exe-size-limit";

let clean = false;

// Override with env var: CONFIG=Release bun cmd/build.ts, PLATFORM=Win32 etc.
const config = process.env["CONFIG"] ?? "Debug";
const platform = process.env["PLATFORM"] ?? "x64";
// Target: SumatraPDF-dll is the primary; also SumatraPDF for static build.
const target = process.env["TARGET"] ?? "SumatraPDF-dll";

async function main() {
  const timeStart = performance.now();

  console.log(`${config} build (${platform}) for target: ${target}`);
  if (clean) {
    const outDir = join("out", `dbg${platform === "Win32" ? "32" : "64"}`);
    const dirs = [outDir];
    for (const dir of dirs) {
      clearDirPreserveSettings(dir);
    }
  }

  const { msbuildPath } = detectVisualStudio();
  const sln = String.raw`vs2022\SumatraPDF.sln`;
  const t = `/t:${target}`;
  const p = `/p:Configuration=${config};Platform=${platform}`;
  await runLogged(msbuildPath, [sln, t, p, `/m`]);

  // Lightweight-moat guardrail (UI modernization report, risk #1): the release
  // EXE must stay small. Enabled by default for Release builds (<= 15 MB);
  // overridable via ENFORCE_EXE_SIZE_LIMIT_MB (<= 0 disables the check).
  const outDir = join("out", `${config === "Release" ? "rel" : "dbg"}${platform === "Win32" ? "32" : "64"}`);
  const exePath = join(outDir, `${target}.exe`);
  const limitEnv = process.env["ENFORCE_EXE_SIZE_LIMIT_MB"];
  if (config === "Release" || limitEnv) {
    const limitMb = limitEnv ? Number(limitEnv) : kDefaultExeSizeLimitMb;
    if (!Number.isFinite(limitMb) || limitMb <= 0) {
      console.log(`EXE size check disabled (ENFORCE_EXE_SIZE_LIMIT_MB=${limitEnv ?? ""})`);
    } else {
      const r = enforceExeSizeLimit(exePath, limitMb);
      if (!r.ok) {
        throw new Error(`EXE size check failed: ${r.msg}`);
      }
      console.log(`EXE size check: ${r.sizeMb.toFixed(1)} MB <= ${r.limitMb} MB (${exePath})`);
    }
  }

  const elapsed = ((performance.now() - timeStart) / 1000).toFixed(1);
  console.log(`build took ${elapsed}s`);
}

await main();
