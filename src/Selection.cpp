/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "base/Base.h"
#include "base/Pixmap.h"
#include <uiautomationcore.h>
#include "base/Dpi.h"
#include "base/Win.h"

#ifdef _MSC_VER
#include "GpuBackend.h"
#endif

#include "wingui/UIModels.h"

#include "Settings.h"
#include "DocController.h"
#include "EngineBase.h"
#include "GlobalPrefs.h"
#include "ChmModel.h"
#include "DisplayModel.h"
#include "TextSelection.h"
#include "Notifications.h"
#include "SumatraConfig.h"
#include "SumatraPDF.h"
#include "Canvas.h"
#include "MainWindow.h"
#include "HardwareProfile.h"
#include "WindowTab.h"
#include "Selection.h"
#include "Toolbar.h"
#include "Translations.h"
#include "uia/Provider.h"

// AlphaBlend lives in msimg32.h, which our pinned Windows SDK doesn't ship;
// load it dynamically at first use (msimg32.dll is present on Win7+).
typedef BOOL(WINAPI* Sig_AlphaBlend)(HDC, int, int, int, int, HDC, int, int, int, int, BLENDFUNCTION);
static Sig_AlphaBlend DynAlphaBlend = nullptr;
static bool triedLoadAlphaBlend = false;

static bool EnsureAlphaBlendLoaded() {
    if (triedLoadAlphaBlend) {
        return DynAlphaBlend != nullptr;
    }
    triedLoadAlphaBlend = true;
    HMODULE h = GetModuleHandleW(L"msimg32.dll");
    if (!h) {
        h = LoadLibraryW(L"msimg32.dll");
    }
    if (h) {
        DynAlphaBlend = (Sig_AlphaBlend)GetProcAddress(h, "AlphaBlend");
    }
    return DynAlphaBlend != nullptr;
}

SelectionOnPage::SelectionOnPage(int pageNo, const RectF* const rect) {
    this->pageNo = pageNo;
    if (rect) {
        this->rect = *rect;
    } else {
        this->rect = RectF();
    }
}

Rect SelectionOnPage::GetRect(DisplayModel* dm) const {
    // if the page is not visible, we return an empty rectangle
    PageInfo* pageInfo = dm->GetPageInfo(pageNo);
    if (!pageInfo || pageInfo->visibleRatio <= 0.0) {
        return Rect();
    }

    return dm->CvtToScreen(pageNo, rect);
}

Vec<SelectionOnPage>* SelectionOnPage::FromRectangle(DisplayModel* dm, Rect rect) {
    Vec<SelectionOnPage>* sel = new Vec<SelectionOnPage>();

    for (int pageNo = dm->GetEngine()->PageCount(); pageNo >= 1; --pageNo) {
        PageInfo* pi = dm->GetPageInfo(pageNo);
        ReportIf(!(!pi || 0.0 == pi->visibleRatio || pi->isShown));
        if (!pi || !pi->isShown) {
            continue;
        }

        Rect intersect = rect.Intersect(pi->pageOnScreen);
        if (intersect.IsEmpty()) {
            continue;
        }

        /* selection intersects with a page <pageNo> on the screen */
        RectF isectD = dm->CvtFromScreen(intersect, pageNo);
        sel->Append(SelectionOnPage(pageNo, &isectD));
    }
    sel->Reverse();

    if (len(*sel) == 0) {
        delete sel;
        return nullptr;
    }
    return sel;
}

Vec<SelectionOnPage>* SelectionOnPage::FromTextSelect(TextSel* textSel) {
    Vec<SelectionOnPage>* sel = new Vec<SelectionOnPage>(textSel->len);

    for (int i = textSel->len - 1; i >= 0; i--) {
        RectF rect = ToRectF(textSel->rects[i]);
        sel->Append(SelectionOnPage(textSel->pages[i], &rect));
    }
    sel->Reverse();

    if (len(*sel) == 0) {
        delete sel;
        return nullptr;
    }
    return sel;
}

void DeleteOldSelectionInfo(MainWindow* win, bool alsoTextSel) {
    win->showSelection = false;
    win->selectionMeasure = SizeF();
    WindowTab* tab = win->CurrentTab();
    if (!tab) {
        return;
    }

    delete tab->selectionOnPage;
    tab->selectionOnPage = nullptr;
    if (alsoTextSel && tab->AsFixed()) {
        tab->AsFixed()->textSelection->Reset();
    }
}

// Low-end Fast-Path: paint the translucent overlay with plain GDI
// (one 1x1 premultiplied DIBSection, AlphaBlend-stretched per rect) instead of
// GDI+ GraphicsPath/SolidBrush. Same visual result, an order of magnitude less
// CPU on machines without GPU acceleration. The selection border is dropped.
static void PaintTransparentRectanglesGdi(HDC hdc, Vec<Rect>& rects, COLORREF selectionColor, u8 alpha, int pad) {
    if (len(rects) == 0 || alpha == 0) {
        return;
    }

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = 1;
    bmi.bmiHeader.biHeight = -1; // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HDC memdc = CreateCompatibleDC(hdc);
    if (!memdc) {
        return;
    }
    HBITMAP bmp = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp || !bits) {
        if (bmp) DeleteObject(bmp);
        DeleteDC(memdc);
        return;
    }
    HGDIOBJ oldBmp = SelectObject(memdc, bmp);

    u8 r, g, b;
    UnpackColor(selectionColor, r, g, b);
    // AC_SRC_ALPHA sources must be premultiplied
    u32* px = (u32*)bits;
    *px = ((u32)alpha << 24) | ((u32)(r * alpha / 255) << 16) | ((u32)(g * alpha / 255) << 8) | (u32)(b * alpha / 255);

    BLENDFUNCTION bf = {};
    bf.BlendOp = AC_SRC_OVER;
    bf.SourceConstantAlpha = alpha;
    bf.AlphaFormat = AC_SRC_ALPHA;

    bool canBlend = EnsureAlphaBlendLoaded();
    HBRUSH fallbackBrush = nullptr;
    if (!canBlend) {
        fallbackBrush = CreateSolidBrush(selectionColor);
    }

    for (int i = 0; i < len(rects); i++) {
        Rect rc = rects.at(i);
        if (pad > 0) {
            rc.Inflate(pad, pad);
        }
        if (rc.dx <= 0 || rc.dy <= 0) {
            continue;
        }
        if (canBlend) {
            DynAlphaBlend(hdc, rc.x, rc.y, rc.dx, rc.dy, memdc, 0, 0, 1, 1, bf);
        } else {
            RECT rcRect = {rc.x, rc.y, rc.x + rc.dx, rc.y + rc.dy};
            FillRect(hdc, &rcRect, fallbackBrush);
        }
    }

    if (fallbackBrush) {
        DeleteObject(fallbackBrush);
    }
    SelectObject(memdc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(memdc);
}

void PaintTransparentRectangles(HDC hdc, Rect screenRc, Vec<Rect>& rects, COLORREF selectionColor, u8 alpha, int pad,
                                bool drawBorder) {
    // Low-end Fast-Path: plain GDI AlphaBlend, skips D2D/GDI+ overhead
    if (g_hwProfile.isLowEnd) {
        PaintTransparentRectanglesGdi(hdc, rects, selectionColor, alpha, pad);
        return;
    }

    // GPU-accelerated path via Direct2D (when available). Falls back to GDI+.
#ifdef _MSC_VER
    if (gGpuBackend && gGpuBackend->isAvailable) {
        if (GpuBackend::DrawOverlayRects(hdc, screenRc, rects, selectionColor, alpha, pad, drawBorder)) {
            return; // D2D succeeded
        }
        // D2D failed — fall through to GDI+ below
    }
#endif

    // create path from rectangles
    Gdiplus::GraphicsPath path(Gdiplus::FillModeWinding);
    screenRc.Inflate(pad, pad);
    for (int i = 0; i < len(rects); i++) {
        Rect rc = rects.at(i);
        if (pad > 0) {
            rc.Inflate(pad, pad);
        }
        rc = rc.Intersect(screenRc);
        if (!rc.IsEmpty()) {
            path.AddRectangle(ToGdipRect(rc));
        }
    }

    Gdiplus::Graphics gs(hdc);
    u8 r, g, b;
    UnpackColor(selectionColor, r, g, b);
    Gdiplus::Color c(alpha, r, g, b);
    Gdiplus::SolidBrush tmpBrush(c);
    gs.FillPath(&tmpBrush, &path);
    if (drawBorder && pad > 0) {
        // black outline around the filled region (only the selection asks for this;
        // find-match and read-aloud highlights stay borderless)
        path.Outline(nullptr, 0.2f);
        Gdiplus::Pen tmpPen(Gdiplus::Color(alpha, 0, 0, 0), (float)pad);
        gs.DrawPath(&tmpPen, &path);
    }
}

void PaintSelection(MainWindow* win, HDC hdc) {
    if (!win->AsFixed()) {
        return;
    }

    Vec<Rect> rects;

    if (win->mouseAction == MouseAction::Selecting) {
        // during rectangle selection
        Rect selRect = win->selectionRect;
        if (selRect.dx < 0) {
            selRect.x += selRect.dx;
            selRect.dx *= -1;
        }
        if (selRect.dy < 0) {
            selRect.y += selRect.dy;
            selRect.dy *= -1;
        }

        rects.Append(selRect);
    } else {
        // during text selection or after selection is done
        if (MouseAction::SelectingText == win->mouseAction) {
            // double/triple-click set the glyph range immediately; only extend
            // on repaint when the pointer has actually moved (issue #5712).
            int endX = win->selectionRect.x + win->selectionRect.dx;
            int endY = win->selectionRect.y + win->selectionRect.dy;
            bool dragged = IsDragDistance(win->selectionRect.x, endX, win->selectionRect.y, endY);
            UpdateTextSelection(win, dragged);
            WindowTab* tab = win->CurrentTab();
            if (!tab) {
                return;
            }
            if (!tab->selectionOnPage) {
                // prevent the selection from disappearing while the
                // user is still at it (OnSelectionStop removes it
                // if it is still empty at the end)
                tab->selectionOnPage = new Vec<SelectionOnPage>();
                win->showSelection = true;
            }
        }

        WindowTab* tab = win->CurrentTab();
        if (!tab) {
            return;
        }
        ReportDebugIf(!tab->selectionOnPage);
        if (!tab->selectionOnPage) {
            return;
        }

        for (SelectionOnPage& sel : *tab->selectionOnPage) {
            rects.Append(sel.GetRect(win->AsFixed()));
        }
    }

    ParsedColor* parsedCol = GetPrefsColor(gGlobalPrefs->fixedPageUI.selectionColor);
    // honor the alpha channel of SelectionColor (#aarrggbb): a smaller alpha makes
    // the overlay more transparent so the selected text stays crisp (issue #3209).
    // Fall back to the historical default when no alpha is given (e.g. #rrggbb).
    u8 alpha = GetAlpha(parsedCol->col);
    if (alpha == 0) {
        alpha = kSelectionDefaultAlpha;
    }
    PaintTransparentRectangles(hdc, win->canvasRc, rects, parsedCol->col, alpha, 2, /*drawBorder*/ true);
}

void UpdateTextSelection(MainWindow* win, bool select) {
    if (!win->AsFixed()) {
        return;
    }

    // logf("UpdateTextSelection: select: %d\n", (int)select);
    DisplayModel* dm = win->AsFixed();
    if (select) {
        int pageNo = dm->GetPageNoByPoint(win->selectionRect.BR());
        if (win->ctrl && win->ctrl->ValidPageNo(pageNo)) {
            PointF pt = dm->CvtFromScreen(win->selectionRect.BR(), pageNo);
            if (win->selectingByWord) {
                // double-click-drag: extend a whole word at a time (issue #4761)
                dm->textSelection->SelectWordsUpTo(pageNo, pt.x, pt.y);
            } else {
                dm->textSelection->SelectUpTo(pageNo, pt.x, pt.y);
            }
        }
    }

    DeleteOldSelectionInfo(win);
    WindowTab* tab = win->CurrentTab();
    if (!tab) {
        return;
    }
    tab->selectionOnPage = SelectionOnPage::FromTextSelect(&dm->textSelection->result);
    win->showSelection = tab->selectionOnPage != nullptr;

    if (win->uiaProvider) {
        win->uiaProvider->OnSelectionChanged();
    }
}

// isTextSelectionOut is set to true if this is text-only selection (as opposed to
// rectangular selection)
TempStr GetSelectedTextTemp(WindowTab* tab, Str lineSep, bool& isTextOnlySelectionOut) {
    if (!tab || !tab->selectionOnPage) {
        return {};
    }
    if (len(*tab->selectionOnPage) == 0) {
        return {};
    }
    DisplayModel* dm = tab->AsFixed();
    ReportIf(!dm);
    if (!dm) {
        return {};
    }
    if (dm->GetEngine()->IsImageCollection()) {
        return {};
    }

    isTextOnlySelectionOut = dm->textSelection->result.len > 0;
    if (isTextOnlySelectionOut) {
        Str s = dm->textSelection->ExtractText(lineSep);
        TempStr res = str::DupTemp(s);
        str::Free(s);
        return res;
    }
    StrVec selections;
    for (SelectionOnPage& sel : *tab->selectionOnPage) {
        // selection may reference pages that no longer exist after a reload
        if (!dm->ValidPageNo(sel.pageNo)) {
            continue;
        }
        Str text = dm->GetTextInRegion(sel.pageNo, sel.rect);
        if (text) {
            selections.Append(text);
        }
    }
    if (len(selections) == 0) {
        return {};
    }
    TempStr s = JoinTemp(&selections, lineSep);
    return s;
}

void CopySelectionToClipboard(MainWindow* win) {
    WindowTab* tab = win->CurrentTab();
    if (!tab) {
        return;
    }
    ReportIf(len(*tab->selectionOnPage) == 0 && win->mouseAction != MouseAction::SelectingText);

    if (!OpenClipboard(nullptr)) {
        return;
    }
    EmptyClipboard();
    defer {
        CloseClipboard();
    };

    DisplayModel* dm = win->AsFixed();
    TempStr selText = nullptr;
    bool isTextOnlySelectionOut = false;
    if (!gDisableDocumentRestrictions && (dm && !dm->GetEngine()->AllowsCopyingText())) {
        NotificationCreateArgs args;
        args.hwndParent = win->hwndCanvas;
        args.msg = _TRA("Copying text was denied (copying as image only)");
        ShowNotification(args);
    } else {
        selText = GetSelectedTextTemp(tab, "\r\n", isTextOnlySelectionOut);
    }

    if (!str::IsEmpty(selText)) {
        AppendTextToClipboard(selText);
    }

    if (isTextOnlySelectionOut) {
        // don't also copy the first line of a text selection as an image
        return;
    }

    if (!dm || !tab->selectionOnPage || len(*tab->selectionOnPage) == 0) {
        return;
    }
    /* also copy a screenshot of the current selection to the clipboard */
    SelectionOnPage* selOnPage = &tab->selectionOnPage->at(0);
    if (!dm->ValidPageNo(selOnPage->pageNo)) {
        return;
    }
    float zoom = dm->GetZoomReal(selOnPage->pageNo);
    int rotation = dm->GetRotation();
    RenderPageArgs args(selOnPage->pageNo, zoom, rotation, &selOnPage->rect, RenderTarget::Export);
    Pixmap* bmp = dm->GetEngine()->RenderPage(args);
    if (bmp) {
        CopyImageToClipboard(bmp->hbmp, true);
    }
    FreePixmap(bmp);
}

void OnSelectAll(MainWindow* win, bool textOnly) {
    if (!HasPermission(Perm::CopySelection)) {
        return;
    }

    if (HwndIsFocused(win->hwndFindEdit) || HwndIsFocused(win->hwndPageEdit)) {
        EditSelectAll(GetFocus());
        return;
    }

    if (win->AsChm()) {
        win->AsChm()->SelectAll();
        return;
    }
    if (!win->AsFixed()) {
        return;
    }

    DisplayModel* dm = win->AsFixed();
    if (textOnly) {
        int pageNo;
        for (pageNo = 1; !dm->PageShown(pageNo); pageNo++) {
            ;
        }
        dm->textSelection->StartAt(pageNo, 0);
        for (pageNo = win->ctrl->PageCount(); !dm->PageShown(pageNo); pageNo--) {
            ;
        }
        dm->textSelection->SelectUpTo(pageNo, -1);
        win->selectionRect = Rect::FromXY(INT_MIN / 2, INT_MIN / 2, INT_MAX, INT_MAX);
        UpdateTextSelection(win);
    } else {
        DeleteOldSelectionInfo(win, true);
        win->selectionRect = Rect::FromXY(INT_MIN / 2, INT_MIN / 2, INT_MAX, INT_MAX);
        win->CurrentTab()->selectionOnPage = SelectionOnPage::FromRectangle(dm, win->selectionRect);
    }

    win->showSelection = win->CurrentTab()->selectionOnPage != nullptr;
    ScheduleRepaint(win, 0);
}

#define SELECT_AUTOSCROLL_AREA_WIDTH DpiScale(win->hwndFrame, 15)
#define SELECT_AUTOSCROLL_STEP_LENGTH DpiScale(win->hwndFrame, 10)

bool NeedsSelectionEdgeAutoscroll(MainWindow* win, int x, int y) {
    return x < SELECT_AUTOSCROLL_AREA_WIDTH || x > win->canvasRc.dx - SELECT_AUTOSCROLL_AREA_WIDTH ||
           y < SELECT_AUTOSCROLL_AREA_WIDTH || y > win->canvasRc.dy - SELECT_AUTOSCROLL_AREA_WIDTH;
}

void OnSelectionEdgeAutoscroll(MainWindow* win, int x, int y) {
    int dx = 0, dy = 0;

    if (x < SELECT_AUTOSCROLL_AREA_WIDTH) {
        dx = -SELECT_AUTOSCROLL_STEP_LENGTH;
    } else if (x > win->canvasRc.dx - SELECT_AUTOSCROLL_AREA_WIDTH) {
        dx = SELECT_AUTOSCROLL_STEP_LENGTH;
    }
    if (y < SELECT_AUTOSCROLL_AREA_WIDTH) {
        dy = -SELECT_AUTOSCROLL_STEP_LENGTH;
    } else if (y > win->canvasRc.dy - SELECT_AUTOSCROLL_AREA_WIDTH) {
        dy = SELECT_AUTOSCROLL_STEP_LENGTH;
    }

    ReportIf(NeedsSelectionEdgeAutoscroll(win, x, y) != (dx != 0 || dy != 0));
    if (dx != 0 || dy != 0) {
        ReportIf(!win->AsFixed());
        DisplayModel* dm = win->AsFixed();
        Point oldOffset = dm->GetViewPort().TL();
        win->MoveDocBy(dx, dy);

        dx = dm->GetViewPort().x - oldOffset.x;
        dy = dm->GetViewPort().y - oldOffset.y;
        win->selectionRect.x -= dx;
        win->selectionRect.y -= dy;
        win->selectionRect.dx += dx;
        win->selectionRect.dy += dy;
    }
}

void OnSelectionStart(MainWindow* win, int x, int y, WPARAM) {
    ReportIf(!win->AsFixed());
    DeleteOldSelectionInfo(win, true);

    win->selectionRect = Rect(x, y, 0, 0);
    win->showSelection = true;
    win->selectingByWord = false;
    win->mouseAction = MouseAction::Selecting;

    bool isShift = IsShiftPressed();
    bool isCtrl = IsCtrlPressed();

    // Ctrl+drag forces a rectangular selection
    if (!isCtrl || isShift) {
        DisplayModel* dm = win->AsFixed();
        int pageNo = dm->GetPageNoByPoint(Point(x, y));
        if (dm->ValidPageNo(pageNo)) {
            PointF pt = dm->CvtFromScreen(Point(x, y), pageNo);
            dm->textSelection->StartAt(pageNo, pt.x, pt.y);
            win->mouseAction = MouseAction::SelectingText;
        }
    }

    SetCapture(win->hwndCanvas);
    SetTimer(win->hwndCanvas, SMOOTHSCROLL_TIMER_ID, SMOOTHSCROLL_DELAY_IN_MS, nullptr);
    ScheduleRepaint(win, 0);
}

void OnSelectionStop(MainWindow* win, int x, int y, bool aborted) {
    if (GetCapture() == win->hwndCanvas) {
        ReleaseCapture();
    }
    KillTimer(win->hwndCanvas, SMOOTHSCROLL_TIMER_ID);

    // update the text selection before changing the selectionRect
    if (MouseAction::SelectingText == win->mouseAction) {
        // double/triple-click set the glyph range immediately; a tiny mouse jitter
        // while the button is held still updates selectionRect.dx/dy. Only extend
        // the selection on mouse-up when the pointer actually moved (issue #5712).
        bool dragged = IsDragDistance(win->selectionRect.x, x, win->selectionRect.y, y);
        UpdateTextSelection(win, dragged);
    }

    win->selectionRect = Rect::FromXY(win->selectionRect.x, win->selectionRect.y, x, y);
    if (aborted || (MouseAction::Selecting == win->mouseAction ? win->selectionRect.IsEmpty()
                                                               : !win->CurrentTab()->selectionOnPage)) {
        DeleteOldSelectionInfo(win, true);
    } else if (win->mouseAction == MouseAction::Selecting) {
        win->CurrentTab()->selectionOnPage = SelectionOnPage::FromRectangle(win->AsFixed(), win->selectionRect);
        win->showSelection = win->CurrentTab()->selectionOnPage != nullptr;
    }
    win->selectingByWord = false;
    // refresh selection-dependent toolbar buttons once, when the selection is
    // finalized, rather than on every repaint while dragging (UpdateTextSelection
    // runs from PaintSelection on each frame, which flickered the toolbar)
    ToolbarUpdateStateForWindow(win, false);
    ScheduleRepaint(win, 0);
}
