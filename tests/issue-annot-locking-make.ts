// Generate a minimal PDF with annotations AND internal links for
// testing the annotation locking and rendering fixes.
//
// The generated PDF:
//   - 3 pages, each with text content
//   - Page 1: a Highlight annotation + a Text (sticky note) annotation + link to Page 2
//   - Page 2: a Highlight annotation + link to Page 3
//   - Page 3: a Text annotation
//   - Outline entries (for TestGetToc, TestDest)
//   - Named destinations (for TestNamedDest)
//
// Regenerate with: bun tests/issue-annot-locking-make.ts
//
// Output: tests/issue-annot-locking.pdf

import { writeFileSync } from "node:fs";
import { join } from "node:path";

const DIR = import.meta.dir;

function enc(s: string): Buffer {
    return Buffer.from(s, "latin1");
}

function makePdfWithAnnotations(): Buffer {
    // Object index:
    // 1  - Catalog
    // 2  - Pages
    // 3  - Page 1
    // 4  - Page 1 Contents
    // 5  - Font /F1 (Helvetica)
    // 6  - Highlight annot on page 1
    // 7  - Text annot on page 1
    // 8  - Link annot on page 1 → page 2
    // 9  - Page 2
    // 10 - Page 2 Contents
    // 11 - Highlight annot on page 2
    // 12 - Link annot on page 2 → page 3
    // 13 - Page 3
    // 14 - Page 3 Contents
    // 15 - Text annot on page 3
    // 16 - Outlines root
    // 17 - Outline entry "Go to page 2"
    // 18 - Outline entry "Go to page 3"
    // 19 - Named Destinations

    const body: Record<number, Buffer> = {};

    body[1] = enc("<< /Type /Catalog /Pages 2 0 R /Outlines 16 0 R /Dests 19 0 R >>");
    body[2] = enc("<< /Type /Pages /Kids [3 0 R 9 0 R 13 0 R] /Count 3 >>");
    body[5] = enc("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>");

    // --- Page 1 ---
    const p1c = "BT /F1 24 Tf 72 740 Td (Page 1 - Test) Tj ET\nBT /F1 12 Tf 72 700 Td (Highlight and text annots.) Tj ET";
    body[4] = enc(`<< /Length ${p1c.length} >>\nstream\n${p1c}\nendstream`);
    body[6] = enc("<< /Type /Annot /Subtype /Highlight /Rect [72 680 200 695] /C [1 0.9 0] /P 3 0 R /QuadPoints [72 695 200 695 72 680 200 680] >>");
    body[7] = enc("<< /Type /Annot /Subtype /Text /Rect [450 700 480 730] /Contents (Page 1 comment) /C [1 0 0] /P 3 0 R >>");
    body[8] = enc("<< /Type /Annot /Subtype /Link /Rect [72 600 200 620] /Border [0 0 1] /Dest [9 0 R /XYZ 72 700 1] /P 3 0 R >>");
    body[3] = enc("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R /Annots [6 0 R 7 0 R 8 0 R] >>");

    // --- Page 2 ---
    const p2c = "BT /F1 24 Tf 72 740 Td (Page 2 - Test) Tj ET\nBT /F1 12 Tf 72 700 Td (Highlight + link to page 3.) Tj ET";
    body[10] = enc(`<< /Length ${p2c.length} >>\nstream\n${p2c}\nendstream`);
    body[11] = enc("<< /Type /Annot /Subtype /Highlight /Rect [72 680 300 695] /C [0 1 0.8] /P 9 0 R /QuadPoints [72 695 300 695 72 680 300 680] >>");
    body[12] = enc("<< /Type /Annot /Subtype /Link /Rect [72 600 200 620] /Border [0 0 1] /Dest [13 0 R /XYZ 72 700 1] /P 9 0 R >>");
    body[9] = enc("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 5 0 R >> >> /Contents 10 0 R /Annots [11 0 R 12 0 R] >>");

    // --- Page 3 ---
    const p3c = "BT /F1 24 Tf 72 740 Td (Page 3 - Test) Tj ET\nBT /F1 12 Tf 72 700 Td (Text annotation only.) Tj ET";
    body[14] = enc(`<< /Length ${p3c.length} >>\nstream\n${p3c}\nendstream`);
    body[15] = enc("<< /Type /Annot /Subtype /Text /Rect [72 650 102 680] /Contents (Page 3 comment) /C [0 0 1] /P 13 0 R >>");
    body[13] = enc("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 5 0 R >> >> /Contents 14 0 R /Annots [15 0 R] >>");

    // --- Outlines ---
    body[16] = enc("<< /Type /Outlines /First 17 0 R /Last 18 0 R /Count 2 >>");
    body[17] = enc("<< /Title (Go to page 2) /Parent 16 0 R /Next 18 0 R /Dest [9 0 R /XYZ 72 700 1] >>");
    body[18] = enc("<< /Title (Go to page 3) /Parent 16 0 R /Prev 17 0 R /Dest [13 0 R /XYZ 72 700 1] >>");

    // --- Named Dests ---
    body[19] = enc("<< /Names [ (page2) [9 0 R /XYZ 72 700 1] (page3) [13 0 R /XYZ 72 700 1] ] >>");

    // --- Build xref ---
    const maxObj = 19;
    const parts: Buffer[] = [enc("%PDF-1.7\n%\xe2\xe3\xcf\xd3\n")];
    const off: Record<number, number> = {};
    let pos = parts[0].length;
    for (let k = 1; k <= maxObj; k++) {
        off[k] = pos;
        const o = Buffer.concat([enc(`${k} 0 obj\n`), body[k], enc("\nendobj\n")]);
        parts.push(o);
        pos += o.length;
    }
    let xref = `xref\n0 ${maxObj + 1}\n0000000000 65535 f \n`;
    for (let k = 1; k <= maxObj; k++) {
        xref += String(off[k]).padStart(10, "0") + " 00000 n \n";
    }
    parts.push(enc(`${xref}trailer\n<< /Size ${maxObj + 1} /Root 1 0 R >>\nstartxref\n${pos}\n%%EOF\n`));
    return Buffer.concat(parts);
}

if (import.meta.main) {
    const outPath = join(DIR, "issue-annot-locking.pdf");
    const data = makePdfWithAnnotations();
    writeFileSync(outPath, data);
    console.log(`generated: ${outPath} (${data.length} bytes)`);
}



