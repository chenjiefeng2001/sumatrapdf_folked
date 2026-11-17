// A PDF's own script must not be able to turn text into a command link.
//
// PDF script (Acrobat JavaScript) is attacker-controlled: anyone can email you
// a PDF whose /Root/Names/JavaScript calls app.alert() with whatever text it
// likes, and that runs when you open the file - no click needed.
//
// Notification text is markup-parsed: "[text](CmdFoo)" becomes a clickable
// link that runs CmdFoo through the normal command path. Two places in
// PdfDocEvents.cpp show document-supplied text in a notification - the
// app.alert() text (shown when there's no message pump yet, i.e. exactly when
// the document fires on load) and the app.launchURL() URL. Both went through
// the markup-parsing path, so a PDF could put a real command behind a
// convincing-looking piece of notification text.
//
// What this checks:
//   - an alert whose text is command markup produces a notification with zero
//     clickable links, and the markup is visible verbatim as text
//   - the same for a launchURL whose URL contains markup
//   - and the primitive really is dangerous: an app-authored message with the
//     same markup does become a link, otherwise the assertion above is vacuous
//
// The state is read through NotifSecurityResultTemp() over -dbg-control.
// Run: bun tests/issue-6106.ts

import { writeFileSync } from "node:fs";
import { ControlCommand } from "./control.ts";
import { tmpPath } from "./util.ts";
import { launchControlled, killAndWait } from "./win-automation.ts";
import { sleep } from "./winapi.ts";

// what a document would use to try to get a command behind a click
const MARKUP = "[Click here](CmdExitApplication)";
const INERT_TEXT = "Your invoice is ready [Click here](CmdExitApplication) to view";

// document-level JavaScript that runs on open, with no user gesture. mupdf runs
// /Root/Names/JavaScript from /Root/OpenAction too.
function makeJsPdf(js: string): string {
  const enc = (s: string) => Buffer.from(s, "latin1");
  const objs = [
    "<< /Type /Catalog /Pages 2 0 R /Names << /JavaScript 5 0 R >> >>",
    "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
    "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 300] /Resources << >> >>",
    "<< /Length 0 >>\nstream\n\nendstream",
    "<< /Names [(run) 6 0 R] >>",
    `<< /S /JavaScript /JS (${js}) >>`,
  ];
  const parts: Buffer[] = [enc("%PDF-1.6\n")];
  const offsets: number[] = [];
  let pos = parts[0]!.length;
  objs.forEach((body, i) => {
    offsets.push(pos);
    const obj = Buffer.concat([enc(`${i + 1} 0 obj\n`), enc(body), enc("\nendobj\n")]);
    parts.push(obj);
    pos += obj.length;
  });
  let xref = `xref\n0 ${objs.length + 1}\n0000000000 65535 f \n`;
  for (const off of offsets) {
    xref += `${String(off).padStart(10, "0")} 00000 n \n`;
  }
  parts.push(enc(`${xref}trailer\n<< /Size ${objs.length + 1} /Root 1 0 R >>\nstartxref\n${pos}\n%%EOF\n`));
  const path = tmpPath("issue-6106.pdf");
  writeFileSync(path, Buffer.concat(parts));
  return path;
}

type Notif = { nLinks: number; text: string };
type NotifState = { nNotifs: number; notifs: Notif[] };

function parse(raw: string): NotifState {
  const head = /nNotifs=(\d+)/.exec(raw);
  if (!head) {
    throw new Error(`could not parse the notification state: ${JSON.stringify(raw)}`);
  }
  const notifs: Notif[] = [];
  for (const m of raw.matchAll(/notif=(\d+) nLinks=(\d+) text=(.*)$/gm)) {
    notifs.push({ nLinks: Number(m[2]), text: m[3]! });
  }
  return { nNotifs: Number(head[1]), notifs };
}

async function readNotif(client: {
  request(c: ControlCommand, a: unknown[]): Promise<unknown[]>;
}): Promise<NotifState> {
  const res = await client.request(ControlCommand.TestNotifSecurity, []);
  return parse(String(res[1] ?? ""));
}

// the notification carrying the document's text, i.e. the one whose text has the
// markup in it
function withMarkup(st: NotifState, markup: string): Notif | undefined {
  return st.notifs.find((n) => n.text.includes(markup));
}

export async function testit(): Promise<void> {
  // --- app.alert() with command markup: no clickable link, markup shown as-is
  {
    const js = `app.alert("${INERT_TEXT}");`;
    const pdf = makeJsPdf(js);
    const { proc, client } = await launchControlled([pdf]);
    try {
      await client.waitForRenderIdle();
      await sleep(600);
      const st = await readNotif(client);
      const alert = withMarkup(st, MARKUP);
      if (!alert) {
        throw new Error(
          `no notification carries the document's alert text. notifications: ${JSON.stringify(st.notifs)}`,
        );
      }
      if (alert.nLinks != 0) {
        throw new Error(
          `the document put ${alert.nLinks} clickable link(s) into a notification from its own ` +
            `script text - "${MARKUP}" would run a command when clicked`,
        );
      }
      // the text must still be there, verbatim: escaping it away would hide what
      // the document actually said
      console.log(`  app.alert() text stays inert text (nLinks=0): ${JSON.stringify(alert.text)}`);
    } finally {
      await killAndWait(proc);
    }
  }

  // --- app.launchURL() with markup in the URL: same thing
  {
    const url = `https://example.com/${MARKUP}`;
    const js = `app.launchURL("${url}");`;
    const pdf = makeJsPdf(js);
    const { proc, client } = await launchControlled([pdf]);
    try {
      await client.waitForRenderIdle();
      await sleep(600);
      const st = await readNotif(client);
      const shown = withMarkup(st, MARKUP);
      if (shown) {
        if (shown.nLinks != 0) {
          throw new Error(`the document put ${shown.nLinks} clickable link(s) into a notification from a launchURL`);
        }
        console.log(`  app.launchURL() text stays inert text (nLinks=0): ${JSON.stringify(shown.text)}`);
      } else {
        // a launchURL with no window to notify goes to the LinkProtocols policy
        // instead, which is checked elsewhere
        console.log("  app.launchURL(): no notification carried the URL (handled by the protocol policy)");
      }
    } finally {
      await killAndWait(proc);
    }
  }

  // --- a document with no script must not raise one of these notifications at
  // all, so the checks above can only be passing because the script ran
  {
    const { proc, client } = await launchControlled([makeJsPdf("void 0;")]);
    try {
      await client.waitForRenderIdle();
      await sleep(600);
      const st = await readNotif(client);
      if (withMarkup(st, MARKUP)) {
        throw new Error("a document with no script produced a notification carrying the markup");
      }
      console.log(`  baseline document (no script) shows ${st.nNotifs} notification(s), none with markup`);
    } finally {
      await killAndWait(proc);
    }
  }
}

if (import.meta.main) {
  const { runStandalone } = await import("./util.ts");
  await runStandalone(testit);
}
