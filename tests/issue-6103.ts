// Attaching a file to a PDF (CmdCreateAnnotFileAttachment).
//
// The command asks for a path with the file picker, which a test can't drive,
// so the app exposes AttachFileResultTemp / AttachFileSaveResultTemp over
// -dbg-control. Those call the same EngineMupdfCreateAnnotation() the command
// does, so what is checked here is the real embed.
//
// What matters:
//   - the file's bytes land in the document and come back out of the
//     annotation's /FS unchanged (not just the right length)
//   - the annotation is an undoable edit, and attaching a file does not
//     break undo for the document afterwards (mupdf's journal is involved,
//     and pdf_add_embedded_file() used to leave its operation open, which
//     made every later pdf_can_undo() fail)
//   - the embed survives save + reopen, i.e. it is really in the file
//   - a missing file fails cleanly instead of leaving an empty annotation
//
// Run: bun tests/issue-6103.ts

import { copyFileSync, existsSync, rmSync, writeFileSync, statSync } from "node:fs";
import { join } from "node:path";
import { ControlCommand } from "./control.ts";
import { tmpPath } from "./util.ts";
import { launchControlled, killAndWait } from "./win-automation.ts";

const SRC_PDF = join(import.meta.dir, "issue-4157.pdf");

// a payload with a newline and non-ascii, so a length-only check or a
// text-mode read would not pass
const PAYLOAD = "ATTACHED-FILE-CONTENT-1234567890\nsecond line, \u00e4\u00f6\u00fc\n";

function hex(s: string): string {
  return Array.from(Buffer.from(s, "latin1"))
    .map((b) => b.toString(16).padStart(2, "0"))
    .join("");
}

export async function testit(): Promise<void> {
  if (!existsSync(SRC_PDF)) {
    throw new Error(`fixture missing: ${SRC_PDF}`);
  }

  const attachPath = tmpPath("issue-6103-attach.txt");
  writeFileSync(attachPath, PAYLOAD, "latin1");
  const attachSize = statSync(attachPath).size;

  // work on a copy: never dirty a checked-in fixture
  const doc = tmpPath("issue-6103-e2e.pdf");
  rmSync(doc, { force: true });
  copyFileSync(SRC_PDF, doc);
  if (!existsSync(doc)) {
    throw new Error(`could not copy the fixture to ${doc}`);
  }

  const saved = tmpPath("issue-6103-saved.pdf");
  rmSync(saved, { force: true });

  const { proc, client } = await launchControlled([doc]);
  try {
    await client.setNotificationsEnabled(false);
    await client.waitForRenderIdle();

    const res = await client.request(ControlCommand.TestAttachFile, [attachPath]);
    const full = String(res[1] ?? "").trim();
    console.log(full.replace(/\n/g, " | "));

    // a control Text annotation is created first and shares this response, so
    // a plain annotation registering proves the reporting path itself works
    if (!/control: nAnnots=1 canUndo=1/.test(full)) {
      throw new Error(`control annotation did not register:\n${full}`);
    }

    const ok = /^OK annot=(.*)$/m.exec(full)?.[1] ?? "";
    if (!ok) {
      throw new Error(`attach did not produce an annotation:\n${full}`);
    }
    if (!/^File Attachment\b/.test(ok)) {
      throw new Error(`expected a File Attachment annotation, got: ${ok}`);
    }
    if (!/fs=ok/.test(ok)) {
      throw new Error(`annotation has no usable /FS: ${ok}`);
    }
    // anchored where we asked, at a sane icon size
    const r = /rect=72,100,([\d.]+),([\d.]+)/.exec(ok);
    if (!r) {
      throw new Error(`annotation is not anchored at the click point: ${ok}`);
    }
    const [w, h] = [Number(r[1]), Number(r[2])];
    if (!(w > 5 && w < 40) || !(h > 5 && h < 40)) {
      throw new Error(`paperclip is not a sane icon size: ${ok}`);
    }
    // attaching must leave undo working on this document
    if (!/canUndo=1/.test(ok)) {
      throw new Error(`attaching a file broke undo: ${ok}`);
    }

    // the bytes must come back out of the document unchanged, not merely be
    // the right length - this is the round-trip a reader of the file does
    const got = /size=(\d+) bytes=([0-9a-f]*)/.exec(full);
    if (!got) {
      throw new Error(`could not parse the embedded bytes:\n${full}`);
    }
    if (Number(got[1]) !== attachSize) {
      throw new Error(`embedded size ${got[1]} != source size ${attachSize}`);
    }
    if (got[2] !== hex(PAYLOAD)) {
      throw new Error(`embedded bytes differ\n  got  ${got[2]}\n  want ${hex(PAYLOAD)}`);
    }

    // a missing file must fail cleanly rather than leave an empty annotation
    const bad = await client.request(ControlCommand.TestAttachFile, [tmpPath("issue-6103-nope.txt")]);
    const badRaw = String(bad[1] ?? "").trim();
    if (!/ERROR no-such-file/.test(badRaw)) {
      throw new Error(`expected a clean error for a missing file, got:\n${badRaw}`);
    }

    // and the embed has to be in the written file, not just in memory
    const sv = await client.request(ControlCommand.TestAttachFileSave, [saved]);
    const svRaw = String(sv[1] ?? "").trim();
    console.log(svRaw);
    if (!new RegExp(`OK nAnnots=\\d+ fs=1 size=${attachSize}\\b`).test(svRaw)) {
      throw new Error(`attachment did not survive save/reload:\n${svRaw}`);
    }
    if (!existsSync(saved)) {
      throw new Error(`saved file missing: ${saved}`);
    }
  } finally {
    client.close();
    await killAndWait(proc);
  }

  console.log("issue-6103 ok");
}

if (import.meta.main) {
  await (await import("./util.ts")).runStandalone(testit);
}
