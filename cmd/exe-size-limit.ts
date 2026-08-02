// EXE size guardrail for the "lightweight" moat (UI modernization report,
// risk #1: release EXE must stay small). Fails the build if the produced EXE
// grows beyond the configured limit. Pure function, so it can be unit-tested
// headlessly (tests/ad-hoc-exe-size.ts) without invoking MSBuild.

import { statSync } from "node:fs";

export const kDefaultExeSizeLimitMb = 15;

export type ExeSizeCheck = {
  ok: boolean;
  sizeMb: number;
  limitMb: number;
  msg: string;
};

export function enforceExeSizeLimit(exePath: string, limitMb: number): ExeSizeCheck {
  let sizeMb: number;
  try {
    sizeMb = statSync(exePath).size / (1024 * 1024);
  } catch (e) {
    return { ok: false, sizeMb: 0, limitMb, msg: `cannot stat ${exePath}: ${(e as Error)?.message ?? e}` };
  }
  if (sizeMb <= limitMb) {
    return { ok: true, sizeMb, limitMb, msg: `size ${sizeMb.toFixed(1)} MB within limit ${limitMb} MB` };
  }
  return { ok: false, sizeMb, limitMb, msg: `EXE size ${sizeMb.toFixed(1)} MB exceeds limit ${limitMb} MB (${exePath})` };
}
