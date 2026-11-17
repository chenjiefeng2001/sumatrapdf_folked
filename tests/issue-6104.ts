// PDF portfolio support: a PDF whose catalog has /Root/Collection is a bundle
// of embedded files rather than a document to read. Acrobat shows a file list;
// we list the embedded files in the sidebar, in the order /Collection/D gives,
// with each file's /Desc.
//
// What this checks:
//   - the document is recognised as a portfolio
//   - the files come out in /Collection/D order, not the name tree's order
//     (the two fixtures have the same /Names/EmbeddedFiles array and different
//     /Collection/D, so a swapped implementation fails one of them)
//   - each row carries its /Desc, and loads the bytes belonging to *that* file
//   - activating a row opens the embedded file, which used to do nothing
//     (LinkHandler::GotoLink returns early for an attachment)
//
// The sidebar caption and cue text are also portfolio-dependent, but they are
// VirtText/Edit children with no HWND, so they are not checked here.
//
// Fixtures are generated into tests/tmp/ on the run. Run:
//   bun tests/issue-6104.ts

import { mkdirSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { ControlCommand } from "./control.ts";
import { tmpPath } from "./util.ts";
import { launchControlled, killAndWait } from "./win-automation.ts";

function buildPdf(objs: string[]): Buffer {
  let out = "%PDF-1.7\n%\xE2\xE3\xCF\xD3\n";
  const offsets: number[] = [];
  for (let i = 0; i < objs.length; i++) {
    offsets.push(out.length);
    out += `${i + 1} 0 obj\n${objs[i]}\nendobj\n`;
  }
  const xrefStart = out.length;
  out += `xref\n0 ${objs.length + 1}\n0000000000 65535 f \n`;
  for (const off of offsets) out += `${off.toString().padStart(10, "0")} 00000 n \n`;
  out += `trailer\n<< /Size ${objs.length + 1} /Root 1 0 R >>\nstartxref\n${xrefStart}\n%%EOF\n`;
  return Buffer.from(out, "latin1");
}

// an embedded file stream needs /Params /Size, which is how the enumeration
// recognises a real embedded file
function embeddedStream(s: string): string {
  return `<< /Length ${s.length} /Params << /Size ${s.length} >> >>\nstream\n${s}endstream`;
}

const cover = "BT /F1 18 Tf 40 140 Td (PORTFOLIO COVER) Tj ET\n";

// a complete little PDF to embed, so opening the attachment really opens
// something (an xref-less one would just fail to load)
const innerText = "BT /F1 18 Tf 20 60 Td (INNER README) Tj ET\n";
const innerPdf = buildPdf([
  `<< /Type /Catalog /Pages 2 0 R >>`,
  `<< /Type /Pages /Kids [3 0 R] /Count 1 >>`,
  `<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 100] /Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R >>`,
  `<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>`,
  embeddedStream(innerText),
]).toString("latin1");

const NAMES_TREE = `<< /Names [(readme.pdf) 11 0 R (zebra.txt) 12 0 R (alpha.txt) 13 0 R] >>`;
const FS = [
  `<< /Type /Filespec /F (readme.pdf) /UF (readme.pdf) /Desc (The readme) /EF << /F 6 0 R >> >>`,
  `<< /Type /Filespec /F (zebra.txt) /UF (zebra.txt) /Desc (Zebra) /EF << /F 7 0 R >> >>`,
  `<< /Type /Filespec /F (alpha.txt) /UF (alpha.txt) /Desc (Alpha) /EF << /F 8 0 R >> >>`,
];

function portfolio(collectionOrder: string, withCollection: boolean, efForReadme: string): Buffer {
  const catalog = withCollection
    ? `<< /Type /Catalog /Pages 2 0 R /Collection 10 0 R /Names << /EmbeddedFiles 9 0 R >> >>`
    : `<< /Type /Catalog /Pages 2 0 R /Names << /EmbeddedFiles 9 0 R >> >>`;
  return buildPdf([
    catalog,
    `<< /Type /Pages /Kids [3 0 R] /Count 1 >>`,
    `<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 200] /Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R >>`,
    `<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>`,
    embeddedStream(cover),
    embeddedStream(efForReadme),
    embeddedStream("ZEBRA-CONTENT\n"),
    embeddedStream("ALPHA-CONTENT\n"),
    NAMES_TREE,
    `<< /Type /Collection /D [${collectionOrder}] >>`,
    ...FS,
  ]);
}

function writeFixtures(dir: string): { portfolio: string; plain: string; pdfFirst: string } {
  mkdirSync(dir, { recursive: true });
  // /Collection/D is alpha, readme, zebra - deliberately not the name tree's
  // readme, zebra, alpha
  const p = join(dir, "portfolio.pdf");
  writeFileSync(p, portfolio("13 0 R 11 0 R 12 0 R", true, "README-CONTENT\n"));
  // same name tree, no /Collection: order must come from the name tree
  const plain = join(dir, "notaportfolio.pdf");
  writeFileSync(plain, portfolio("13 0 R 11 0 R 12 0 R", false, "README-CONTENT\n"));
  // readme.pdf first, and holding a real PDF, so row 1 can be opened
  const pdfFirst = join(dir, "pdf-first.pdf");
  writeFileSync(pdfFirst, portfolio("11 0 R 12 0 R 13 0 R", true, innerPdf));
  return { portfolio: p, plain, pdfFirst };
}

type Row = { name: string; desc: string; bytes: number; first: string };

function parseListing(raw: string): { isPortfolio: boolean; rows: Row[] } {
  const isPortfolio = /isPortfolio=1/.test(raw);
  const rows: Row[] = [];
  // descriptions may contain spaces, so match up to the " bytes=" marker
  const re = /name=(\S*) desc=(.*?) bytes=(\d+) first=(\S*)/g;
  for (const m of raw.matchAll(re)) {
    rows.push({ name: m[1], desc: m[2], bytes: Number(m[3]), first: m[4] });
  }
  return { isPortfolio, rows };
}

export async function testit(): Promise<void> {
  const dir = tmpPath("issue-6104");
  const { portfolio: pf, plain, pdfFirst } = writeFixtures(dir);

  // --- a portfolio lists its files in /Collection/D order, with descriptions
  {
    const { proc, client } = await launchControlled([pf]);
    try {
      await client.setNotificationsEnabled(false);
      await client.waitForRenderIdle();
      const res = await client.request(ControlCommand.TestPortfolio, []);
      const raw = String(res[1] ?? "");
      console.log(raw.trim());
      const { isPortfolio, rows } = parseListing(raw);
      if (!isPortfolio) {
        throw new Error("portfolio.pdf was not recognised as a portfolio");
      }
      const want = ["alpha.txt", "readme.pdf", "zebra.txt"];
      const got = rows.map((r) => r.name);
      if (JSON.stringify(got) !== JSON.stringify(want)) {
        throw new Error(`portfolio order wrong: got ${got.join(", ")}, want ${want.join(", ")}`);
      }
      const wantContent: Record<string, string> = {
        "alpha.txt": "ALPHA-CONTENT",
        "readme.pdf": "README-CONTENT",
        "zebra.txt": "ZEBRA-CONTENT",
      };
      for (const r of rows) {
        if (!r.desc) {
          throw new Error(`${r.name} has no description`);
        }
        if (r.bytes <= 0) {
          throw new Error(`${r.name} loaded no bytes`);
        }
        if (r.first !== wantContent[r.name]) {
          throw new Error(`${r.name} loaded the wrong bytes: got ${r.first}`);
        }
      }
    } finally {
      client.close();
      await killAndWait(proc);
    }
  }

  // --- without /Collection the order comes from the name tree instead
  {
    const { proc, client } = await launchControlled([plain]);
    try {
      await client.setNotificationsEnabled(false);
      await client.waitForRenderIdle();
      const res = await client.request(ControlCommand.TestPortfolio, []);
      const raw = String(res[1] ?? "");
      const { isPortfolio, rows } = parseListing(raw);
      if (isPortfolio) {
        throw new Error("a PDF with no /Collection was reported as a portfolio");
      }
      const want = ["readme.pdf", "zebra.txt", "alpha.txt"];
      const got = rows.map((r) => r.name);
      if (JSON.stringify(got) !== JSON.stringify(want)) {
        throw new Error(`plain embedded files out of order: got ${got.join(", ")}, want ${want.join(", ")}`);
      }
    } finally {
      client.close();
      await killAndWait(proc);
    }
  }

  // --- activating a row opens the embedded file
  {
    const { proc, client } = await launchControlled([pdfFirst]);
    try {
      await client.setNotificationsEnabled(false);
      await client.waitForRenderIdle();
      const before = String((await client.request(ControlCommand.TestPortfolio, []))[1] ?? "");
      if (!/nItems=3/.test(before)) {
        throw new Error(`fixture did not list 3 files:\n${before}`);
      }
      const missing = await client.request(ControlCommand.TestOpenTocItem, [99]);
      if (!/ERROR no-item/.test(String(missing[1] ?? ""))) {
        throw new Error(`activating a nonexistent item should fail: ${String(missing[1] ?? "")}`);
      }
      const act = await client.request(ControlCommand.TestOpenTocItem, [1]);
      const actRaw = String(act[1] ?? "").trim();
      console.log(actRaw);
      if (!/activated=readme\.pdf/.test(actRaw)) {
        throw new Error(`activating row 1 should be readme.pdf: ${actRaw}`);
      }
      // opening the embedded file swaps the document in the current tab, so
      // the attachment list becomes the inner PDF's, which has none
      const after = String((await client.request(ControlCommand.TestPortfolio, []))[1] ?? "");
      if (!/nItems=0/.test(after)) {
        throw new Error(`activating readme.pdf did not open it (still showing the portfolio)\n${after}`);
      }
    } finally {
      client.close();
      await killAndWait(proc);
    }
  }

  console.log("issue-6104 ok");
}

if (import.meta.main) {
  await (await import("./util.ts")).runStandalone(testit);
}
