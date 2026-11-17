/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

// Undo/redo for PDF edits, built on mupdf's journal.
//
// mupdf can record every change to a document's objects as a "journal" of
// fragments and step backwards and forwards through them (object.h:
// pdf_enable_journal and friends). That is exactly what an undo stack needs,
// and it saves us from snapshotting whole files or writing an inverse operation
// for each of the ~20 annotation setters.
//
// The catch: once the journal is on, mupdf *throws* if the document is modified
// outside an operation (object.h: "Any modification performed on a journalled
// PDF without an operation having been started will throw an error"). So every
// mutation site has to open one, which is what PdfScopedOperation is for.
//
// We enable the journal lazily, on the first edit, rather than at load time.
// Before that, mupdf is free to repair the document as it sees fit - notably
// pdf_check_structure_tree(), which writes to the structure tree and which
// mupdf itself calls from the page-running path. By the time a user can edit a
// page they have seen it rendered, so that repair is long done.
//
// Header-only on purpose: Annotation.cpp and friends already include mupdf, and
// a .cpp would have to be added to the UI-less DLL builds (the Windows Search
// filter, the Explorer preview handler) that compile those same files.
//
// Deliberately includes nothing: src/base/Base.h has no include guard (it is
// always the PCH, always first), so pulling it in from here would re-parse it.
// Include this after EngineMupdf.h, which the callers already do, and only from
// a .cpp that has already included <mupdf/pdf.h>.

class EngineMupdf;

// Turn the journal on if it isn't already. mupdf has no "is the journal on?"
// query, so the engine tracks it; enabling twice would leak the first journal.
inline void PdfEnsureJournal(fz_context* ctx, EngineMupdf* e) {
    if (!ctx || !e || !e->pdfdoc || e->journalEnabled) {
        return;
    }
    pdf_enable_journal(ctx, e->pdfdoc);
    e->journalEnabled = true;
    logf("PdfEnsureJournal: undo journal enabled\n");
}

// An undoable operation. Operations may nest - an inner one is folded into the
// enclosing one - so a compound edit (a form-field edit that also reformats its
// appearance, say) comes out as a single undo step.
//
// Declare this *outside* the fz_try that does the mutating, and let it commit
// when the scope ends. mupdf's fz_try is setjmp/longjmp-based, so a destructor
// inside the try block would be skipped when the edit throws; outside, the
// function still returns normally after fz_catch and the destructor runs then.
//
// A failed edit commits too, on purpose. pdf_abandon_operation() would discard
// the record of whatever did get changed, leaving the user with an edit they
// cannot undo and no way to know it happened.
class PdfScopedOperation {
  public:
    PdfScopedOperation(fz_context* ctx, EngineMupdf* e, const char* name) : ctx_(ctx), doc_(e ? e->pdfdoc : nullptr) {
        if (!ctx_ || !doc_) {
            return;
        }
        PdfEnsureJournal(ctx_, e);
        pdf_begin_operation(ctx_, doc_, name);
        started_ = true;
    }

    ~PdfScopedOperation() {
        if (started_) {
            pdf_end_operation(ctx_, doc_);
        }
    }

    PdfScopedOperation(const PdfScopedOperation&) = delete;
    PdfScopedOperation& operator=(const PdfScopedOperation&) = delete;

  private:
    fz_context* ctx_ = nullptr;
    pdf_document* doc_ = nullptr;
    bool started_ = false;
};
