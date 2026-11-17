// Extracted text of a *tagged* PDF must follow the structure tree's reading
// order, not the order the content stream happens to draw the text in. This is
// what makes copy/paste, "Extract Text From Document", Read Aloud and search
// results come out in the order the document is meant to be read (single
// column reflow, multi-column papers, slides with notes).
//
// An *untagged* PDF has no reading order to follow, so its text must stay in
// drawing order - the DFS walk has to be a no-op there.
//
// Fixtures are generated into tests/tmp/ on the run (nothing to keep in the
// repo). Run: bun tests/ad-hoc-reading-order.ts
//
// Fixture: one page whose content stream draws "SECOND-PARA" first (top) and
// "FIRST-PARA" second, while the structure tree lists MCID 0 (FIRST-PARA)
// before MCID 1 (SECOND-PARA). Reading order is therefore FIRST, SECOND.

import { mkdirSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { extractPageText, tmpPath } from "./util.ts";

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

// drawn top-down, so the second paragraph of the reading order comes first
const marked = `/P << /MCID 1 >> BDC
BT /F1 18 Tf 40 140 Td (SECOND-PARA) Tj ET
EMC
/P << /MCID 0 >> BDC
BT /F1 18 Tf 40 60 Td (FIRST-PARA) Tj ET
EMC
`;

const plain = `BT /F1 18 Tf 40 140 Td (SECOND-PARA) Tj ET
BT /F1 18 Tf 40 60 Td (FIRST-PARA) Tj ET
`;

function writeFixtures(dir: string): { tagged: string; untagged: string } {
  mkdirSync(dir, { recursive: true });
  // 1 catalog 2 pages 3 page 4 font 5 contents 6 document 7 para(mcid 0)
  // 8 structtreeroot 9 para(mcid 1) 10 parenttree
  const tagged = join(dir, "tagged.pdf");
  writeFileSync(
    tagged,
    buildPdf([
      `<< /Type /Catalog /Pages 2 0 R /StructTreeRoot 8 0 R /MarkInfo << /Marked true >> >>`,
      `<< /Type /Pages /Kids [3 0 R] /Count 1 >>`,
      `<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 200] /StructParents 0 /Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R >>`,
      `<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>`,
      `<< /Length ${marked.length} >>\nstream\n${marked}endstream`,
      `<< /Type /StructElem /S /Document /P 8 0 R /K [7 0 R 9 0 R] >>`,
      `<< /Type /StructElem /S /P /P 6 0 R /Pg 3 0 R /K 0 >>`,
      `<< /Type /StructTreeRoot /K [6 0 R] /ParentTree 10 0 R /ParentTreeNextKey 1 >>`,
      `<< /Type /StructElem /S /P /P 6 0 R /Pg 3 0 R /K 1 >>`,
      // one key per page (its /StructParents); the array under it is indexed by MCID
      `<< /Nums [0 [7 0 R 9 0 R]] >>`,
    ]),
  );
  // 1 catalog 2 pages 3 page 4 font 5 contents
  const untagged = join(dir, "untagged.pdf");
  writeFileSync(
    untagged,
    buildPdf([
      `<< /Type /Catalog /Pages 2 0 R >>`,
      `<< /Type /Pages /Kids [3 0 R] /Count 1 >>`,
      `<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 200] /Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R >>`,
      `<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>`,
      `<< /Length ${plain.length} >>\nstream\n${plain}endstream`,
    ]),
  );
  return { tagged, untagged };
}

function orderOf(text: string, what: string): "FIRST,SECOND" | "SECOND,FIRST" {
  const a = text.indexOf("FIRST-PARA");
  const b = text.indexOf("SECOND-PARA");
  if (a < 0 || b < 0) {
    throw new Error(`${what}: missing paragraph text in ${JSON.stringify(text)}`);
  }
  return a < b ? "FIRST,SECOND" : "SECOND,FIRST";
}

export async function testit(): Promise<void> {
  const { tagged, untagged } = writeFixtures(tmpPath("reading-order"));

  const taggedOrder = orderOf(extractPageText(tagged, 1), "tagged");
  console.log(`tagged: ${taggedOrder}`);
  if (taggedOrder !== "FIRST,SECOND") {
    throw new Error(`tagged PDF ignored the structure tree's reading order (got ${taggedOrder})`);
  }

  const untaggedOrder = orderOf(extractPageText(untagged, 1), "untagged");
  console.log(`untagged: ${untaggedOrder}`);
  if (untaggedOrder !== "SECOND,FIRST") {
    throw new Error(`untagged PDF text order changed, it must stay in drawing order (got ${untaggedOrder})`);
  }

  console.log("reading order ok");
}

if (import.meta.main) {
  await (await import("./util.ts")).runStandalone(testit);
}
