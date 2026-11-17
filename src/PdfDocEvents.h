/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

class EngineBase;

// Route mupdf's document-level events (app.alert(), doc.print(),
// app.launchURL(), doc.mailDoc(), app.execMenuItem()) to the UI. Must be
// called before pdf_enable_js(), which runs /Root/Names/JavaScript and can
// raise events immediately.
void PdfInstallDocEventCallback(EngineBase* engine);

// Run a document's /Root/AA/* additional action for a lifecycle event.
// No-ops for non-PDF documents and when JavaScript is disabled.
void PdfDocEventWillPrint(EngineBase* engine);
void PdfDocEventDidPrint(EngineBase* engine);
void PdfDocEventWillSave(EngineBase* engine);
void PdfDocEventDidSave(EngineBase* engine);
void PdfDocEventWillClose(EngineBase* engine);