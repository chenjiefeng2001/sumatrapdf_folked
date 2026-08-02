// Headless test for the EXE-size guardrail (cmd/exe-size-limit.ts). Pure
// function tests — no app binary needed, so run it with --no-build:
//   bun tests/ad-hoc-exe-size.ts --no-build

import { writeFileSync } from "node:fs";
import { join } from "node:path";
import { enforceExeSizeLimit, kDefaultExeSizeLimitMb } from "../cmd/exe-size-limit";
import { runStandalone, tmpPath } from "./util";

export async function testit(): Promise<void> {
  // within limit: a 1 MB file passes the 15 MB default
  const small = tmpPath("exe-size-small.bin");
  writeFileSync(small, Buffer.alloc(1024 * 1024));
  const ok = enforceExeSizeLimit(small, kDefaultExeSizeLimitMb);
  if (!ok.ok) throw new Error(`expected within-limit to pass: ${ok.msg}`);

  // over limit: a 20 MB file must fail a 15 MB limit
  const big = tmpPath("exe-size-big.bin");
  writeFileSync(big, Buffer.alloc(20 * 1024 * 1024));
  const over = enforceExeSizeLimit(big, kDefaultExeSizeLimitMb);
  if (over.ok) throw new Error("expected over-limit to fail");

  // boundary: a file exactly at the limit passes
  const boundary = tmpPath("exe-size-boundary.bin");
  writeFileSync(boundary, Buffer.alloc(15 * 1024 * 1024));
  const at = enforceExeSizeLimit(boundary, 15);
  if (!at.ok) throw new Error(`expected at-limit to pass: ${at.msg}`);

  // a missing file fails loudly (build must not silently skip the check)
  const missing = enforceExeSizeLimit(join(tmpPath("no-such-dir"), "no-such.exe"), 15);
  if (missing.ok) throw new Error("expected missing file to fail");
}

if (import.meta.main) {
  await runStandalone(testit);
}
