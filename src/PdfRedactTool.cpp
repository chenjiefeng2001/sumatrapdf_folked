/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// `sumatrapdf-tool redact` - permanently remove the content covered by a PDF's
// redaction annotations. mupdf 1.28 removed its own `mutool redact`, so this
// fills that gap with the same pdf_redact_page() the GUI uses.
//
// Deliberately free of src/base helpers: sumatrapdf-tool.exe links only
// libsumatrapdf.dll, not the app's base library.

#include "base/Base.h"

#include <mupdf/pdf.h>

#include <stdio.h>
#include <string.h>

#include "PdfRedactTool.h"

#define MAX_PAGE_RANGES 64

struct RedactArgs {
    pdf_redact_options opts;
    const char* pages;
    const char* input;
    const char* output;
    int ranges[MAX_PAGE_RANGES];
    int nRanges;
};

static void RedactUsage(void) {
    fprintf(stderr,
            "usage: sumatrapdf-tool redact [options] <input.pdf> <output.pdf>\n"
            "\n"
            "Applies every redaction annotation in the document: the content they\n"
            "cover is removed and the annotations themselves are deleted. This is\n"
            "irreversible, so the result always goes to a separate output file.\n"
            "\n"
            "options:\n"
            "  -text <remove|none|invisible>  how to treat overlapping text\n"
            "        (default: remove; 'none' leaves text readable under the mark)\n"
            "  -image <none|remove|pixels|remove-invisible>\n"
            "        (default: pixels)\n"
            "  -lineart <none|covered|touched>\n"
            "        (default: covered)\n"
            "  -no-black-boxes  don't draw a black box over each redaction\n"
            "  -pages <spec>     only these pages, e.g. '1-3,7' (default: all)\n");
}

// "-pages 1-3,7" -> pairs of page numbers in args->ranges
static bool ParsePagesSpec(const char* spec, RedactArgs& args) {
    args.nRanges = 0;
    const char* p = spec;
    while (*p) {
        while (*p == ',' || *p == ' ' || *p == '\t') {
            p++;
        }
        if (!*p) {
            break;
        }
        if (*p < '0' || *p > '9') {
            return false;
        }
        int start = 0;
        while (*p >= '0' && *p <= '9') {
            start = start * 10 + (*p++ - '0');
        }
        int end = start;
        if (*p == '-') {
            p++;
            if (*p < '0' || *p > '9') {
                return false;
            }
            end = 0;
            while (*p >= '0' && *p <= '9') {
                end = end * 10 + (*p++ - '0');
            }
        }
        if (start < 1 || start > end) {
            return false;
        }
        if (args.nRanges >= MAX_PAGE_RANGES) {
            return false;
        }
        args.ranges[args.nRanges++] = start;
        args.ranges[args.nRanges++] = end;
    }
    return args.nRanges > 0;
}

static bool PageSelected(const RedactArgs& args, int pageNo) {
    if (args.nRanges == 0) {
        return true;
    }
    for (int i = 0; i < args.nRanges; i += 2) {
        if (pageNo >= args.ranges[i] && pageNo <= args.ranges[i + 1]) {
            return true;
        }
    }
    return false;
}

static bool ParseRedactArgs(int argc, char* argv[], RedactArgs& args) {
    memset(&args, 0, sizeof(args));
    // stricter than mupdf's own defaults, which leave vector graphics alone
    args.opts.black_boxes = 1;
    args.opts.image_method = PDF_REDACT_IMAGE_PIXELS;
    args.opts.line_art = PDF_REDACT_LINE_ART_REMOVE_IF_COVERED;
    args.opts.text = PDF_REDACT_TEXT_REMOVE;

    int i = 1;
    for (; i < argc; i++) {
        char* a = argv[i];
        if (a[0] != '-' || a[1] == '\0') {
            break;
        }
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            return false;
        }
        if (strcmp(a, "-no-black-boxes") == 0) {
            args.opts.black_boxes = 0;
            continue;
        }
        bool wantsValue = (strcmp(a, "-text") == 0 || strcmp(a, "-image") == 0 || strcmp(a, "-lineart") == 0 ||
                           strcmp(a, "-pages") == 0);
        if (!wantsValue || i + 1 >= argc) {
            fprintf(stderr, "unknown option '%s'\n", a);
            return false;
        }
        char* val = argv[++i];
        if (strcmp(a, "-pages") == 0) {
            if (!ParsePagesSpec(val, args)) {
                fprintf(stderr, "bad -pages spec '%s'\n", val);
                return false;
            }
            args.pages = val;
            continue;
        }
        if (strcmp(a, "-text") == 0) {
            if (strcmp(val, "remove") == 0) {
                args.opts.text = PDF_REDACT_TEXT_REMOVE;
            } else if (strcmp(val, "none") == 0) {
                args.opts.text = PDF_REDACT_TEXT_NONE;
            } else if (strcmp(val, "invisible") == 0) {
                args.opts.text = PDF_REDACT_TEXT_REMOVE_INVISIBLE;
            } else {
                fprintf(stderr, "unknown -text value '%s'\n", val);
                return false;
            }
            continue;
        }
        if (strcmp(a, "-image") == 0) {
            if (strcmp(val, "none") == 0) {
                args.opts.image_method = PDF_REDACT_IMAGE_NONE;
            } else if (strcmp(val, "remove") == 0) {
                args.opts.image_method = PDF_REDACT_IMAGE_REMOVE;
            } else if (strcmp(val, "pixels") == 0) {
                args.opts.image_method = PDF_REDACT_IMAGE_PIXELS;
            } else if (strcmp(val, "remove-invisible") == 0) {
                args.opts.image_method = PDF_REDACT_IMAGE_REMOVE_UNLESS_INVISIBLE;
            } else {
                fprintf(stderr, "unknown -image value '%s'\n", val);
                return false;
            }
            continue;
        }
        if (strcmp(val, "none") == 0) {
            args.opts.line_art = PDF_REDACT_LINE_ART_NONE;
        } else if (strcmp(val, "covered") == 0) {
            args.opts.line_art = PDF_REDACT_LINE_ART_REMOVE_IF_COVERED;
        } else if (strcmp(val, "touched") == 0) {
            args.opts.line_art = PDF_REDACT_LINE_ART_REMOVE_IF_TOUCHED;
        } else {
            fprintf(stderr, "unknown -lineart value '%s'\n", val);
            return false;
        }
    }
    if (argc - i != 2) {
        return false;
    }
    args.input = argv[i];
    args.output = argv[i + 1];
    return true;
}

int PdfRedactMain(int argc, char* argv[]) {
    RedactArgs args;
    if (!ParseRedactArgs(argc, argv, args)) {
        RedactUsage();
        return 1;
    }

    fz_context* ctx = fz_new_context(nullptr, nullptr, FZ_STORE_UNLIMITED);
    if (!ctx) {
        fprintf(stderr, "redact: could not create mupdf context\n");
        return 1;
    }
    int exitCode = 1;
    fz_try(ctx) {
        fz_register_document_handlers(ctx);
        pdf_document* doc = pdf_open_document(ctx, args.input);
        int nPages = pdf_count_pages(ctx, doc);
        printf("redact: '%s' has %d page(s)\n", args.input, nPages);
        int nRedacted = 0;
        for (int i = 0; i < nPages; i++) {
            int pageNo = i + 1;
            if (!PageSelected(args, pageNo)) {
                continue;
            }
            pdf_page* page = pdf_load_page(ctx, doc, i);
            fz_try(ctx) {
                if (pdf_redact_page(ctx, doc, page, &args.opts)) {
                    nRedacted++;
                    printf("redact: page %d\n", pageNo);
                }
            }
            fz_catch(ctx) {
                fz_report_error(ctx);
                fprintf(stderr, "redact: page %d failed: %s\n", pageNo, fz_caught_message(ctx));
            }
            pdf_drop_page(ctx, page);
        }
        if (nRedacted == 0) {
            fprintf(stderr, "redact: no redaction annotations found; '%s' not written\n", args.output);
            pdf_drop_document(ctx, doc);
            fz_drop_context(ctx);
            return 1;
        }
        pdf_write_options opts;
        memset(&opts, 0, sizeof(opts));
        opts.do_compress = 1;
        opts.do_compress_images = 1;
        opts.do_compress_fonts = 1;
        // pdf_redact_page() set doc->redacted, so an incremental save is not
        // possible, and the removed objects only really leave the file if we
        // garbage collect.
        opts.do_garbage = 1;
        pdf_save_document(ctx, doc, args.output, &opts);
        printf("redact: wrote '%s' (%d page(s) redacted)\n", args.output, nRedacted);
        exitCode = 0;
        pdf_drop_document(ctx, doc);
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
        fprintf(stderr, "redact: %s\n", fz_caught_message(ctx));
        exitCode = 1;
    }
    fz_drop_context(ctx);
    return exitCode;
}