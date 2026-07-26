// Generate a PDF with multiple annotation types for testing annotation render
// stability (Invalidate-before-rerender fix).
//
// The generated PDF:
//   - 2 pages, each with text content
//   - Page 1: Highlight + StrikeOut + Underline + Squiggly + FreeText + Line + Text annotations
//   - Page 2: Highlight + Circle + Square + FileAttachment annotations
//
// Regenerate with: bun tests/issue-annot-cache-make.ts
//
// Output: tests/issue-annot-cache.pdf

import { writeFileSync } from "node:fs";
import { join } from "node:path";

const DIR = import.meta.dir;

function enc(s: string): Buffer {
    return Buffer.from(s, "latin1");
}

function makePdf(): Buffer {
    const body: Record<number, Buffer> = {};

    body[1] = enc("<< /Type /Catalog /Pages 2 0 R >>");
    body[2] = enc("<< /Type /Pages /Kids [3 0 R 9 0 R] /Count 2 >>");
    body[5] = enc("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>");

    // --- Page 1 content ---
    const p1c = "BT /F1 24 Tf 72 740 Td (Page 1 - Annot Test) Tj ET\n"
        + "BT /F1 12 Tf 72 700 Td (Highlight) Tj ET\n"
        + "BT /F1 12 Tf 72 670 Td (StrikeOut) Tj ET\n"
        + "BT /F1 12 Tf 72 640 Td (Underline) Tj ET\n"
        + "BT /F1 12 Tf 72 610 Td (Squiggly) Tj ET\n"
        + "BT /F1 12 Tf 72 580 Td (FreeText area) Tj ET\n"
        + "BT /F1 12 Tf 72 550 Td (Line area) Tj ET\n"
        + "BT /F1 12 Tf 72 520 Td (Text note area) Tj ET";
    body[4] = enc(`<< /Length ${p1c.length} >>\nstream\n${p1c}\nendstream`);

    // --- Page 1 annotations ---
    // Highlight (type=Highlight, subtype=Highlight)
    body[6] = enc("<< /Type /Annot /Subtype /Highlight /Rect [72 685 200 700] /C [1 1 0] /P 3 0 R /QuadPoints [72 700 200 700 72 685 200 685] >>");
    // StrikeOut
    body[7] = enc("<< /Type /Annot /Subtype /StrikeOut /Rect [72 655 200 670] /C [1 0 0] /P 3 0 R /QuadPoints [72 670 200 670 72 655 200 655] >>");
    // Underline
    body[8] = enc("<< /Type /Annot /Subtype /Underline /Rect [72 625 200 640] /C [0 0 1] /P 3 0 R /QuadPoints [72 640 200 640 72 625 200 625] >>");
    // Squiggly
    body[10] = enc("<< /Type /Annot /Subtype /Squiggly /Rect [72 595 200 610] /C [0 1 0] /P 3 0 R /QuadPoints [72 610 200 610 72 595 200 595] >>");
    // FreeText
    body[11] = enc("<< /Type /Annot /Subtype /FreeText /Rect [72 540 300 575] /Contents (FreeText content) /C [0.8 0.8 1] /P 3 0 R >>");
    // Line
    body[12] = enc("<< /Type /Annot /Subtype /Line /Rect [72 500 300 550] /L [72 510 280 540] /C [1 0 1] /P 3 0 R >>");
    // Text (sticky note)
    body[13] = enc("<< /Type /Annot /Subtype /Text /Rect [400 540 430 570] /Contents (Note on page 1) /C [1 0 0] /P 3 0 R >>");
    // Page 1 object
    body[3] = enc("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R /Annots [6 0 R 7 0 R 8 0 R 10 0 R 11 0 R 12 0 R 13 0 R] >>");

    // --- Page 2 content ---
    const p2c = "BT /F1 24 Tf 72 740 Td (Page 2 - More Annots) Tj ET\n"
        + "BT /F1 12 Tf 72 700 Td (Highlight) Tj ET\n"
        + "BT /F1 12 Tf 72 670 Td (Circle area) Tj ET\n"
        + "BT /F1 12 Tf 72 640 Td (Square area) Tj ET\n"
        + "BT /F1 12 Tf 72 610 Td (FileAttachment) Tj ET";
    body[14] = enc(`<< /Length ${p2c.length} >>\nstream\n${p2c}\nendstream`);

    // --- Page 2 annotations ---
    body[15] = enc("<< /Type /Annot /Subtype /Highlight /Rect [72 685 220 700] /C [0.9 1 0] /P 9 0 R /QuadPoints [72 700 220 700 72 685 220 685] >>");
    body[16] = enc("<< /Type /Annot /Subtype /Circle /Rect [72 630 160 665] /C [0 1 1] /IC [0.9 0.9 0.9] /P 9 0 R >>");
    body[17] = enc("<< /Type /Annot /Subtype /Square /Rect [250 630 350 665] /C [1 0.5 0] /IC [0.9 0.9 0.9] /P 9 0 R >>");
    body[18] = enc("<< /Type /Annot /Subtype /FileAttachment /Rect [72 580 100 610] /C [0 0 1] /FS << /Type /Filespec /F (test.txt) >> /P 9 0 R >>");
    // Page 2 object
    body[9] = enc("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 5 0 R >> >> /Contents 14 0 R /Annots [15 0 R 16 0 R 17 0 R 18 0 R] >>");

    // Build cross-ref table
    const keys = Object.keys(body).map(Number).sort((a, b) => a - b);
    const offsets: number[] = [];
    let src = "%PDF-1.4\n";
    for (const k of keys) {
        offsets.push(src.length);
        src += `${k} 0 obj\n${body[k].toString("latin1")}\nendobj\n`;
    }
    const xrefOffset = src.length;
    src += "xref\n";
    src += `0 ${keys.length + 1}\n`;
    src += "0000000000 65535 f \n";
    for (const off of offsets) {
        src += `${String(off).padStart(10, "0")} 00000 n \n`;
    }
    src += "trailer << /Size " + (keys.length + 1) + " /Root 1 0 R >>\n";
    src += "startxref\n" + xrefOffset + "\n%%EOF\n";

    return enc(src);
}

const pdfPath = join(DIR, "issue-annot-cache.pdf");
writeFileSync(pdfPath, makePdf());
console.log("Generated: " + pdfPath);
