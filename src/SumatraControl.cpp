/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "base/Base.h"
#include "base/UITask.h"
#include "base/Win.h"
#include "gui/Dpi.h"

#include "gui/UIModels.h"
#include "gui/Layout.h"
#include "gui/Layout_win.h"
#include "gui/PlatformFont.h"
#include "gui/Gfx.h"
#include "gui/VirtCtrl.h"
#include "gui/win/WinGui.h"
#include "gui/win/TabsCtrl.h"

#include "Settings.h"
#include "DisplayMode.h"
#include "DocumentLayout.h"
#include "DocController.h"
#include "DocProperties.h"
#include "EngineBase.h"
#include "DisplayModel.h"
#include "RenderCache.h"
#include "Commands.h"
#include "CommandAvailability.h"
#include "GlobalPrefs.h"
#include "Flags.h"
#include "SumatraTest.h"
#include "SumatraPDF.h"
#include "MainWindow.h"
#include "WindowTab.h"
#include "FileHistory.h"
#include "Favorites.h"
#include "SelectionTranslate.h"
#include "ImageSaveCropResize.h"
#include "base/GuessFileType.h"
#include "FindWindow.h"
#include "Toolbar.h"
#include "LinkFollow.h"
#include "SelectTextKeyboard.h"
#include "HomePage.h"
#include "Notifications.h"
#include "AIChatCommon.h"
#include "SumatraDialogs.h"
#include "EditAnnotations.h"
#include "EutlTrust.h"
#include "CommandPalette.h"
#include "PdfTools.h"

extern bool gIsStartup;
TempStr FindHistoryResultTemp(int* exitCodeOut);
TempStr LinkDestHighlightResultTemp(int* exitCodeOut);

static int FontHeight(PlatformFont* font) {
    return font ? PlatformFontLineHeight(font) : 0;
}

static TempStr DpiResultTemp(Str action, int* exitCodeOut) {
    str::Builder out;
    auto finish = [&](int code) -> TempStr {
        if (exitCodeOut) {
            *exitCodeOut = code;
        }
        return ToStrTemp(out);
    };

    if (str::Eq(action, StrL("hidden"))) {
        int prevX = dpiX;
        int prevY = dpiY;
        DpiSet(240, 240);
        WindowBase w;
        CreateCustomArgs args;
        args.visible = false;
        args.title = StrL("DPI test");
        w.CreateCustom(args);
        int layoutDpi = DpiGet();
        int windowDpi = w.GetDpi();
        int fontDy = FontHeight(GetDefaultGuiFont());
        w.Destroy();
        DpiSet(prevX, prevY);
        out.Append(fmt("layout=%d window=%d font=%d\n", layoutDpi, windowDpi, fontDy));
        return finish(layoutDpi == 240 && windowDpi == 240 && fontDy >= 24 ? 0 : 1);
    }

    if (!str::Eq(action, StrL("state")) || len(gWindows) == 0) {
        out.Append(StrL("ERROR TestDpi expects hidden or state\n"));
        return finish(1);
    }
    MainWindow* win = gWindows[0];
    out.Append(fmt("frame=%d current=%d home=%d tocLabel=%d tocEdit=%d aiLabel=%d aiInput=%d aiCheckbox=%d find=%d\n",
                   win->frameDpi, DpiGet(), FontHeight(win->homeSearch ? win->homeSearch->GetFont() : nullptr),
                   FontHeight(win->tocLabel ? win->tocLabel->font : nullptr),
                   FontHeight(win->tocFilterEdit ? win->tocFilterEdit->GetFont() : nullptr),
                   FontHeight(win->aiChatLabel ? win->aiChatLabel->font : nullptr),
                   FontHeight(win->aiChatInput ? win->aiChatInput->GetFont() : nullptr),
                   FontHeight(win->aiChatCheckbox ? win->aiChatCheckbox->GetFont() : nullptr),
                   FindWindowFontHeight(win)));
    return finish(0);
}

// Silent add for -dbg-control tests (no name dialog, no settings flush).
static void AddFavoriteSilent(MainWindow* win, int pageNo) {
    if (!win || !win->IsDocLoaded() || !win->ctrl) {
        return;
    }
    WindowTab* tab = win->CurrentTab();
    if (!tab || !tab->filePath || !win->ctrl->ValidPageNo(pageNo)) {
        return;
    }
    Str path = tab->filePath;
    FileState* fs = FileHistoryFindByPath(path);
    if (!fs) {
        fs = NewFileState(path);
        FileHistoryAppend(fs);
    }
    if (!fs->favorites) {
        return;
    }
    for (Favorite* fav : *fs->favorites) {
        if (fav->pageNo == pageNo) {
            return;
        }
    }
    TempStr pageLabel = win->ctrl->GetPageLabeTemp(pageNo);
    TempStr plainLabel = fmt("%d", pageNo);
    bool needsLabel = pageLabel && !str::Eq(plainLabel, pageLabel);
    Str pl = needsLabel ? pageLabel : Str{};
    Favorite* fn = NewFavorite(pageNo, {}, pl);
    DisplayModel* dm = win->AsFixed();
    if (dm && dm->GetScrollState().page == pageNo) {
        ScrollState ss = dm->GetScrollState();
        fn->scrollPos = PointF((float)ss.x, (float)ss.y);
    }
    fs->favorites->Append(fn);
}

// Drive favorites on the already-open document for tests/issue-3744.ts.
// action: "add" | "goto" | "next" | "prev" | "page". pageNo is used by add/goto.
static TempStr FavoriteNavResultTemp(Str action, int pageNo, int* exitCodeOut) {
    str::Builder out;
    auto finish = [&](Str msg, int code) -> TempStr {
        out.Append(msg);
        out.AppendChar('\n');
        if (exitCodeOut) {
            *exitCodeOut = code;
        }
        return ToStrTemp(out);
    };

    if (len(gWindows) == 0) {
        return finish(StrL("NOTREADY no-window"), 2);
    }
    MainWindow* win = gWindows[0];
    if (!win || !win->IsDocLoaded() || !win->ctrl) {
        return finish(StrL("NOTREADY no-doc"), 2);
    }

    if (str::EqI(action, "add")) {
        if (!win->ctrl->ValidPageNo(pageNo)) {
            return finish(fmt("ERROR bad-page page=%d", pageNo), 1);
        }
        AddFavoriteSilent(win, pageNo);
    } else if (str::EqI(action, "goto")) {
        if (!win->ctrl->ValidPageNo(pageNo)) {
            return finish(fmt("ERROR bad-page page=%d", pageNo), 1);
        }
        win->ctrl->GoToPage(pageNo, true);
    } else if (str::EqI(action, "goto-fav")) {
        if (!win->ctrl->ValidPageNo(pageNo)) {
            return finish(fmt("ERROR bad-page page=%d", pageNo), 1);
        }
        FileState* fs = FileHistoryFindByPath(win->ctrl->GetFilePath());
        Favorite* fav = nullptr;
        if (fs && fs->favorites) {
            for (Favorite* f : *fs->favorites) {
                if (f->pageNo == pageNo) {
                    fav = f;
                    break;
                }
            }
        }
        if (!fav) {
            return finish(fmt("ERROR no-fav page=%d", pageNo), 1);
        }
        JumpToFavorite(win, fav);
    } else if (str::EqI(action, "next")) {
        GoToNextFavorite(win, true);
    } else if (str::EqI(action, "prev")) {
        GoToNextFavorite(win, false);
    } else if (str::EqI(action, "page")) {
        // report only
    } else {
        return finish(fmt("ERROR unknown-action action=%s", action), 1);
    }

    int cur = win->ctrl->CurrentPageNo();
    int y = -1;
    DisplayModel* dm = win->AsFixed();
    if (dm) {
        ScrollState ss = dm->GetScrollState();
        y = (int)ss.y;
        cur = ss.page;
    }
    return finish(fmt("OK page=%d y=%d", cur, y), 0);
}

// action: "get" | "r2l" | "presentation" | "fullscreen"
// Reports the current page layout and whether presentation / windowed
// fullscreen is on. presentation/fullscreen toggle that mode first.
static TempStr DisplayModeResultTemp(Str action, int* exitCodeOut) {
    str::Builder out;
    auto finish = [&](Str msg, int code) -> TempStr {
        out.Append(msg);
        out.AppendChar('\n');
        if (exitCodeOut) {
            *exitCodeOut = code;
        }
        return ToStrTemp(out);
    };

    if (len(gWindows) == 0) {
        return finish(StrL("NOTREADY no-window"), 2);
    }
    MainWindow* win = gWindows[0];
    if (!win || !win->IsDocLoaded() || !win->ctrl) {
        return finish(StrL("NOTREADY no-doc"), 2);
    }

    bool reportR2L = str::EqI(action, StrL("r2l"));
    if (!action || str::EqI(action, StrL("get")) || reportR2L) {
        // report only
    } else if (str::EqI(action, StrL("presentation"))) {
        ToggleFullScreen(win, win->AsFixed() != nullptr);
    } else if (str::EqI(action, StrL("fullscreen"))) {
        ToggleFullScreen(win, false);
    } else {
        return finish(fmt("ERROR unknown-action action=%s", action), 1);
    }

    if (reportR2L) {
        DisplayModel* dm = win->AsFixed();
        if (!dm) {
            return finish(StrL("ERROR not-fixed-page"), 1);
        }
        AppCommandCtx ctx = NewAppCommandCtx(win);
        bool available =
            GetCommandVisibility(CmdToggleMangaMode, ctx, CommandSurface::Palette) == CommandVisibility::Show;
        return finish(fmt("OK r2l=%d available=%d", dm->GetDisplayR2L() ? 1 : 0, available ? 1 : 0), 0);
    }

    Str mode = DisplayModeToString(win->ctrl->GetDisplayMode());
    Str zoomLabel;
    ZoomToString(&zoomLabel, win->ctrl->GetZoomVirtual(false), nullptr);
    TempStr res = fmt("OK mode=%s presentation=%d fullscreen=%d zoom=%s", mode, win->InPresentation() ? 1 : 0,
                      win->isFullScreen ? 1 : 0, zoomLabel);
    str::Free(zoomLabel);
    return finish(res, 0);
}

// Boxes the current page actually declares (issue #814). Optional int arg is pageNo.
static TempStr PageBoxesResultTemp(int pageNo, int* exitCodeOut) {
    str::Builder out;
    auto finish = [&](Str msg, int code) -> TempStr {
        out.Append(msg);
        out.AppendChar('\n');
        if (exitCodeOut) {
            *exitCodeOut = code;
        }
        return ToStrTemp(out);
    };

    if (len(gWindows) == 0) {
        return finish(StrL("NOTREADY no-window"), 2);
    }
    MainWindow* win = gWindows[0];
    if (!win || !win->IsDocLoaded() || !win->ctrl) {
        return finish(StrL("NOTREADY no-doc"), 2);
    }
    DisplayModel* dm = win->AsFixed();
    EngineBase* engine = dm ? dm->GetEngine() : nullptr;
    if (!engine) {
        return finish(StrL("ERROR not-fixed-page"), 1);
    }
    if (pageNo < 1) {
        pageNo = win->ctrl->CurrentPageNo();
    }
    if (!win->ctrl->ValidPageNo(pageNo)) {
        return finish(fmt("ERROR bad-page page=%d", pageNo), 1);
    }
    Vec<PdfPageBox> boxes;
    engine->GetPdfPageBoxes(pageNo, boxes);
    str::Builder line;
    line.Append(fmt("OK page=%d show=%d", pageNo, win->showPageBoxes ? 1 : 0));
    for (const PdfPageBox& box : boxes) {
        line.Append(fmt(" %s=%.2f,%.2f,%.2f,%.2f", Str(PdfPageBoxName(box.kind)), box.rect.x, box.rect.y, box.rect.dx,
                        box.rect.dy));
    }
    return finish(ToStrTemp(line), 0);
}

// Reports sidebar vs canvas client x positions so tests can check SidebarOnRight.
static TempStr SidebarLayoutResultTemp(int* exitCodeOut) {
    str::Builder out;
    auto finish = [&](Str msg, int code) -> TempStr {
        out.Append(msg);
        out.AppendChar('\n');
        if (exitCodeOut) {
            *exitCodeOut = code;
        }
        return ToStrTemp(out);
    };

    if (len(gWindows) == 0) {
        return finish(StrL("NOTREADY no-window"), 2);
    }
    MainWindow* win = gWindows[0];
    if (!win || !win->hwndFrame) {
        return finish(StrL("NOTREADY no-window"), 2);
    }

    auto clientX = [&](HWND hwnd) -> int {
        if (!hwnd || !HwndIsVisible(hwnd)) {
            return -1;
        }
        return HwndScreenToClient(win->hwndFrame, HwndWindowRect(hwnd).TL()).x;
    };

    bool pref = gGlobalPrefs && gGlobalPrefs->sidebarOnRight;
    bool tocVis = win->hwndTocBox && HwndIsVisible(win->hwndTocBox);
    bool favVis = win->hwndFavBox && HwndIsVisible(win->hwndFavBox);
    int tocX = clientX(win->hwndTocBox);
    int favX = clientX(win->hwndFavBox);
    int canvasX = clientX(win->hwndCanvas);
    return finish(fmt("OK pref=%d tocVis=%d favVis=%d tocX=%d favX=%d canvasX=%d", pref ? 1 : 0, tocVis ? 1 : 0,
                      favVis ? 1 : 0, tocX, favX, canvasX),
                  0);
}

struct LayoutProbeState {
    MainWindow* win = nullptr;
    int count = 0;
    bool active = false;
};

static LayoutProbeState gLayoutProbe;

static void LayoutProbeAfterLayout(MainWindow* win) {
    if (gLayoutProbe.active && gLayoutProbe.win == win) {
        gLayoutProbe.count++;
    }
}

static void AppendLayoutRect(str::Builder& out, Str name, bool visible, Rect rect) {
    out.Append(
        fmt("item name=%s visible=%d rect=%d,%d,%d,%d\n", name, visible ? 1 : 0, rect.x, rect.y, rect.dx, rect.dy));
}

static void AppendHwndLayoutRect(str::Builder& out, MainWindow* win, Str name, HWND hwnd) {
    Rect rect;
    if (hwnd) {
        rect = hwnd == win->hwndFrame ? HwndWindowRect(hwnd) : ChildPosWithinParent(hwnd);
    }
    AppendLayoutRect(out, name, hwnd && HwndIsVisible(hwnd), rect);
}

static void AppendLayoutTree(str::Builder& out, Str path, ILayout* layout, int depth = 0) {
    if (!layout || depth > 64) {
        return;
    }
    Rect rect = layout->lastBounds;
    Kind kind = layout->GetKind();
    Str kindName = kind ? Str(kind) : StrL("none");
    out.Append(fmt("layout path=%s kind=%s visibility=%d rect=%d,%d,%d,%d\n", path, kindName,
                   (int)layout->GetVisibility(), rect.x, rect.y, rect.dx, rect.dy));
    int n = layout->LayoutChildCount();
    for (int i = 0; i < n; i++) {
        AppendLayoutTree(out, fmt("%s/%d", path, i), layout->LayoutChildAt(i), depth + 1);
    }
}

static TempStr LayoutInfoResultTemp(Str action, int* exitCodeOut) {
    str::Builder out;
    auto finish = [&](Str msg, int code) -> TempStr {
        out.Append(msg);
        if (exitCodeOut) {
            *exitCodeOut = code;
        }
        return ToStrTemp(out);
    };

    if (len(gWindows) == 0 || !gWindows[0] || !gWindows[0]->hwndFrame) {
        return finish(StrL("NOTREADY no-window\n"), 2);
    }
    MainWindow* win = gWindows[0];
    if (!action || str::EqI(action, StrL("get"))) {
        // report only
    } else if (str::EqI(action, StrL("start")) || str::EqI(action, StrL("reset"))) {
        gLayoutProbe.win = win;
        gLayoutProbe.count = 0;
        gLayoutProbe.active = true;
        gAfterLayout = MkFunc1Void(LayoutProbeAfterLayout);
    } else if (str::EqI(action, StrL("stop"))) {
        if (gLayoutProbe.active) {
            gAfterLayout = {};
            gLayoutProbe.active = false;
        }
    } else {
        return finish(fmt("ERROR unknown-action action=%s\n", action), 1);
    }

    bool watching = gLayoutProbe.active && gLayoutProbe.win == win;
    out.Append(fmt("OK count=%d watching=%d\n", gLayoutProbe.count, watching ? 1 : 0));
    AppendHwndLayoutRect(out, win, StrL("frame"), win->hwndFrame);
    AppendHwndLayoutRect(out, win, StrL("canvas"), win->hwndCanvas);
    AppendHwndLayoutRect(out, win, StrL("toolbar"), win->hwndToolbar);
    AppendHwndLayoutRect(out, win, StrL("tabs"), win->tabsCtrl ? win->tabsCtrl->hwnd : nullptr);
    AppendHwndLayoutRect(out, win, StrL("menu"), win->hwndMenuReBar);
    AppendHwndLayoutRect(out, win, StrL("toc"), win->hwndTocBox);
    AppendHwndLayoutRect(out, win, StrL("favorites"), win->hwndFavBox);
    AppendHwndLayoutRect(out, win, StrL("aiChat"), win->hwndAiChatBox);

    AppendLayoutTree(out, StrL("chrome"), win->chromeLayout);
    AppendLayoutTree(out, StrL("frameLayout"), win->frameLayout);
    AppendLayoutTree(out, StrL("caption"), win->captionLayout);
    AppendLayoutTree(out, StrL("toc"), win->tocLayout);
    AppendLayoutTree(out, StrL("favorites"), win->favLayout);
    AppendLayoutTree(out, StrL("aiChat"), win->aiChatLayout);
    AppendLayoutTree(out, StrL("homeSearch"), win->homeSearchLayout);

    DisplayModel* dm = win->AsFixed();
    if (dm) {
        out.Append(fmt("pages count=%d spacing=%d,%d\n", dm->PageCount(), dm->pageSpacing.dx, dm->pageSpacing.dy));
        int n = std::min(dm->PageCount(), 8);
        for (int pageNo = 1; pageNo <= n; pageNo++) {
            PageInfo* pi = dm->GetPageInfo(pageNo);
            if (!pi) {
                continue;
            }
            Rect p = pi->pos;
            Rect s = pi->pageOnScreen;
            out.Append(fmt("page n=%d shown=%d pos=%d,%d,%d,%d screen=%d,%d,%d,%d\n", pageNo, pi->isShown ? 1 : 0, p.x,
                           p.y, p.dx, p.dy, s.x, s.y, s.dx, s.dy));
        }
    }
    return finish({}, 0);
}

static TempStr DocumentSignaturesResultTemp(int* exitCodeOut) {
    auto finish = [exitCodeOut](Str result, int code) -> TempStr {
        if (exitCodeOut) {
            *exitCodeOut = code;
        }
        return str::DupTemp(result);
    };
    if (len(gWindows) == 0) {
        return finish(StrL("NOTREADY no-window"), 2);
    }
    MainWindow* win = gWindows[0];
    DisplayModel* dm = win ? win->AsFixed() : nullptr;
    EngineBase* engine = dm ? dm->GetEngine() : nullptr;
    if (!engine) {
        return finish(StrL("NOTREADY no-fixed-document"), 2);
    }
#if OS_WIN
    EutlRegisterLookup();
#endif
    Props props;
    engine->GetProperties(props);
    Str sigs = GetPropValueTemp(props, DocProp::Signatures);
    if (len(sigs) == 0) {
        return finish(StrL("ERROR no-signatures"), 1);
    }
    return finish(str::DupTemp(sigs), 0);
}

static TempStr DocumentFontListResultTemp(int* exitCodeOut) {
    auto finish = [exitCodeOut](Str result, int code) -> TempStr {
        if (exitCodeOut) {
            *exitCodeOut = code;
        }
        return str::DupTemp(result);
    };
    if (len(gWindows) == 0) {
        return finish(StrL("NOTREADY no-window"), 2);
    }
    MainWindow* win = gWindows[0];
    DisplayModel* dm = win ? win->AsFixed() : nullptr;
    EngineBase* engine = dm ? dm->GetEngine() : nullptr;
    if (!engine) {
        return finish(StrL("NOTREADY no-fixed-document"), 2);
    }
    TempStr fonts = engine->GetPropertyTemp(DocProp::FontList);
    if (!fonts) {
        return finish(StrL("ERROR no-fonts"), 1);
    }
    return finish(fmt("OK fonts=%s", fonts), 0);
}

enum class ControlCmd : u16 {
    Ping = 1,
    Quit = 2,
    TestSynctex = 10,
    TestSearch = 11,
    TestDest = 12,
    TestNamedDest = 13,
    TestChm = 14,
    TestSelectionTranslate = 15,
    TestTripleClickLineSelect = 16,
    TestContextMenuSelection = 17,
    TestGoToFindMatch = 18,
    // IDs 19-21 unused (reserved on the -dbg-control wire protocol; do not renumber).
    // Assign new test commands starting at 23.
    TestInverseSearch = 22,
    TestImageResizeArrowKey = 23,
    TestFindResultPageColumnClip = 24,
    TestFileKind = 25,
    TestScrollToLink = 26,
    TestI18nErrorString = 27,
    TestPageInfoOverlay = 28,
    TestGetToc = 29,
    TestPageLinks = 30,
    TestWindowStateDuringLoad = 31,
    TestTocNavigate = 32,
    TestMarkdownTocNavigate = 33,
    TestFavoriteNav = 34,
    TestToolbarButtons = 35,
    TestKeyboardLinkFollow = 36,
    TestFindResultsOrder = 37,
    TestClickClearsSelection = 38,
    TestRectSelectionDrag = 39,
    TestSelectTextKeyboard = 40,
    TestAIChat = 41,
    TestAIChatReplay = 42,
    TestMarkdownFollowLink = 43,
    TestHomeListRows = 44,
    TestPageComments = 45,
    TestAdvSettingsRows = 46,
    TestDestZoomNav = 47,
    TestAnnotEditorLayout = 48,
    TestDisplayMode = 49,
    TestSidebarLayout = 50,
    TestCadEnhanceColors = 51,
    TestFindPageRange = 52,
    TestDocumentFontList = 53,
    WaitRenderIdle = 54,
    SetNotificationsEnabled = 55,
    TestHomeSelection = 56,
    TestImageRenderEdges = 57,
    TestInsertImage = 58,
    TestRenderPageColors = 59,
    TestListSigningCerts = 60,
    TestSignDocument = 61,
    TestGetPolicies = 62,
    TestPageBoxes = 63,
    TestDocumentSignatures = 64,
    TestCommandPalette = 65,
    TestFindHistory = 66,
    TestImageResizeEdges = 67,
    TestLinkDestHighlight = 68,
    TestConvertToImages = 69,
    TestLayout = 70,
    TestDpi = 71,
    TestPageGeometry = 72,
};

enum class ControlArgType : u16 {
    End = 0,
    Int32 = 1,
    Bytes = 2,
    String = 3,
    List = 4,
};

constexpr size_t kControlMaxRequestBytes = 16ll * 1024 * 1024;
constexpr size_t kControlMaxResponseBytes = 16ll * 1024 * 1024;
constexpr size_t kControlMaxArgBytes = 4ll * 1024 * 1024;
constexpr int kControlMaxArgDepth = 32;
constexpr int kControlMaxArgCount = 4096;
constexpr DWORD kControlIdleReadTimeoutMs = 5 * 60 * 1000;
constexpr DWORD kControlRequestReadTimeoutMs = 15 * 1000;
constexpr DWORD kControlResponseTimeoutMs = 2 * 60 * 1000;
constexpr DWORD kControlErrorWriteTimeoutMs = 5 * 1000;

struct ControlArg {
    ControlArgType type = ControlArgType::End;
    i32 intVal = 0;
    u8* bytes = nullptr;
    u32 bytesLen = 0;
    Str str;
    Vec<ControlArg*>* list = nullptr;
};

static void DeleteControlArg(ControlArg* arg) {
    if (!arg) {
        return;
    }
    free(arg->bytes);
    str::FreePtr(&arg->str);
    if (arg->list) {
        for (ControlArg* el : *arg->list) {
            DeleteControlArg(el);
        }
        delete arg->list;
    }
    delete arg;
}

enum class RenderIdleState : u8 {
    NotReady = 0,
    Busy = 1,
    Idle = 2,
};

struct ControlRequest {
    u16 cmd = 0;
    u16 reqId = 0;
    Vec<ControlArg*> args;
    str::Builder results;
    HANDLE done = nullptr;
    RenderIdleState idleState = RenderIdleState::NotReady;
    const char* parseError = nullptr;
    AtomicInt refs = 1;
    char idleInfo[320]{};
};

static void DeleteControlRequest(ControlRequest* req) {
    if (!req) {
        return;
    }
    for (ControlArg* arg : req->args) {
        DeleteControlArg(arg);
    }
    SafeCloseHandle(&req->done);
    delete req;
}

static void ReleaseControlRequest(ControlRequest* req) {
    if (req && AtomicIntDec(&req->refs) == 0) {
        DeleteControlRequest(req);
    }
}

struct PacketReader {
    const u8* data = nullptr;
    size_t size = 0;
    size_t pos = 0;

    bool ReadU16(u16& v) {
        if (data == nullptr || pos > size || size - pos < 2) {
            return false;
        }
        v = (u16)(data[pos] | (data[pos + 1] << 8));
        pos += 2;
        return true;
    }

    bool ReadU32(u32& v) {
        if (data == nullptr || pos > size || size - pos < 4) {
            return false;
        }
        v = (u32)data[pos] | ((u32)data[pos + 1] << 8) | ((u32)data[pos + 2] << 16) | ((u32)data[pos + 3] << 24);
        pos += 4;
        return true;
    }

    bool ReadBytes(u8* dst, size_t n) {
        if ((n > 0 && dst == nullptr) || pos > size || n > size - pos) {
            return false;
        }
        if (n > 0) {
            memcpy(dst, data + pos, n);
        }
        pos += n;
        return true;
    }
};

struct ControlParseBudget {
    int argCount = 0;
};

static bool AppendControlData(ControlRequest* req, Str data) {
    if (!req || data.len < 0 || len(req->results) < 0 || (size_t)len(req->results) > kControlMaxResponseBytes) {
        return false;
    }
    if ((size_t)data.len > kControlMaxResponseBytes - (size_t)len(req->results)) {
        return false;
    }
    return req->results.Append(data);
}

static void SetProtocolError(ControlRequest* req, Str msg) {
    if (!req) {
        return;
    }
    req->results.Reset();
    size_t msgLen = (size_t)std::max(msg.len, 0);
    msgLen = std::min(msgLen, kControlMaxResponseBytes - 15);
    i32 errorCode = -1;
    u32 errorCodeBytes = (u32)errorCode;
    u8 intHeader[6] = {(u8)ControlArgType::Int32,  0,
                       (u8)errorCodeBytes,         (u8)(errorCodeBytes >> 8),
                       (u8)(errorCodeBytes >> 16), (u8)(errorCodeBytes >> 24)};
    u8 stringHeader[6] = {(u8)ControlArgType::String, 0, (u8)msgLen, (u8)(msgLen >> 8), (u8)(msgLen >> 16),
                          (u8)(msgLen >> 24)};
    u8 zero = 0;
    u8 end[2] = {(u8)ControlArgType::End, 0};
    if (!AppendControlData(req, Str((char*)intHeader, dimof(intHeader))) ||
        !AppendControlData(req, Str((char*)stringHeader, dimof(stringHeader))) ||
        !AppendControlData(req, Str(msg.s, (int)msgLen)) || !AppendControlData(req, Str((char*)&zero, 1)) ||
        !AppendControlData(req, Str((char*)end, dimof(end)))) {
        req->results.Reset();
    }
}

static bool AppendArgEnd(ControlRequest* req) {
    u8 data[2] = {(u8)ControlArgType::End, 0};
    return AppendControlData(req, Str((char*)data, dimof(data)));
}

static bool AppendArgInt(ControlRequest* req, i32 v) {
    u32 n = (u32)v;
    u8 data[6] = {(u8)ControlArgType::Int32, 0, (u8)n, (u8)(n >> 8), (u8)(n >> 16), (u8)(n >> 24)};
    return AppendControlData(req, Str((char*)data, dimof(data)));
}

static bool AppendArgString(ControlRequest* req, Str str) {
    if (!str) {
        str = StrL("");
    }
    size_t n = (size_t)str.len;
    if (n > (size_t)INT_MAX || n > kControlMaxResponseBytes - 7) {
        return false;
    }
    u8 header[6] = {(u8)ControlArgType::String, 0, (u8)n, (u8)(n >> 8), (u8)(n >> 16), (u8)(n >> 24)};
    u8 zero = 0;
    return AppendControlData(req, Str((char*)header, dimof(header))) && AppendControlData(req, str) &&
           AppendControlData(req, Str((char*)&zero, 1));
}

static bool ParseArg(PacketReader& r, ControlArg** argOut, int depth, ControlParseBudget* budget, Str* error);

static bool ParseArgList(PacketReader& r, Vec<ControlArg*>* args, bool explicitCount, int depth,
                         ControlParseBudget* budget, Str* error, u16 count = 0) {
    if (!args || !budget || !error) {
        return false;
    }
    for (u16 i = 0; !explicitCount || i < count; i++) {
        ControlArg* arg = nullptr;
        if (!ParseArg(r, &arg, depth, budget, error)) {
            return false;
        }
        if (!arg) {
            if (explicitCount) {
                *error = StrL("unexpected end marker in control list");
            }
            return !explicitCount;
        }
        if (!args->Append(arg)) {
            DeleteControlArg(arg);
            *error = StrL("out of memory");
            return false;
        }
    }
    return true;
}

static bool ParseArg(PacketReader& r, ControlArg** argOut, int depth, ControlParseBudget* budget, Str* error) {
    if (!argOut || !budget || !error || depth > kControlMaxArgDepth) {
        if (error) {
            *error = StrL("control argument nesting is too deep");
        }
        return false;
    }
    u16 typeRaw = 0;
    if (!r.ReadU16(typeRaw)) {
        *error = StrL("truncated control argument");
        return false;
    }
    ControlArgType type = (ControlArgType)typeRaw;
    if (type == ControlArgType::End) {
        *argOut = nullptr;
        return true;
    }
    if (budget->argCount >= kControlMaxArgCount) {
        *error = StrL("too many control arguments");
        return false;
    }
    budget->argCount++;

    ControlArg* arg = new ControlArg();
    arg->type = type;
    if (type == ControlArgType::Int32) {
        u32 v = 0;
        if (!r.ReadU32(v)) {
            DeleteControlArg(arg);
            *error = StrL("truncated int32 control argument");
            return false;
        }
        arg->intVal = (i32)v;
    } else if (type == ControlArgType::Bytes) {
        u32 n = 0;
        if (!r.ReadU32(n)) {
            DeleteControlArg(arg);
            *error = StrL("truncated bytes control argument");
            return false;
        }
        if ((size_t)n > kControlMaxArgBytes) {
            DeleteControlArg(arg);
            *error = StrL("bytes control argument is too large");
            return false;
        }
        arg->bytes = AllocArray<u8>((int)n + 1);
        if (!arg->bytes || !r.ReadBytes(arg->bytes, n)) {
            DeleteControlArg(arg);
            *error = StrL("out of memory reading bytes control argument");
            return false;
        }
        arg->bytesLen = n;
    } else if (type == ControlArgType::String) {
        u32 n = 0;
        if (!r.ReadU32(n)) {
            DeleteControlArg(arg);
            *error = StrL("truncated string control argument");
            return false;
        }
        if ((size_t)n > kControlMaxArgBytes) {
            DeleteControlArg(arg);
            *error = StrL("string control argument is too large");
            return false;
        }
        char* strBuf = AllocArray<char>((int)n + 1);
        if (!strBuf || !r.ReadBytes((u8*)strBuf, n)) {
            DeleteControlArg(arg);
            *error = StrL("out of memory reading string control argument");
            return false;
        }
        arg->str = Str(strBuf, (int)n);
        u8 zero = 1;
        if (!r.ReadBytes(&zero, 1) || zero != 0) {
            DeleteControlArg(arg);
            *error = StrL("invalid control string terminator");
            return false;
        }
    } else if (type == ControlArgType::List) {
        u16 count = 0;
        if (!r.ReadU16(count)) {
            DeleteControlArg(arg);
            *error = StrL("truncated control list");
            return false;
        }
        if (depth >= kControlMaxArgDepth) {
            DeleteControlArg(arg);
            *error = StrL("control argument nesting is too deep");
            return false;
        }
        arg->list = new Vec<ControlArg*>();
        if (!ParseArgList(r, arg->list, true, depth + 1, budget, error, count)) {
            DeleteControlArg(arg);
            return false;
        }
    } else {
        DeleteControlArg(arg);
        *error = StrL("unknown control argument type");
        return false;
    }
    *argOut = arg;
    return true;
}

static ControlArg* ArgAt(ControlRequest* req, size_t idx, ControlArgType type) {
    if (!req || idx >= (size_t)len(req->args)) {
        return nullptr;
    }
    ControlArg* arg = req->args[(int)idx];
    if (!arg || arg->type != type) {
        return nullptr;
    }
    return arg;
}

static Str StringArg(ControlRequest* req, size_t idx) {
    ControlArg* arg = ArgAt(req, idx, ControlArgType::String);
    return arg ? arg->str : Str{};
}

static bool IntArg(ControlRequest* req, size_t idx, i32& valOut) {
    ControlArg* arg = ArgAt(req, idx, ControlArgType::Int32);
    if (!arg) {
        return false;
    }
    valOut = arg->intVal;
    return true;
}

static void AppendError(ControlRequest* req, Str msg) {
    SetProtocolError(req, msg);
}

static void AppendTestResult(ControlRequest* req, int exitCode, Str result) {
    if (!AppendArgInt(req, exitCode) || !AppendArgString(req, result) || !AppendArgEnd(req)) {
        SetProtocolError(req, StrL("control response is too large"));
    }
}

static void ExecuteControlRequest(ControlRequest* req) {
    AutoCall releaseReq(ReleaseControlRequest, req);
    switch ((ControlCmd)req->cmd) {
        case ControlCmd::Ping:
            if (!AppendArgString(req, StrL("pong")) || !AppendArgEnd(req)) {
                SetProtocolError(req, StrL("control response is too large"));
            }
            break;

        case ControlCmd::Quit:
            if (!AppendArgInt(req, 0) || !AppendArgEnd(req)) {
                SetProtocolError(req, StrL("control response is too large"));
            }
            PostAppExit();
            break;

        // A notification covers part of the document for a couple of seconds,
        // so a test that reads pixels either waits it out or turns them off.
        case ControlCmd::SetNotificationsEnabled: {
            i32 enabled = 0;
            if (!IntArg(req, 0, enabled)) {
                AppendError(req, "SetNotificationsEnabled expects int enabled");
                break;
            }
            SetNotificationsEnabled(enabled != 0);
            AppendTestResult(req, 0, enabled ? StrL("OK enabled") : StrL("OK disabled"));
            break;
        }

        case ControlCmd::TestSynctex: {
            i32 line = 0;
            Str pdf = StringArg(req, 0);
            Str src = StringArg(req, 1);
            if (!pdf || !src || !IntArg(req, 2, line)) {
                AppendError(req, "TestSynctex expects string pdf, string source, int line");
                break;
            }
            AppendTestResult(req, 0, SynctexResultTemp(pdf, src, line));
            break;
        }

        case ControlCmd::TestInverseSearch: {
            i32 page = 0, x = 0, y = 0;
            Str pdf = StringArg(req, 0);
            if (!pdf || !IntArg(req, 1, page) || !IntArg(req, 2, x) || !IntArg(req, 3, y)) {
                AppendError(req, "TestInverseSearch expects string pdf, int page, int x, int y");
                break;
            }
            AppendTestResult(req, 0, InverseSearchResultTemp(pdf, page, x, y));
            break;
        }

        case ControlCmd::TestSearch: {
            Str pdf = StringArg(req, 0);
            Str needle = StringArg(req, 1);
            Str password = StringArg(req, 2);
            if (!pdf || !needle) {
                AppendError(req, "TestSearch expects string pdf, string needle, optional string password");
                break;
            }
            if (!password && gCli) {
                password = gCli->password;
            }
            AppendTestResult(req, 0, SearchResultTemp(pdf, needle, password));
            break;
        }

        case ControlCmd::TestDest: {
            i32 destNo = 0;
            Str pdf = StringArg(req, 0);
            if (!pdf || !IntArg(req, 1, destNo)) {
                AppendError(req, "TestDest expects string pdf, int destinationNumber");
                break;
            }
            AppendTestResult(req, 0, DestResultTemp(pdf, destNo));
            break;
        }

        case ControlCmd::TestNamedDest: {
            Str pdf = StringArg(req, 0);
            Str name = StringArg(req, 1);
            if (!pdf || !name) {
                AppendError(req, "TestNamedDest expects string pdf, string name");
                break;
            }
            AppendTestResult(req, 0, NamedDestResultTemp(pdf, name));
            break;
        }

        case ControlCmd::TestChm: {
            Str chm = StringArg(req, 0);
            if (!chm) {
                AppendError(req, "TestChm expects string chmPath");
                break;
            }
            int exitCode = 0;
            Str res = ChmResultTemp(chm, &exitCode);
            AppendTestResult(req, exitCode, res);
            break;
        }

        case ControlCmd::TestSelectionTranslate: {
            i32 backend = 0;
            Str srcLang = StringArg(req, 1);
            Str dstLang = StringArg(req, 2);
            Str text = StringArg(req, 3);
            if (!IntArg(req, 0, backend) || !srcLang || !dstLang || !text) {
                AppendError(req,
                            "TestSelectionTranslate expects int backend, string srcLang, string dstLang, string text");
                break;
            }
            int exitCode = 0;
            Str res = SelectionTranslateResultTemp(backend, srcLang, dstLang, text, &exitCode);
            AppendTestResult(req, exitCode, res);
            break;
        }

        case ControlCmd::TestTripleClickLineSelect: {
            Str pdf = StringArg(req, 0);
            Str clickWord = StringArg(req, 1);
            Str expectedLine = StringArg(req, 2);
            if (!pdf || !clickWord || !expectedLine) {
                AppendError(req, "TestTripleClickLineSelect expects string pdf, string clickWord, string expectedLine");
                break;
            }
            int exitCode = 0;
            Str res = TripleClickLineSelectResultTemp(pdf, clickWord, expectedLine, &exitCode);
            AppendTestResult(req, exitCode, res);
            break;
        }

        case ControlCmd::TestContextMenuSelection: {
            Str word1 = StringArg(req, 0);
            Str word2 = StringArg(req, 1);
            Str cursorWord = StringArg(req, 2);
            if (!word1 || !word2 || !cursorWord) {
                AppendError(req, "TestContextMenuSelection expects string word1, string word2, string cursorWord");
                break;
            }
            int exitCode = 0;
            Str res = ContextMenuSelectionResultTemp(word1, word2, cursorWord, &exitCode);
            AppendTestResult(req, exitCode, res);
            break;
        }

        case ControlCmd::TestGoToFindMatch: {
            Str word = StringArg(req, 0);
            Str typed = StringArg(req, 1);
            if (!word || !typed) {
                AppendError(req, "TestGoToFindMatch expects string word, string typed");
                break;
            }
            int exitCode = 0;
            Str res = GoToFindMatchResultTemp(word, typed, &exitCode);
            AppendTestResult(req, exitCode, res);
            break;
        }

        case ControlCmd::TestImageResizeArrowKey: {
            Str imagePath = StringArg(req, 0);
            if (!imagePath) {
                AppendError(req, "TestImageResizeArrowKey expects string imagePath");
                break;
            }
            int exitCode = 0;
            Str res = ImageResizeArrowKeyResultTemp(imagePath, &exitCode);
            AppendTestResult(req, exitCode, res);
            break;
        }

        case ControlCmd::TestImageResizeEdges: {
            Str imagePath = StringArg(req, 0);
            i32 newW = 0;
            i32 newH = 0;
            if (!imagePath || !IntArg(req, 1, newW) || !IntArg(req, 2, newH)) {
                AppendError(req, "TestImageResizeEdges expects string imagePath, int newW, int newH");
                break;
            }
            int exitCode = 0;
            Str res = ImageResizeEdgesResultTemp(imagePath, newW, newH, &exitCode);
            AppendTestResult(req, exitCode, res);
            break;
        }

        case ControlCmd::TestClickClearsSelection: {
            Str word = StringArg(req, 0);
            if (!word) {
                AppendError(req, "TestClickClearsSelection expects string word");
                break;
            }
            int exitCode = 0;
            Str res = ClickClearsSelectionResultTemp(word, &exitCode);
            AppendTestResult(req, exitCode, res);
            break;
        }

        case ControlCmd::TestRectSelectionDrag: {
            Str word = StringArg(req, 0);
            if (!word) {
                AppendError(req, "TestRectSelectionDrag expects string word");
                break;
            }
            int exitCode = 0;
            Str res = RectSelectionDragResultTemp(word, &exitCode);
            AppendTestResult(req, exitCode, res);
            break;
        }

        case ControlCmd::TestFindResultsOrder: {
            Str term = StringArg(req, 0);
            if (!term) {
                AppendError(req, "TestFindResultsOrder expects string term, int startPage");
                break;
            }
            i32 startPage = 0;
            IntArg(req, 1, startPage);
            int exitCode = 0;
            Str res = FindResultsOrderResultTemp(term, startPage, &exitCode);
            AppendTestResult(req, exitCode, res);
            break;
        }

        case ControlCmd::TestFindResultPageColumnClip: {
            int exitCode = 0;
            Str res = FindResultPageColumnClipResultTemp(&exitCode);
            AppendTestResult(req, exitCode, res);
            break;
        }

        case ControlCmd::TestFileKind: {
            Str path = StringArg(req, 0);
            Str expectedKind = StringArg(req, 1);
            if (!path || !expectedKind) {
                AppendError(req, "TestFileKind expects string path, string expectedKind");
                break;
            }
            int exitCode = 0;
            Str res = FileKindResultTemp(path, expectedKind, &exitCode);
            AppendTestResult(req, exitCode, res);
            break;
        }

        case ControlCmd::TestScrollToLink: {
            i32 minDelta = 50;
            IntArg(req, 0, minDelta);
            int exitCode = 0;
            Str res = ScrollToLinkResultTemp(minDelta, &exitCode);
            AppendTestResult(req, exitCode, res);
            break;
        }

        case ControlCmd::TestI18nErrorString: {
            int exitCode = 0;
            Str res = I18nErrorStringResultTemp(&exitCode);
            AppendTestResult(req, exitCode, res);
            break;
        }

        case ControlCmd::TestPageInfoOverlay: {
            Str pathTwo = StringArg(req, 0);
            Str pathOne = StringArg(req, 1);
            if (!pathTwo || !pathOne) {
                AppendError(req, "TestPageInfoOverlay expects string pathTwoPages, string pathOnePage");
                break;
            }
            int exitCode = 0;
            Str res = PageInfoOverlayResultTemp(pathTwo, pathOne, &exitCode);
            AppendTestResult(req, exitCode, res);
            break;
        }

        case ControlCmd::TestGetToc: {
            Str path = StringArg(req, 0);
            if (!path) {
                AppendError(req, "TestGetToc expects string path");
                break;
            }
            int exitCode = 0;
            Str res = GetTocResultTemp(path, &exitCode);
            AppendTestResult(req, exitCode, res);
            break;
        }

        case ControlCmd::TestPageLinks: {
            Str path = StringArg(req, 0);
            i32 pageNo = 1;
            if (!path || !IntArg(req, 1, pageNo)) {
                AppendError(req, "TestPageLinks expects string path, int pageNo");
                break;
            }
            int exitCode = 0;
            Str res = PageLinksResultTemp(path, pageNo, &exitCode);
            AppendTestResult(req, exitCode, res);
            break;
        }

        case ControlCmd::TestInsertImage: {
            Str pdfPath = StringArg(req, 0);
            Str imagePath = StringArg(req, 1);
            if (!pdfPath || !imagePath) {
                AppendError(req, "TestInsertImage expects string pdfPath, string imagePath");
                break;
            }
            int exitCode = 0;
            Str res = ImageInsertResultTemp(pdfPath, imagePath, &exitCode);
            AppendTestResult(req, exitCode, res);
            break;
        }

        case ControlCmd::TestListSigningCerts: {
            int exitCode = 0;
            Str res = ListSigningCertsResultTemp(&exitCode);
            AppendTestResult(req, exitCode, res);
            break;
        }

        case ControlCmd::TestSignDocument: {
            Str pdfPath = StringArg(req, 0);
            Str destPath = StringArg(req, 1);
            Str thumbprint = StringArg(req, 2);
            Str certPath = StringArg(req, 3);
            Str certPassword = StringArg(req, 4);
            Str imagePath = StringArg(req, 5);
            i32 appearanceFlags = -1;
            IntArg(req, 6, appearanceFlags);
            if (!pdfPath || !destPath) {
                AppendError(req,
                            "TestSignDocument expects string pdfPath, string destPath [, thumbprint] [, certPath] [, "
                            "password] [, imagePath] [, appearanceFlags]");
                break;
            }
            int exitCode = 0;
            Str res = SignDocumentResultTemp(pdfPath, destPath, thumbprint, certPath, certPassword, imagePath,
                                             appearanceFlags, &exitCode);
            AppendTestResult(req, exitCode, res);
            break;
        }

        // What InitializePolicies actually granted. Used by the restrict.ini
        // GHSA test so it does not have to detect an IFileOpenDialog window
        // (that dialog is often hosted out-of-process and misses a 3s poll).
        case ControlCmd::TestGetPolicies: {
            str::Builder out;
            out.Append(fmt("restricted=%d\n", HasPermission(Perm::RestrictedUse) ? 1 : 0));
            out.Append(fmt("internet=%d\n", HasPermission(Perm::InternetAccess) ? 1 : 0));
            out.Append(fmt("disk=%d\n", HasPermission(Perm::DiskAccess) ? 1 : 0));
            out.Append(fmt("prefs=%d\n", HasPermission(Perm::SavePreferences) ? 1 : 0));
            out.Append(fmt("registry=%d\n", HasPermission(Perm::RegistryAccess) ? 1 : 0));
            out.Append(fmt("printer=%d\n", HasPermission(Perm::PrinterAccess) ? 1 : 0));
            out.Append(fmt("copy=%d\n", HasPermission(Perm::CopySelection) ? 1 : 0));
            out.Append(fmt("fullscreen=%d\n", HasPermission(Perm::FullscreenAccess) ? 1 : 0));
            AppendTestResult(req, 0, ToStrTemp(out));
            break;
        }

        case ControlCmd::TestRenderPageColors: {
            Str path = StringArg(req, 0);
            if (!path) {
                AppendError(req, "TestRenderPageColors expects string path");
                break;
            }
            int exitCode = 0;
            Str res = PageRenderColorsResultTemp(path, &exitCode);
            AppendTestResult(req, exitCode, res);
            break;
        }

        case ControlCmd::TestImageRenderEdges: {
            Str path = StringArg(req, 0);
            i32 zoomPercent = 100;
            i32 clipKind = 0;
            if (!path) {
                AppendError(req, "TestImageRenderEdges expects string path [, int zoomPercent] [, int clipKind]");
                break;
            }
            IntArg(req, 1, zoomPercent);
            IntArg(req, 2, clipKind);
            int exitCode = 0;
            Str res = ImageRenderEdgesResultTemp(path, zoomPercent, clipKind, &exitCode);
            AppendTestResult(req, exitCode, res);
            break;
        }

        case ControlCmd::TestCadEnhanceColors: {
            Str path = StringArg(req, 0);
            i32 pageNo = 1;
            i32 zoomPercent = 25;
            if (!path || !IntArg(req, 1, pageNo)) {
                AppendError(req, "TestCadEnhanceColors expects string path, int pageNo [, int zoomPercent]");
                break;
            }
            IntArg(req, 2, zoomPercent); // optional
            int exitCode = 0;
            Str res = CadEnhanceColorsResultTemp(path, pageNo, zoomPercent, &exitCode);
            AppendTestResult(req, exitCode, res);
            break;
        }

        case ControlCmd::TestPageComments: {
            Str path = StringArg(req, 0);
            i32 pageNo = 1;
            if (!path || !IntArg(req, 1, pageNo)) {
                AppendError(req, "TestPageComments expects string path, int pageNo");
                break;
            }
            int exitCode = 0;
            Str res = PageCommentsResultTemp(path, pageNo, &exitCode);
            AppendTestResult(req, exitCode, res);
            break;
        }

        case ControlCmd::TestWindowStateDuringLoad: {
            int exitCode = 0;
            Str res = WindowStateDuringLoadResultTemp(&exitCode);
            AppendTestResult(req, exitCode, res);
            break;
        }

        case ControlCmd::TestTocNavigate: {
            i32 destNo = 1;
            if (!IntArg(req, 0, destNo)) {
                AppendError(req, "TestTocNavigate expects int destNo (1-based)");
                break;
            }
            int exitCode = 0;
            Str res = TocNavigateResultTemp(destNo, &exitCode);
            AppendTestResult(req, exitCode, res);
            break;
        }

        case ControlCmd::TestDestZoomNav: {
            i32 destNo = 1;
            i32 startZoomPerc = 0;
            if (!IntArg(req, 0, destNo)) {
                AppendError(req, "TestDestZoomNav expects int destNo (1-based) [, int startZoomPerc]");
                break;
            }
            IntArg(req, 1, startZoomPerc); // optional
            int exitCode = 0;
            Str res = DestZoomNavResultTemp(destNo, startZoomPerc, &exitCode);
            AppendTestResult(req, exitCode, res);
            break;
        }

        case ControlCmd::TestAnnotEditorLayout: {
            i32 clientDy = 0;
            i32 selectItem = 0;
            i32 selectLast = 0;
            IntArg(req, 0, clientDy);   // optional
            IntArg(req, 1, selectItem); // optional, 1-based; -1 = select all
            IntArg(req, 2, selectLast); // optional, 1-based range end
            int exitCode = 0;
            Str res = AnnotEditorLayoutResultTemp(clientDy, selectItem, &exitCode, selectLast);
            AppendTestResult(req, exitCode, res);
            break;
        }

        case ControlCmd::TestDisplayMode: {
            Str action = StringArg(req, 0);
            int exitCode = 0;
            Str res = DisplayModeResultTemp(action, &exitCode);
            AppendTestResult(req, exitCode, res);
            break;
        }

        case ControlCmd::TestDocumentSignatures: {
            int exitCode = 0;
            Str res = DocumentSignaturesResultTemp(&exitCode);
            AppendTestResult(req, exitCode, res);
            break;
        }

        case ControlCmd::TestCommandPalette: {
            int exitCode = 0;
            Str res = CommandPaletteStateTemp(&exitCode);
            AppendTestResult(req, exitCode, res);
            break;
        }

        case ControlCmd::TestPageBoxes: {
            i32 pageNo = 0;
            IntArg(req, 0, pageNo);
            int exitCode = 0;
            Str res = PageBoxesResultTemp(pageNo, &exitCode);
            AppendTestResult(req, exitCode, res);
            break;
        }

        case ControlCmd::TestDocumentFontList: {
            int exitCode = 0;
            Str res = DocumentFontListResultTemp(&exitCode);
            AppendTestResult(req, exitCode, res);
            break;
        }

        case ControlCmd::TestFindPageRange: {
            Str pdf = StringArg(req, 0);
            Str needle = StringArg(req, 1);
            i32 first = 0;
            i32 last = 0;
            IntArg(req, 2, first);
            IntArg(req, 3, last);
            Str spec = StringArg(req, 4); // optional "3,4-6,18-"
            if (!pdf || !needle) {
                AppendError(
                    req, "TestFindPageRange expects string pdf, string needle [, int first, int last [, string spec]]");
                break;
            }
            int exitCode = 0;
            Str res = FindPageRangeResultTemp(pdf, needle, first, last, spec, &exitCode);
            AppendTestResult(req, exitCode, res);
            break;
        }

        case ControlCmd::TestSidebarLayout: {
            int exitCode = 0;
            Str res = SidebarLayoutResultTemp(&exitCode);
            AppendTestResult(req, exitCode, res);
            break;
        }

        case ControlCmd::TestLayout: {
            Str action = StringArg(req, 0);
            int exitCode = 0;
            Str res = LayoutInfoResultTemp(action, &exitCode);
            AppendTestResult(req, exitCode, res);
            break;
        }

        case ControlCmd::TestDpi: {
            Str action = StringArg(req, 0);
            int exitCode = 0;
            Str res = DpiResultTemp(action, &exitCode);
            AppendTestResult(req, exitCode, res);
            break;
        }

        case ControlCmd::TestMarkdownTocNavigate: {
            i32 destNo = 0;
            i32 minScrollY = 1;
            if (!IntArg(req, 0, destNo) || !IntArg(req, 1, minScrollY)) {
                AppendError(req, "TestMarkdownTocNavigate expects int destNo, int minScrollY");
                break;
            }
            int exitCode = 0;
            Str res = MarkdownTocNavigateResultTemp(destNo, minScrollY, &exitCode);
            AppendTestResult(req, exitCode, res);
            break;
        }

        case ControlCmd::TestMarkdownFollowLink: {
            Str href = StringArg(req, 0);
            i32 follow = 0;
            if (!IntArg(req, 1, follow)) {
                AppendError(req, "TestMarkdownFollowLink expects string href, int follow");
                break;
            }
            int exitCode = 0;
            Str res = MarkdownFollowLinkResultTemp(href, follow != 0, &exitCode);
            AppendTestResult(req, exitCode, res);
            break;
        }

        case ControlCmd::TestLinkDestHighlight: {
            int exitCode = 0;
            Str res = LinkDestHighlightResultTemp(&exitCode);
            AppendTestResult(req, exitCode, res);
            break;
        }

        case ControlCmd::TestConvertToImages: {
            Str templatePath = StringArg(req, 0);
            Str pagesSpec = StringArg(req, 1);
            if (!templatePath || !pagesSpec) {
                AppendError(req, "TestConvertToImages expects string templatePath, string pages");
                break;
            }
            int exitCode = 0;
            Str res = ConvertPagesToImagesResultTemp(templatePath, pagesSpec, &exitCode);
            AppendTestResult(req, exitCode, res);
            break;
        }

        case ControlCmd::TestFindHistory: {
            int exitCode = 0;
            Str res = FindHistoryResultTemp(&exitCode);
            AppendTestResult(req, exitCode, res);
            break;
        }

        case ControlCmd::TestHomeSelection: {
            int exitCode = 0;
            Str res = HomeSelectionResultTemp(&exitCode);
            AppendTestResult(req, exitCode, res);
            break;
        }

        case ControlCmd::TestHomeListRows: {
            int exitCode = 0;
            Str res = HomeListRowsResultTemp(&exitCode);
            AppendTestResult(req, exitCode, res);
            break;
        }

        case ControlCmd::TestAdvSettingsRows: {
            Str action = StringArg(req, 0);
            i32 arg = 0;
            IntArg(req, 1, arg); // optional; only "scroll" uses it
            if (!action) {
                AppendError(req, "TestAdvSettingsRows expects string action [, int rows]");
                break;
            }
            int exitCode = 0;
            Str res = AdvSettingsRowsResultTemp(action, arg, &exitCode);
            AppendTestResult(req, exitCode, res);
            break;
        }

        case ControlCmd::TestFavoriteNav: {
            Str action = StringArg(req, 0);
            i32 pageNo = 0;
            IntArg(req, 1, pageNo); // optional for next/prev/page
            if (!action) {
                AppendError(req, "TestFavoriteNav expects string action [, int pageNo]");
                break;
            }
            int exitCode = 0;
            Str res = FavoriteNavResultTemp(action, pageNo, &exitCode);
            AppendTestResult(req, exitCode, res);
            break;
        }

        case ControlCmd::TestToolbarButtons: {
            int exitCode = 0;
            Str res = ToolbarButtonsResultTemp(&exitCode);
            AppendTestResult(req, exitCode, res);
            break;
        }

        case ControlCmd::TestKeyboardLinkFollow: {
            int exitCode = 0;
            Str res = KeyboardLinkFollowResultTemp(&exitCode);
            AppendTestResult(req, exitCode, res);
            break;
        }

        case ControlCmd::TestSelectTextKeyboard: {
            int exitCode = 0;
            Str res = SelectTextKeyboardResultTemp(&exitCode);
            AppendTestResult(req, exitCode, res);
            break;
        }

        case ControlCmd::TestAIChat: {
            i32 backend = 0;
            Str filePath = StringArg(req, 1);
            Str message = StringArg(req, 2);
            if (!IntArg(req, 0, backend) || !filePath || !message) {
                AppendError(req, "TestAIChat expects int backend, string filePath, string message");
                break;
            }
            int exitCode = 0;
            Str res = AIChatTestResultTemp(backend, filePath, message, &exitCode);
            AppendTestResult(req, exitCode, res);
            break;
        }

        case ControlCmd::TestAIChatReplay: {
            Str userMsg = StringArg(req, 0);
            Str response = StringArg(req, 1);
            if (!userMsg || !response) {
                AppendError(req, "TestAIChatReplay expects string userMsg, string response");
                break;
            }
            int exitCode = 0;
            Str res = AIChatTestReplayResultTemp(userMsg, response, &exitCode);
            AppendTestResult(req, exitCode, res);
            break;
        }

        case ControlCmd::TestPageGeometry: {
            Str path = StringArg(req, 0);
            i32 passes = 3;
            if (!path) {
                AppendError(req, "TestPageGeometry expects string path, optional int passes");
                break;
            }
            IntArg(req, 1, passes);
            int exitCode = 0;
            Str geoRes = PageGeometryResultTemp(path, passes, &exitCode);
            AppendTestResult(req, exitCode, geoRes);
            break;
        }

        default:
            AppendError(req, "unknown control command");
            break;
    }
    SetEvent(req->done);
}

// Snapshot for WaitRenderIdle. Must run on the UI thread: window/doc state
// and the cache walk both belong there. Does not block; the control thread
// polls so WM_PAINT can still request missing tiles.
static void SnapshotRenderIdle(ControlRequest* req) {
    AutoCall releaseReq(ReleaseControlRequest, req);
    req->idleState = RenderIdleState::NotReady;
    req->idleInfo[0] = 0;
    if (gIsStartup) {
        // LoadOnStartup applies -zoom after the first paint; a snapshot
        // during that window would see the default-zoom tiles as "done"
        str::BufSet(Str(req->idleInfo, dimof(req->idleInfo)), StrL("startup"));
        SetEvent(req->done);
        return;
    }
    if (len(gWindows) == 0) {
        str::BufSet(Str(req->idleInfo, dimof(req->idleInfo)), StrL("no-window"));
        SetEvent(req->done);
        return;
    }
    MainWindow* win = gWindows[0];
    if (!win || !win->IsDocLoaded()) {
        str::BufSet(Str(req->idleInfo, dimof(req->idleInfo)), StrL("no-doc"));
        SetEvent(req->done);
        return;
    }
    DisplayModel* dm = win->AsFixed();
    if (!dm) {
        // ebook / CHM / etc.: nothing in RenderCache to wait for
        req->idleState = RenderIdleState::Idle;
        str::BufSet(Str(req->idleInfo, dimof(req->idleInfo)), StrL("no-fixed"));
        SetEvent(req->done);
        return;
    }
    // Paint first: that's what queues missing target tiles. Checking the
    // cache before this paint sees a leftover preview and no in-flight work.
    // DrainQueue runs RenderFinished from tiles that completed during the
    // paint; that posts another repaint which may request the next resolution.
    if (win->hwndCanvas) {
        InvalidateRect(win->hwndCanvas, nullptr, FALSE);
        UpdateWindow(win->hwndCanvas);
    }
    uitask::DrainQueue();
    if (win->hwndCanvas) {
        InvalidateRect(win->hwndCanvas, nullptr, FALSE);
        UpdateWindow(win->hwndCanvas);
    }
    float zoomV = dm->GetZoomVirtual(true);
    int pageNo = dm->FirstVisiblePageNo();
    if (pageNo < 1) {
        pageNo = 1;
    }
    float zoomR = dm->GetZoomReal(pageNo);
    USHORT res = gRenderCache ? gRenderCache->GetTileRes(dm, pageNo) : (USHORT)0;
    Size vp = dm->GetViewPort().Size();
    Str whyNot;
    bool busy = gRenderCache && gRenderCache->IsBusyFor(dm);
    bool ready = false;
    // LoadDocument Relayouts before the canvas has a real size; fit zoom then
    // stays unset and no page is visible. That is not idle (issue-1203).
    if (dm->zoomReal < 0.01f || dm->GetCanvasSize().IsEmpty() || vp.IsEmpty()) {
        whyNot = StrL("no-layout");
    } else {
        ready = gRenderCache && !busy && gRenderCache->VisibleTargetTilesReady(dm, &whyNot);
    }
    if (busy) {
        whyNot = StrL("rendering");
    }
    if (win->scrollAnimActive) {
        whyNot = StrL("scrolling");
        ready = false;
    }
    int nQ = gRenderCache ? gRenderCache->requestCount : -1;
    TempStr busyInfo = gRenderCache ? gRenderCache->BusyInfoTemp(dm) : (TempStr) "";
    str::BufSet(Str(req->idleInfo, dimof(req->idleInfo)),
                fmt("zoomV=%.1f zoomR=%.3f res=%d vp=%dx%d ready=%d q=%d why=%s %s", zoomV, zoomR, (int)res, vp.dx,
                    vp.dy, ready ? 1 : 0, nQ, whyNot, busyInfo));
    req->idleState = ready ? RenderIdleState::Idle : (gRenderCache ? RenderIdleState::Busy : RenderIdleState::NotReady);
    SetEvent(req->done);
}

static DWORD ControlTimeRemaining(u64 deadline) {
    u64 now = GetTickCount64();
    if (now >= deadline) {
        return 0;
    }
    u64 remaining = deadline - now;
    return remaining > MAXDWORD ? MAXDWORD : (DWORD)remaining;
}

static void CancelControlIo(HANDLE h, OVERLAPPED& ov) {
    CancelIoEx(h, &ov);
    DWORD transferred = 0;
    GetOverlappedResult(h, &ov, &transferred, TRUE);
}

// Block on the control thread until visible tiles are cached at target
// resolution, or until timeoutMs. Optional first int arg is the timeout.
static bool RunWaitRenderIdle(ControlRequest* req) {
    i32 timeoutMs = 15000;
    IntArg(req, 0, timeoutMs);
    timeoutMs = std::max(timeoutMs, 1);
    timeoutMs = std::min(timeoutMs, 60000);
    u64 deadline = GetTickCount64() + (u64)timeoutMs;
    for (;;) {
        ResetEvent(req->done);
        AtomicIntInc(&req->refs);
        if (!uitask::Post(MkFunc0<ControlRequest>(SnapshotRenderIdle, req), "WaitRenderIdle")) {
            ReleaseControlRequest(req);
            return false;
        }
        DWORD wait = WaitForSingleObject(req->done, ControlTimeRemaining(deadline));
        if (wait != WAIT_OBJECT_0) {
            if (WaitForSingleObject(req->done, 0) != WAIT_OBJECT_0) {
                return false;
            }
        }
        if (req->idleState == RenderIdleState::Idle) {
            AppendTestResult(req, 0, req->idleInfo[0] ? Str(req->idleInfo) : StrL("idle"));
            return true;
        }
        if (GetTickCount64() >= deadline) {
            Str kind = req->idleState == RenderIdleState::NotReady ? StrL("timeout-notready") : StrL("timeout-busy");
            AppendTestResult(req, 1, req->idleInfo[0] ? fmt("%s %s", kind, Str(req->idleInfo)) : kind);
            return true;
        }
        DWORD sleepMs = std::min<DWORD>(20, ControlTimeRemaining(deadline));
        if (sleepMs > 0) {
            Sleep(sleepMs);
        }
    }
}

static bool ReadExact(HANDLE h, void* data, DWORD n, u64 deadline, bool* anyBytes = nullptr) {
    if (anyBytes) {
        *anyBytes = false;
    }
    if (n == 0) {
        return true;
    }
    if (!h || !data) {
        return false;
    }
    u8* bytes = (u8*)data;
    DWORD total = 0;
    while (total < n) {
        OVERLAPPED ov{};
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!ov.hEvent) {
            return false;
        }
        BOOL ok = ReadFile(h, bytes + total, n - total, nullptr, &ov);
        if (!ok) {
            DWORD err = GetLastError();
            if (err != ERROR_IO_PENDING) {
                CloseHandle(ov.hEvent);
                return false;
            }
            if (WaitForSingleObject(ov.hEvent, ControlTimeRemaining(deadline)) != WAIT_OBJECT_0) {
                CancelControlIo(h, ov);
                CloseHandle(ov.hEvent);
                return false;
            }
        }
        DWORD nRead = 0;
        if (!GetOverlappedResult(h, &ov, &nRead, FALSE) || nRead == 0 || nRead > n - total) {
            CloseHandle(ov.hEvent);
            return false;
        }
        CloseHandle(ov.hEvent);
        total += nRead;
        if (anyBytes) {
            *anyBytes = true;
        }
    }
    return true;
}

static bool WriteExact(HANDLE h, Str data, u64 deadline) {
    if (data.len == 0) {
        return true;
    }
    if (!h || !data.s || data.len < 0) {
        return false;
    }
    const u8* bytes = (const u8*)data.s;
    int total = 0;
    while (total < data.len) {
        OVERLAPPED ov{};
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!ov.hEvent) {
            return false;
        }
        BOOL ok = WriteFile(h, bytes + total, (DWORD)(data.len - total), nullptr, &ov);
        if (!ok) {
            DWORD err = GetLastError();
            if (err != ERROR_IO_PENDING) {
                CloseHandle(ov.hEvent);
                return false;
            }
            if (WaitForSingleObject(ov.hEvent, ControlTimeRemaining(deadline)) != WAIT_OBJECT_0) {
                CancelControlIo(h, ov);
                CloseHandle(ov.hEvent);
                return false;
            }
        }
        DWORD nWritten = 0;
        if (!GetOverlappedResult(h, &ov, &nWritten, FALSE) || nWritten == 0 || nWritten > (u32)(data.len - total)) {
            CloseHandle(ov.hEvent);
            return false;
        }
        CloseHandle(ov.hEvent);
        total += (int)nWritten;
    }
    return true;
}

static ControlRequest* NewControlError(u16 reqId, Str error) {
    auto* req = new ControlRequest();
    req->reqId = reqId;
    req->parseError = error.s;
    return req;
}

static ControlRequest* ReadControlRequest(HANDLE h) {
    u8 firstSizeByte = 0;
    bool gotBytes = false;
    u64 idleDeadline = GetTickCount64() + kControlIdleReadTimeoutMs;
    if (!ReadExact(h, &firstSizeByte, 1, idleDeadline, &gotBytes)) {
        return gotBytes ? NewControlError(0, StrL("truncated control request size")) : nullptr;
    }

    u64 requestDeadline = GetTickCount64() + kControlRequestReadTimeoutMs;
    u8 remainingSize[3]{};
    if (!ReadExact(h, remainingSize, dimof(remainingSize), requestDeadline, &gotBytes)) {
        return NewControlError(0, StrL("truncated control request size"));
    }
    u32 size = (u32)firstSizeByte | ((u32)remainingSize[0] << 8) | ((u32)remainingSize[1] << 16) |
               ((u32)remainingSize[2] << 24);
    if (size < 4 || (size_t)size > kControlMaxRequestBytes) {
        return NewControlError(0, StrL("invalid control request size"));
    }

    u8 header[4]{};
    if (!ReadExact(h, header, dimof(header), requestDeadline, &gotBytes)) {
        return NewControlError(0, StrL("truncated control request header"));
    }
    PacketReader headerReader{header, dimof(header)};
    u16 cmd = 0;
    u16 reqId = 0;
    if (!headerReader.ReadU16(cmd) || !headerReader.ReadU16(reqId)) {
        return NewControlError(0, StrL("invalid control request header"));
    }

    ControlRequest* req = new ControlRequest();
    req->cmd = cmd;
    req->reqId = reqId;
    AtomicIntInc(&gAllowAllocFailure);
    AutoCall allowAllocFailure(AtomicIntDec, &gAllowAllocFailure);
    u8* data = AllocArray<u8>((int)size);
    if (!data) {
        req->parseError = "out of memory reading control request";
        return req;
    }
    memcpy(data, header, sizeof(header));
    if (!ReadExact(h, data + sizeof(header), size - sizeof(header), requestDeadline, &gotBytes)) {
        req->parseError = "truncated control request payload";
        free(data);
        return req;
    }

    PacketReader r{data, size};
    r.pos = sizeof(header);
    ControlParseBudget budget;
    Str parseError;
    bool ok = ParseArgList(r, &req->args, false, 0, &budget, &parseError) && r.pos == r.size;
    if (!ok) {
        req->parseError = parseError.s ? parseError.s : "malformed control request";
    } else {
        req->done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!req->done) {
            req->parseError = "out of memory creating control request event";
        }
    }
    free(data);
    return req;
}

static bool WriteControlResponse(HANDLE h, ControlRequest* req, u64 deadline) {
    if (!h || !req) {
        return false;
    }
    if (len(req->results) < 2 || (size_t)len(req->results) > kControlMaxResponseBytes - 2) {
        SetProtocolError(req, StrL("control response is too large"));
    }
    Str results = ToStr(req->results);
    if (len(results) < 2 || (u8)results.s[len(results) - 2] != (u8)ControlArgType::End ||
        (u8)results.s[len(results) - 1] != 0) {
        SetProtocolError(req, StrL("malformed control response"));
        results = ToStr(req->results);
    }
    if (len(results) < 2) {
        return false;
    }

    u8 reqId[2] = {(u8)req->reqId, (u8)(req->reqId >> 8)};
    str::Builder payload;
    if (!payload.Append(Str((char*)reqId, dimof(reqId))) || !payload.Append(results) ||
        (size_t)len(payload) > kControlMaxResponseBytes) {
        return false;
    }
    u32 packetSize = (u32)len(payload);
    u8 packetHeader[4] = {(u8)packetSize, (u8)(packetSize >> 8), (u8)(packetSize >> 16), (u8)(packetSize >> 24)};
    str::Builder packet;
    if (!packet.Append(Str((char*)packetHeader, dimof(packetHeader))) || !packet.Append(ToStr(payload))) {
        return false;
    }
    return WriteExact(h, ToStr(packet), deadline);
}

static void ProcessControlConnection(HANDLE h) {
    for (;;) {
        ControlRequest* req = ReadControlRequest(h);
        if (!req) {
            return;
        }
        if (req->parseError) {
            AppendError(req, Str(req->parseError));
        } else {
            u64 responseDeadline = GetTickCount64() + kControlResponseTimeoutMs;
            bool safeToDelete = false;
            // WaitRenderIdle polls on this thread so the UI thread stays free to
            // paint (and thereby request the tiles we are waiting for)
            bool runOnControlThread = (ControlCmd)req->cmd == ControlCmd::TestAIChat ||
                                      (ControlCmd)req->cmd == ControlCmd::TestSelectionTranslate;
            if ((ControlCmd)req->cmd == ControlCmd::WaitRenderIdle) {
                safeToDelete = RunWaitRenderIdle(req);
            } else if (runOnControlThread) {
                AtomicIntInc(&req->refs);
                ExecuteControlRequest(req);
                safeToDelete = true;
            } else {
                AtomicIntInc(&req->refs);
                if (!uitask::Post(MkFunc0<ControlRequest>(ExecuteControlRequest, req), "SumatraControl")) {
                    ReleaseControlRequest(req);
                    safeToDelete = true;
                } else {
                    DWORD wait = WaitForSingleObject(req->done, ControlTimeRemaining(responseDeadline));
                    safeToDelete = wait == WAIT_OBJECT_0 || WaitForSingleObject(req->done, 0) == WAIT_OBJECT_0;
                }
            }
            if (!safeToDelete) {
                ReleaseControlRequest(req);
                return;
            }
            bool ok = WriteControlResponse(h, req, GetTickCount64() + kControlResponseTimeoutMs);
            ReleaseControlRequest(req);
            if (!ok) {
                return;
            }
            continue;
        }
        bool ok = WriteControlResponse(h, req, GetTickCount64() + kControlErrorWriteTimeoutMs);
        ReleaseControlRequest(req);
        if (!ok) {
            return;
        }
    }
}

static WStr FullPipeNameOwned(Str pipeName) {
    if (str::StartsWith(pipeName, StrL(R"(\\.\pipe\)"))) {
        return ToWStr(pipeName);
    }
    TempStr fullName = str::JoinTemp(StrL(R"(\\.\pipe\)"), pipeName);
    return ToWStr(fullName);
}

struct ControlThreadArg {
    Str pipeName;
};

static bool ConnectControlPipe(HANDLE pipe) {
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent) {
        return false;
    }
    BOOL connected = ConnectNamedPipe(pipe, &ov);
    if (connected) {
        CloseHandle(ov.hEvent);
        return true;
    }
    DWORD err = GetLastError();
    if (err == ERROR_PIPE_CONNECTED) {
        CloseHandle(ov.hEvent);
        return true;
    }
    if (err != ERROR_IO_PENDING) {
        CloseHandle(ov.hEvent);
        return false;
    }
    DWORD wait = WaitForSingleObject(ov.hEvent, INFINITE);
    DWORD transferred = 0;
    BOOL ok = wait == WAIT_OBJECT_0 && GetOverlappedResult(pipe, &ov, &transferred, FALSE);
    if (!ok) {
        CancelControlIo(pipe, ov);
    }
    CloseHandle(ov.hEvent);
    return ok != FALSE;
}

static void SumatraControlThread(ControlThreadArg* arg) {
    WStr pipeNameW = FullPipeNameOwned(arg->pipeName);
    str::FreePtr(&arg->pipeName);
    delete arg;

    for (;;) {
        HANDLE pipe =
            CreateNamedPipeW(pipeNameW.s, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                             PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 64 * 1024,
                             64 * 1024, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) {
            logf("CreateNamedPipeW failed for control pipe, err=%u\n", (unsigned)GetLastError());
            return;
        }
        bool connected = ConnectControlPipe(pipe);
        if (connected) {
            ProcessControlConnection(pipe);
        }
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }
}

void StartSumatraControl(Str pipeName) {
    if (len(pipeName) == 0) {
        return;
    }
    auto* arg = new ControlThreadArg{str::Dup(pipeName)};
    RunAsync(MkFunc0(SumatraControlThread, arg), "SumatraControl");
}
