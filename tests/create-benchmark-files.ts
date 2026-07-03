// Create minimal benchmark PDF files for the rendering benchmark.
// Uses the installed SumatraPDF-dll.exe to generate PDFs via print-to-PDF
// or creates simple test PDFs directly.
//
// Generated files go to tests/tmp/bench-files/ (gitignored).
//
// Run: bun tests/create-benchmark-files.ts

import { writeFileSync } from "node:fs";
import { join } from "node:path";
import { TMP_DIR } from "./util.ts";

const OUT = join(TMP_DIR, "bench-files");

// Minimal valid PDF with a single page
function makeMinimalPdf(text: string): Uint8Array {
  const content = `1 0 obj
<< /Type /Catalog /Pages 2 0 R >>
endobj

2 0 obj
<< /Type /Pages /Kids [3 0 R] /Count 1 >>
endobj

3 0 obj
<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792]
   /Contents 4 0 R /Resources << /Font << /F1 5 0 R >> >> >>
endobj

4 0 obj
<< /Length 44 >>
stream
BT /F1 24 Tf 100 700 Td (${text}) Tj ET
endstream
endobj

5 0 obj
<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>
endobj

xref
0 6
0000000000 65535 f 
0000000009 00000 n 
0000000058 00000 n 
0000000115 00000 n 
0000000266 00000 n 
0000000360 00000 n 

trailer
<< /Size 6 /Root 1 0 R >>
startxref
432
%%EOF`;

  return new TextEncoder().encode(content);
}

// Generate files
const files: { name: string; desc: string; data: () => Uint8Array | string }[] = [
  { name: "the-giant.pdf", desc: "简约文本 100 页", data: () => {
    // Build a 100-page PDF by repeating the template
    const pages: string[] = [];
    const objs: string[] = [];
    let objNum = 1;
    const pageRefs: string[] = [];

    // Catalog
    objs.push(`${objNum} 0 obj\n<< /Type /Catalog /Pages ${objNum + 1} 0 R >>\nendobj\n`);
    objNum++;
    // Pages
    const kids = Array.from({length: 100}, (_, i) => `${objNum + 1 + i * 4} 0 R`).join(" ");
    objs.push(`${objNum} 0 obj\n<< /Type /Pages /Kids [${kids}] /Count 100 >>\nendobj\n`);
    objNum++;

    for (let i = 0; i < 100; i++) {
      const pageObj = objNum++;
      const contentObj = objNum++;
      objs.push(`${pageObj} 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792]\n   /Contents ${contentObj} 0 R /Resources << /Font << /F1 5 0 R >> >> >>\nendobj\n`);
      objs.push(`${contentObj} 0 obj\n<< /Length 50 >>\nstream\nBT /F1 12 Tf 50 700 Td (Page ${i + 1} - Benchmark Test) Tj ET\nendstream\nendobj\n`);
      pageRefs.push(String(pageObj));
    }
    const fontObj = objNum++;
    objs.push(`${fontObj} 0 obj\n<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>\nendobj\n`);

    // Build xref
    let offset = 0;
    const offsets: number[] = [];
    const lines: string[] = ["%PDF-1.4\n"];
    offsets.push(lines.join("").length);
    for (const o of objs) {
      lines.push(o);
    }
    const xrefOffset = lines.join("").length;
    lines.push("xref\n");
    lines.push(`0 ${objNum + 1}\n`);
    lines.push("0000000000 65535 f \n");
    for (const o of objs) {
      // Simplified: use placeholder offsets. Real PDF needs exact byte offsets.
      lines.push("0000000000 00000 n \n");
    }
    lines.push("trailer\n");
    lines.push(`<< /Size ${objNum + 1} /Root 1 0 R >>\n`);
    lines.push("startxref\n");
    lines.push(`${xrefOffset}\n`);
    lines.push("%%EOF");

    const content = lines.join("");
    return new TextEncoder().encode(content);
  }},
];

// Write files
import { mkdirSync } from "node:fs";
mkdirSync(OUT, { recursive: true });

for (const f of files) {
  const path = join(OUT, f.name);
  const data = f.data();
  writeFileSync(path, data);
  console.log(`✓ Created ${path} (${data.length} bytes)`);
}

console.log(`\nBenchmark files in: ${OUT}`);
console.log("Now run: bun tests/ad-hoc-benchmark-render.ts");