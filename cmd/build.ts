import { join } from "node:path";
import { detectVisualStudio, runLogged } from "./util";
import { clearDirPreserveSettings } from "./clean";

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

  const elapsed = ((performance.now() - timeStart) / 1000).toFixed(1);
  console.log(`build took ${elapsed}s`);
}

await main();
