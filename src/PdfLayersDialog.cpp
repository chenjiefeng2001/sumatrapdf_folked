/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// Turns a PDF's optional content groups (layers) on and off.
//
// A layer-aware PDF (a CAD drawing, a map, a layered manual) stores parts of
// each page in named optional content groups and says which combination is
// meant to be visible. We show the list the document itself declares, which is
// what Acrobat's layer panel shows too, rather than inventing our own.
//
// Modeless, like Sign Document, so the page updates as you click. Layer
// visibility is view state: it does not modify the document, so it is neither
// recorded as an undo step nor written into the saved file.

#include "base/Base.h"
#include "base/Win.h"
#include "base/File.h"
#include "gui/Dpi.h"

#include "gui/UIModels.h"
#include "gui/Layout.h"
#include "gui/win/WinGui.h"
#include "gui/PlatformFont.h"
#include "gui/Gfx.h"
#include "gui/VirtCtrl.h"

#include "Settings.h"
#include "AppSettings.h"
#include "GlobalPrefs.h"
#include "base/GuessFileType.h"
#include "DocController.h"
#include "EngineBase.h"
#include "EngineAll.h"
#include "DisplayModel.h"
#include "MainWindow.h"
#include "WindowTab.h"
#include "RenderCache.h"
#include "Theme.h"
#include "SumatraPDF.h"
#include "Translations.h"
#include "Toolbar.h"
#include "Commands.h"
#include "SumatraConfig.h"
#include "PdfLayersDialog.h"

struct PdfLayersWnd;

// One toggleable row of the document's layer UI. Heap-allocated and pointed
// at by the checkbox's Func0 handler, which cannot carry an argument - and Vec
// would move the elements out from under it.
struct LayerRow {
    PdfLayersWnd* wnd = nullptr;
    Checkbox* box = nullptr;
    int idx = 0;
};

static PdfLayersWnd* gLayersWnd = nullptr;

struct PdfLayersWnd : WindowBase {
    ~PdfLayersWnd() override {
        for (LayerRow* row : rows) {
            delete row;
        }
    }

    MainWindow* win = nullptr;
    WindowTab* tab = nullptr;
    VBox* vbox = nullptr;
    VirtText* statusText = nullptr;
    Vec<LayerRow*> rows;

    bool Create(MainWindow* win);
    void BuildRows();
    void OnToggled(int idx);
    void OnReset(VirtMouseEvent* ev = nullptr);
    void OnClose(VirtMouseEvent* ev = nullptr);
    // one toggle can change others (radio boxes, and checkboxes a radio box
    // turns off), so re-read every row rather than trusting the click
    void SyncFromDocument();
    void Rerender();
    void SetStatus(Str msg);
};

static void OnLayerRowToggled(LayerRow* row) {
    if (row && row->wnd) {
        row->wnd->OnToggled(row->idx);
    }
}

static void ClearLayersWnd() {
    gLayersWnd = nullptr;
}

static void OnLayersDestroy(WindowBase::DestroyEvent*) {
    ClearLayersWnd();
}

void PdfLayersWnd::Rerender() {
    if (gRenderCache && tab && win) {
        DisplayModel* dm = win->AsFixed();
        EngineBase* engine = tab->GetEngine();
        int nPages = engine->PageCount();
        for (int i = 1; i <= nPages; i++) {
            gRenderCache->Invalidate(dm, i, engine->PageMediabox(i));
        }
    }
    if (win) {
        MainWindowRerender(win);
        ToolbarUpdateStateForWindow(win, true);
    }
}

void PdfLayersWnd::SetStatus(Str msg) {
    if (statusText) {
        statusText->SetText(msg);
        statusText->Invalidate();
    }
}

void PdfLayersWnd::SyncFromDocument() {
    if (!tab) {
        return;
    }
    EngineBase* engine = tab->GetEngine();
    for (LayerRow* row : rows) {
        if (!row || !row->box) {
            continue;
        }
        PdfLayerUiItem item;
        if (!EngineMupdfGetLayerUiItem(engine, row->idx, item)) {
            continue;
        }
        row->box->SetIsEnabled(!item.locked);
        row->box->SetIsChecked(item.selected);
        HwndInvalidate(row->box->hwnd);
    }
}

void PdfLayersWnd::OnToggled(int idx) {
    if (!tab) {
        return;
    }
    // read the row's state rather than tracking it: mupdf normalizes radio
    // boxes and dependent checkboxes for us
    bool want = false;
    for (LayerRow* row : rows) {
        if (row && row->idx == idx && row->box) {
            want = row->box->IsChecked();
        }
    }
    if (!EngineMupdfSetLayerUiSelected(tab->GetEngine(), idx, want)) {
        SyncFromDocument();
        Rerender();
        return;
    }
    SyncFromDocument();
    Rerender();
}

void PdfLayersWnd::OnReset(VirtMouseEvent*) {
    if (!tab) {
        return;
    }
    EngineMupdfResetLayerUi(tab->GetEngine());
    SyncFromDocument();
    Rerender();
    SetStatus(_TRA("Reset to the document's own layers"));
}

void PdfLayersWnd::OnClose(VirtMouseEvent*) {
    Close();
}

void PdfLayersWnd::BuildRows() {
    if (!tab || !vbox) {
        return;
    }
    EngineBase* engine = tab->GetEngine();
    int n = EngineMupdfGetLayerUiCount(engine);
    PlatformFont* f = GetFont();
    bool isRtl = IsUIRtl();
    for (int i = 0; i < n; i++) {
        PdfLayerUiItem item;
        if (!EngineMupdfGetLayerUiItem(engine, i, item)) {
            continue;
        }
        int indent = DpiScale(8) + DpiScale(14) * item.depth;
        if (item.type == 0) {
            // a group heading: a plain, non-interactive line
            vbox->AddChild(NewVirtText({
                .s = item.text,
                .font = f,
                .isRtl = isRtl,
                .padding = DpiScaledInsets(DpiScale(6), 0, 0, indent),
            }));
            continue;
        }
        Checkbox::CreateArgs args;
        args.parent = hwnd;
        args.text = item.text;
        args.font = f;
        args.isRtl = isRtl;
        args.isRadio = (item.type == 2);
        args.initialState = item.selected ? Checkbox::State::Checked : Checkbox::State::Unchecked;
        auto* row = new LayerRow();
        row->wnd = this;
        row->idx = i;
        auto* box = new Checkbox();
        box->SetInsetsPt(DpiScale(2), 0, 0, indent);
        box->Create(args);
        box->SetIsEnabled(!item.locked);
        box->onStateChanged = MkFunc0<LayerRow>(OnLayerRowToggled, row);
        vbox->AddChild(box);
        row->box = box;
        rows.Append(row);
    }
    if (n == 0) {
        vbox->AddChild(NewVirtText({.s = _TRA("This document has no layers."), .font = f, .isRtl = isRtl}));
    }
}

bool PdfLayersWnd::Create(MainWindow* mainWin) {
    win = mainWin;
    tab = win ? win->CurrentTab() : nullptr;

    CreateCustomArgs args;
    args.owner = win ? win->hwndFrame : nullptr;
    args.title = _TRA("PDF Layers");
    args.visible = false;
    args.style = WS_POPUPWINDOW | WS_CAPTION | WS_SYSMENU;
    args.font = GetFont();
    args.icon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(GetAppIconID()));
    CreateCustom(args);
    if (!hwnd) {
        return false;
    }
    closeOnEsc = true;
    closeOnCtrlW = true;

    PlatformFont* f = GetFont();
    bool isRtl = IsUIRtl();
    vbox = new VBox();
    vbox->alignMain = MainAxisAlign::MainStart;
    vbox->alignCross = CrossAxisAlign::Stretch;

    statusText = NewVirtText({.s = StrL(""), .font = f, .isRtl = isRtl});
    vbox->AddChild(statusText);

    layout = new Padding(vbox, DpiScaledInsets(4, 8, 4, 8));
    // rows go between the status line and the buttons, so build them into a
    // sub-box we insert first
    BuildRows();

    HBox* btnRow = new HBox();
    btnRow->gap = f->averageCharWidth;
    btnRow->AddChild(new Spacer(0, 0), 1);
    auto* btnReset = NewThemedButton(hwnd, _TRA("Reset to Default"), f, false);
    btnReset->onClick = MkMethod1<PdfLayersWnd, VirtMouseEvent*, &PdfLayersWnd::OnReset>(this);
    btnRow->AddChild(btnReset);
    auto* btnClose = NewThemedButton(hwnd, _TRA("Close"), f, true);
    btnClose->onClick = MkMethod1<PdfLayersWnd, VirtMouseEvent*, &PdfLayersWnd::OnClose>(this);
    btnRow->AddChild(btnClose);
    vbox->AddChild(btnRow);

    LayoutAndSizeToContent(layout, DpiScale(320), 0, hwnd);
    DoLayout(HwndClientRect(hwnd).Size());
    HwndCenterDialog(hwnd, args.owner);
    UpdateTheme();
    SetStatus(_TRA("Layer visibility is not saved with the document"));
    SetIsVisible(true);
    return true;
}

// true when the document declares layers with something the user can toggle
// (a layer UI made only of group labels has nothing to offer)
bool PdfLayersAvailable(MainWindow* win) {
    if (!win || !win->IsDocLoaded()) {
        return false;
    }
    DisplayModel* dm = win->AsFixed();
    if (!dm) {
        return false;
    }
    EngineBase* engine = dm->GetEngine();
    int n = EngineMupdfGetLayerUiCount(engine);
    for (int i = 0; i < n; i++) {
        PdfLayerUiItem item;
        if (EngineMupdfGetLayerUiItem(engine, i, item) && item.type != 0) {
            return true;
        }
    }
    return false;
}

void ShowPdfLayersDialog(MainWindow* win) {
    if (!PdfLayersAvailable(win)) {
        return;
    }
    if (gLayersWnd) {
        if (IsWindow(gLayersWnd->hwnd)) {
            gLayersWnd->SetIsVisible(true);
            HwndSetFocus(gLayersWnd->hwnd);
            return;
        }
        gLayersWnd = nullptr;
    }
    auto* wnd = new PdfLayersWnd();
    wnd->onBeforeDelete = MkFunc0Void(ClearLayersWnd);
    wnd->onDestroy = MkFunc1Void<WindowBase::DestroyEvent*>(OnLayersDestroy);
    wnd->SetFont(GetAppFont());
    if (!wnd->Create(win)) {
        delete wnd;
        return;
    }
    gLayersWnd = wnd;
}

// the layers window shows one specific document; close it when that tab goes
void PdfLayersDialogOnTabClosed(WindowTab* tab) {
    if (gLayersWnd && gLayersWnd->tab == tab) {
        gLayersWnd->Close();
    }
}
