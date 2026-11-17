/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

// PDF document-level events and additional-actions (/Root/AA/*) support.
//
// mupdf reports what a document's JavaScript wants the application to do
// (app.alert(), doc.print(), app.launchURL(), doc.mailDoc(),
// app.execMenuItem()) through a single callback, pdf_set_doc_event_callback().
// All five events originate in pdf-js.c, so they only ever fire while
// JavaScript runs: with DisableJavaScript off, or when we run a document /
// page / field action ourselves.
//
// Two things to get right when wiring this up:
//   - pdf_enable_js() runs /Root/Names/JavaScript immediately, so the callback
//     has to be installed *before* it, or a document-level script's events are
//     silently dropped.
//   - events arrive on whichever thread ran the JavaScript - document loading,
//     rendering, printing - and they arrive with engine locks held. So
//     everything that touches the UI is posted to the UI thread, and the
//     posted task revalidates the window before doing anything. Only
//     app.alert() is answered inline, because the script blocks on the answer;
//     it needs a message pump, so it is answered only on the UI thread.

#include "base/Base.h"
#include "base/File.h"
#include "base/Http.h"
#include "base/Win.h"
#include "base/UITask.h"

#include <mupdf/pdf.h>

#include "gui/UIModels.h"
#include "Settings.h"
#include "GlobalPrefs.h"
#include "Annotation.h"
#include "DocController.h"
#include "EngineBase.h"
#include "base/GuessFileType.h"
#include "EngineAll.h"
#include "EngineMupdf.h"
#include "Translations.h"
#include "SumatraConfig.h"
#include "DisplayModel.h"
#include "DisplayMode.h"
#include "MainWindow.h"
#include "WindowTab.h"
#include "Notifications.h"
#include "Commands.h"
#include "SumatraPDF.h"
#include "PdfDocEvents.h"

// Acrobat's canonical app.execMenuItem() names, mapped onto our own commands.
// Anything we don't recognize is ignored rather than guessed at.
struct MenuItemCommand {
    const char* item;
    int cmdId;
};

static const MenuItemCommand kMenuItemCommands[] = {
    {"Print", CmdPrint},      {"PrintAgain", CmdPrint}, {"SaveAs", CmdSaveAs},
    {"SaveAsPdf", CmdSaveAs}, {"Close", CmdClose},      {"DocClose", CmdClose},
    {"Quit", CmdClose},       {"Exit", CmdClose},       {"Find", CmdFindFirst},
};

static int MenuItemToCommandId(const char* item) {
    for (const MenuItemCommand& m : kMenuItemCommands) {
        if (str::Eq(item, m.item)) {
            return m.cmdId;
        }
    }
    return 0;
}

// Find the window currently showing this document. Returns nullptr while the
// document is still loading (its events can fire before any window exists) and
// after its tab was closed.
static MainWindow* MainWindowForEngine(EngineBase* engine) {
    for (MainWindow* win : gWindows) {
        if (!IsMainWindowValidAndNotClosing(win)) {
            continue;
        }
        for (WindowTab* tab : win->Tabs()) {
            if (tab && tab->GetEngine() == engine) {
                return win;
            }
        }
    }
    return nullptr;
}

// Payload for a task posted to the UI thread. It captures the frame hwnd
// rather than a MainWindow*, because the window may be gone by the time the
// task runs and a WM_COMMAND to a dead window is dropped, while a stale
// pointer would not be.
struct PostedDocEvent {
    HWND frame = nullptr;
    int cmdId = 0;     // when non-zero, post this command to the frame
    Str url = nullptr; // when set, open this url
};

static void RunPostedDocEvent(PostedDocEvent* ev) {
    if (IsWindow(ev->frame)) {
        if (ev->cmdId) {
            HwndPostCommand(ev->frame, ev->cmdId);
        } else if (ev->url) {
            SumatraLaunchBrowser(ev->url);
        }
    }
    str::Free(ev->url);
    delete ev;
}

static void PostDocEvent(EngineBase* engine, int cmdId, Str url = nullptr) {
    MainWindow* win = MainWindowForEngine(engine);
    if (!win) {
        // A document-level script runs before the frame exists, so there is
        // nothing to post to. Say what the document asked for rather than
        // dropping it silently. The message is plain text: it comes from the
        // document, and notification markup turns text into commands/links.
        if (url) {
            // plain: the URL came from the document, and markup in a
            // notification becomes a clickable [text](CmdFoo) link
            MaybeDelayedPlainWarningNotification(fmt(_TRA("Document wants to open: %s").s, url));
        } else {
            logf("pdf doc event: document has no window yet, dropping command %d\n", cmdId);
        }
        str::Free(url);
        return;
    }
    auto* ev = new PostedDocEvent();
    ev->frame = win->hwndFrame;
    ev->cmdId = cmdId;
    ev->url = url;
    uitask::Post(MkFunc0<PostedDocEvent>(RunPostedDocEvent, ev), "TaskPdfDocEvent");
}

static void HandleAlert(fz_context* ctx, pdf_doc_event* evt) {
    pdf_alert_event* alert = pdf_access_alert_event(ctx, evt);
    if (!alert || !alert->message) {
        return;
    }
    logf("pdf app.alert(): %s\n", Str(alert->message));
    HWND hwndParent = GetHwndForNotification();
    if (!uitask::IsMainUIThread() || !hwndParent) {
        // No message pump, or no window to own the dialog yet: a document-level
        // script runs from EngineMupdf::FinishLoading(), which happens before
        // the frame exists. Surface what the document wanted and leave
        // mupdf's pre-seeded answer alone. Plain: the text is the document's
        // own, and markup in a notification becomes a clickable command link.
        MaybeDelayedPlainWarningNotification(Str(alert->message));
        return;
    }
    // mupdf pre-seeds button_pressed with what Acrobat answers when the user
    // just dismisses the dialog (pdf-js.c app_alert), so returning early
    // already behaves sensibly.
    TASKDIALOG_BUTTON buttons[3];
    int nButtons = 0;
    auto addButton = [&](int id) {
        buttons[nButtons].nButtonID = id;
        nButtons++;
    };
    switch (alert->button_group_type) {
        case PDF_ALERT_BUTTON_GROUP_OK:
            addButton(IDOK);
            break;
        case PDF_ALERT_BUTTON_GROUP_OK_CANCEL:
            addButton(IDOK);
            addButton(IDCANCEL);
            break;
        case PDF_ALERT_BUTTON_GROUP_YES_NO:
            addButton(IDYES);
            addButton(IDNO);
            break;
        case PDF_ALERT_BUTTON_GROUP_YES_NO_CANCEL:
            addButton(IDYES);
            addButton(IDNO);
            addButton(IDCANCEL);
            break;
    }

    // TaskDialogIndirect runs its own modal loop, so its strings must not live
    // in the temp arena (which is reset on every message loop pass). mupdf's
    // strings are UTF-8.
    WStr btnYes = wstr::Dup(WStrL("&Yes"));
    WStr btnNo = wstr::Dup(WStrL("&No"));
    WStr btnOk = wstr::Dup(WStrL("OK"));
    WStr btnCancel = wstr::Dup(WStrL("Cancel"));
    for (int i = 0; i < nButtons; i++) {
        switch (buttons[i].nButtonID) {
            case IDYES:
                buttons[i].pszButtonText = btnYes.s;
                break;
            case IDNO:
                buttons[i].pszButtonText = btnNo.s;
                break;
            case IDOK:
                buttons[i].pszButtonText = btnOk.s;
                break;
            default:
                buttons[i].pszButtonText = btnCancel.s;
                break;
        }
    }

    bool hasCheckBox = alert->has_check_box && alert->check_box_message;
    // Win32's TaskDialogIndirect has no checkbox at all (TASKDIALOGCONFIG
    // exposes cButtons and cRadioButtons only), so an alert's optional
    // "don't show this again" checkbox cannot be shown. Mention it in the
    // message instead of silently dropping it, and leave initially_checked /
    // finally_checked alone: a script that reads oCheckbox.bAfterValue then
    // sees what it would have seen had the user dismissed the dialog.
    WStr content;
    if (hasCheckBox) {
        str::Builder msg;
        msg.Append(alert->message);
        msg.Append("\n\n");
        msg.Append(fmt(_TRA("%s (this option cannot be changed in SumatraPDF)").s, Str(alert->check_box_message)));
        content = wstr::Dup(ToWStrTemp(ToStrTemp(msg)));
    } else {
        content = wstr::Dup(ToWStrTemp(Str(alert->message)));
    }

    WStr title = alert->title ? wstr::Dup(ToWStrTemp(Str(alert->title))) : wstr::Dup(WStrL("PDF alert"));

    TASKDIALOGCONFIG config = {};
    config.cbSize = sizeof(config);
    config.hwndParent = hwndParent;
    config.hInstance = GetModuleHandleW(nullptr);
    // hMainIcon is a union with pszMainIconPath, so TDF_USE_HICON_MAIN is what
    // tells TaskDialogIndirect to read it as an icon handle. Without the flag
    // it treats the handle as a string and crashes.
    config.dwFlags = TDF_SIZE_TO_CONTENT | TDF_POSITION_RELATIVE_TO_WINDOW;
    if (config.hInstance) {
        config.hMainIcon = (HICON)LoadIconW(config.hInstance, MAKEINTRESOURCEW(GetAppIconID()));
        if (config.hMainIcon) {
            config.dwFlags |= TDF_USE_HICON_MAIN;
        }
    }
    config.pszWindowTitle = title.s;
    config.pszContent = content.s;
    config.cButtons = nButtons;
    config.pButtons = buttons;
    config.nDefaultButton = 0;

    int pressed = 0;
    auto hr = TaskDialogIndirect(&config, &pressed, nullptr, nullptr);
    wstr::Free(title);
    wstr::Free(content);
    wstr::Free(btnYes);
    wstr::Free(btnNo);
    wstr::Free(btnOk);
    wstr::Free(btnCancel);
    if (hr != S_OK || pressed == 0) {
        return; // dismissed: keep mupdf's pre-seeded answer
    }
    switch (pressed) {
        case IDOK:
            alert->button_pressed = PDF_ALERT_BUTTON_OK;
            break;
        case IDCANCEL:
            alert->button_pressed = PDF_ALERT_BUTTON_CANCEL;
            break;
        case IDYES:
            alert->button_pressed = PDF_ALERT_BUTTON_YES;
            break;
        case IDNO:
            alert->button_pressed = PDF_ALERT_BUTTON_NO;
            break;
    }
}

static void HandleLaunchUrl(fz_context* ctx, pdf_doc_event* evt, EngineBase* engine) {
    pdf_launch_url_event* ev = pdf_access_launch_url_event(ctx, evt);
    if (!ev || !ev->url) {
        return;
    }
    // ev->new_frame is read from the wrong argument slot in mupdf 1.28
    // (pdf-js.c app_launchURL), so we always open in the current window.
    Str url = str::Dup(Str(ev->url));
    logf("pdf app.launchURL(): %s\n", url);
    // SumatraLaunchBrowser applies the LinkProtocols policy for us.
    PostDocEvent(engine, 0, url);
}

// Percent-encodes one mailto: field. Mailto parameters are '&'-separated, so an
// unescaped '&', '?', '#' or CR/LF in a document-supplied value would let the
// document forge extra parameters, or add mail headers (a silent Bcc) in
// whichever mail client the shell hands the URI to.
static void AppendMailtoField(str::Builder& uri, const char* name, const char* value, bool& firstParam, bool keepAt) {
    if (!value || !value[0]) {
        return;
    }
    uri.Append(firstParam ? StrL("?") : StrL("&"));
    firstParam = false;
    uri.Append(Str(name));
    uri.Append(StrL("="));
    TempStr enc = URLEncodeMayTruncateTemp(Str(value), 0, nullptr);
    if (keepAt) {
        // the recipient is the address part of the URI, and '%40' is a valid
        // encoding of '@' but not what mail clients expect to read
        enc = str::ReplaceTemp(enc, StrL("%40"), StrL("@"));
    }
    uri.Append(enc);
}

static void HandleMailDoc(fz_context* ctx, pdf_doc_event* evt, EngineBase* engine) {
    pdf_mail_doc_event* ev = pdf_access_mail_doc_event(ctx, evt);
    if (!ev) {
        return;
    }
    // mailDoc() is a request, not an instruction, and Acrobat's own UI asks
    // first. Composing a mailto: and letting the user press send is the closest
    // honest equivalent; we do not attach the document ourselves.
    str::Builder uri;
    uri.Append("mailto:");
    bool firstParam = true;
    AppendMailtoField(uri, "to", ev->to, firstParam, /* keepAt */ true);
    AppendMailtoField(uri, "cc", ev->cc, firstParam, /* keepAt */ false);
    AppendMailtoField(uri, "bcc", ev->bcc, firstParam, /* keepAt */ false);
    AppendMailtoField(uri, "subject", ev->subject, firstParam, /* keepAt */ false);
    AppendMailtoField(uri, "body", ev->message, firstParam, /* keepAt */ false);
    logf("pdf doc.mailDoc()\n");
    PostDocEvent(engine, 0, str::Dup(ToStrTemp(uri)));
}

static void HandleExecMenuItem(fz_context* ctx, pdf_doc_event* evt, EngineBase* engine) {
    const char* item = pdf_access_exec_menu_item_event(ctx, evt);
    if (!item) {
        return;
    }
    int cmd = MenuItemToCommandId(item);
    if (!cmd) {
        logf("pdf app.execMenuItem(%s): not supported\n", Str(item));
        return;
    }
    logf("pdf app.execMenuItem(%s)\n", Str(item));
    PostDocEvent(engine, cmd);
}

static void PdfDocEventCb(fz_context* ctx, pdf_document* doc, pdf_doc_event* evt, void* data) {
    if (!evt || !data) {
        return;
    }
    auto* engine = (EngineBase*)data;
    switch (evt->type) {
        case PDF_DOCUMENT_EVENT_ALERT:
            HandleAlert(ctx, evt);
            break;
        case PDF_DOCUMENT_EVENT_PRINT:
            logf("pdf doc.print()\n");
            PostDocEvent(engine, CmdPrint);
            break;
        case PDF_DOCUMENT_EVENT_LAUNCH_URL:
            HandleLaunchUrl(ctx, evt, engine);
            break;
        case PDF_DOCUMENT_EVENT_MAIL_DOC:
            HandleMailDoc(ctx, evt, engine);
            break;
        case PDF_DOCUMENT_EVENT_EXEC_MENU_ITEM:
            HandleExecMenuItem(ctx, evt, engine);
            break;
        default:
            // PDF_DOCUMENT_EVENT_SUBMIT is declared but never issued by mupdf,
            // so /S /SubmitForm actions are silently ignored - which is the
            // outcome we want anyway, since they would exfiltrate the document.
            break;
    }
}

void PdfInstallDocEventCallback(EngineBase* engine) {
    if (!engine || engine->kind != kindEngineMupdf) {
        return;
    }
    EngineMupdf* epdf = (EngineMupdf*)engine;
    if (!epdf->pdfdoc) {
        return;
    }
    // data is the engine and there is nothing to free: mupdf drops the document
    // (and with it the callback) in fz_drop_document, which happens after the
    // engine is unusable.
    pdf_set_doc_event_callback(epdf->BaseCtx(), epdf->pdfdoc, PdfDocEventCb, nullptr, engine);
}

static void RunDocAction(EngineBase* engine, void (*fn)(fz_context*, pdf_document*), const char* what) {
    if (!engine || engine->kind != kindEngineMupdf) {
        return;
    }
    EngineMupdf* epdf = (EngineMupdf*)engine;
    if (!epdf->pdfdoc) {
        return;
    }
    // These run /Root/AA/* actions, which may execute JavaScript and so may
    // re-enter the document-event callback. A throwing action must not abort
    // the operation that triggered it (printing, saving, closing).
    fz_try(epdf->Ctx()) {
        fn(epdf->Ctx(), epdf->pdfdoc);
    }
    fz_catch(epdf->Ctx()) {
        fz_report_error(epdf->Ctx());
        logf("%s: document action failed\n", Str(what));
    }
}

void PdfDocEventWillPrint(EngineBase* engine) {
    RunDocAction(engine, pdf_document_event_will_print, "pdf_document_event_will_print");
}

void PdfDocEventDidPrint(EngineBase* engine) {
    RunDocAction(engine, pdf_document_event_did_print, "pdf_document_event_did_print");
}

void PdfDocEventWillSave(EngineBase* engine) {
    RunDocAction(engine, pdf_document_event_will_save, "pdf_document_event_will_save");
}

void PdfDocEventDidSave(EngineBase* engine) {
    RunDocAction(engine, pdf_document_event_did_save, "pdf_document_event_did_save");
}

void PdfDocEventWillClose(EngineBase* engine) {
    RunDocAction(engine, pdf_document_event_will_close, "pdf_document_event_will_close");
}
