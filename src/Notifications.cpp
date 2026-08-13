/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "base/Base.h"
#include "base/Win.h"
#include "base/ScopedWin.h"
#include "base/Dpi.h"
#include "base/UITask.h"

#include "wingui/UIModels.h"
#include "wingui/Layout.h"
#include "wingui/WinGui.h"

#include "Settings.h"
#include "AppSettings.h"
#include "SumatraPdf.h"

#include "Notifications.h"
#include "TipText.h"
#include "Theme.h"
#include "HardwareProfile.h"
#include "wingui/Renderer.h"

#include "base/Log.h"

using Gdiplus::Graphics;
using Gdiplus::Pen;
using Gdiplus::SolidBrush;

// defined in MainWindow.cpp
HWND GetHwndForNotification();

struct StrNode {
    StrNode* next;
    Str s;
};

static StrNode* gDelayedNotifications = nullptr;

Kind kNotifCursorPos = "cursorPosHelper";
Kind kNotifActionResponse = "responseToAction";
Kind kNotifPageInfo = "pageInfoHelper";
// can have multiple of those
Kind kNotifAdHoc = "notifAdHoc";

constexpr int kPadding = 6;
constexpr int kTopLeftMargin = 8;

constexpr UINT_PTR kNotifTimerTimeoutId = 1;
constexpr UINT_PTR kNotifTimerDelayId = 2;
constexpr UINT_PTR kNotifTimerSlideInId = 3;

struct NotificationWnd : Wnd {
    NotificationWnd() = default;
    ~NotificationWnd() override;

    HWND Create(const NotificationCreateArgs&);

    void OnPaint(HDC hdc, PAINTSTRUCT* ps) override;
    void OnTimer(UINT_PTR event_id) override;
    LRESULT WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) override;

    void UpdateMessage(Str msg, int timeoutMs = 0, bool highlight = false);

    bool HasProgress() const { return progressPerc >= 0; }
    // keepWidth=true (default): called from UpdateMessage when the content
    // changed; the window keeps its previous width/height so progress-style
    // updates don't flicker. keepWidth=false: called from RelayoutNotifications
    // when the parent window was resized; the notification re-wraps against the
    // new canvas width in both directions (wider expands, narrower re-wraps).
    void Layout(Str message, bool keepWidth = true);

    int timeoutMs = kNotifDefaultTimeOut; // 0 means no timeout

    bool highlight = false; // TODO: should really be a color
    bool noClose = false;

    NotificationWndRemoved wndRemovedCb;

    // there can only be a single notification of a given group
    Kind groupId = nullptr;

    // if set, only shown while this is the active tab (see
    // ShowNotificationsForActiveTab)
    WindowTab* tab = nullptr;

    // which canvas corner to anchor to, and distance from the edges
    NotifCorner corner = NotifCorner::TopLeft;
    int xMargin = kNotifDefaultMargin;
    int yMargin = kNotifDefaultMargin;

    // to reduce flicker, we might ask the window to shrink the size less often
    // (notifcation windows are only shrunken if by less than factor shrinkLimit)
    float shrinkLimit = 1.0f;

    int progressPerc = -1;
    int delayInMs = 0;
    UINT_PTR delayTimerId = 0;

    // Phase 3: slide-in animation state. When a notification first becomes
    // visible it slides from an offset position to its final spot over ~150ms,
    // driven by kNotifTimerSlideInId. slideFrom/slideTo are in parent
    // (canvas) coordinates.
    bool isSlidingIn = false;
    float slideT = 0; // 0..1 eased progress
    Rect slideFrom;
    Rect slideTo;
    UINT_PTR slideTimerId = 0;

    // message parsed for the extended tip syntax (links, Key/ shortcuts);
    // drawRich is true when it contains clickable links
    ParsedTip parsedMsg;
    bool drawRich = false;

    Rect rTxt;
    Rect rClose;
    Rect rProgress;
    // DT_* format for drawing the message, set in Layout()
    uint txtFmt = DT_SINGLELINE | DT_NOPREFIX;

    // width of the parent (canvas) client area at the last Layout(); -1 until
    // first layout. RelayoutNotifications uses it to detect a parent resize and
    // re-lay-out the notification against the new width (elastic width).
    int lastParentDx = -1;
};

constexpr int kMaxNotifs = 128;
static NotificationWnd* gNotifs[kMaxNotifs];
static int gNotifsCount = 0;

static int GetForHwnd(HWND hwnd, NotificationWnd* wnds[kMaxNotifs]) {
    int n = 0;
    for (int i = 0; i < gNotifsCount; i++) {
        auto* wnd = gNotifs[i];
        HWND parent = HwndGetParent(wnd->hwnd);
        if (parent == hwnd) {
            wnds[n++] = wnd;
        }
    }
    return n;
}

static int NotificationIndexOf(NotificationWnd* wnd) {
    for (int i = 0; i < gNotifsCount; i++) {
        if (gNotifs[i] == wnd) {
            return i;
        }
    }
    return -1;
}

static int GetForSameHwnd(NotificationWnd* wnd, NotificationWnd* wnds[kMaxNotifs]) {
    HWND parent = GetParent(wnd->hwnd);
    return GetForHwnd(parent, wnds);
}

// Phase 3: slide-in animation for a notification that just became visible.
// The window has already been positioned at its final spot by RelayoutNotifications;
// we record that spot, offset the window back, and animate it forward via a timer.
static void StartSlideIn(NotificationWnd* wnd) {
    if (!AnimationsEnabled()) {
        return;
    }
    HWND parent = GetParent(wnd->hwnd);
    Rect to = MapRectToWindow(WindowRect(wnd->hwnd), HWND_DESKTOP, parent);
    Rect from = to;
    bool atBottom = (wnd->corner == NotifCorner::BottomLeft) || (wnd->corner == NotifCorner::BottomRight);
    bool atRight = (wnd->corner == NotifCorner::TopRight) || (wnd->corner == NotifCorner::BottomRight);
    int offset = DpiScale(wnd->hwnd, 20);
    if (atBottom) {
        from.y += offset;
    } else {
        from.y -= offset;
    }
    if (atRight) {
        from.x += offset;
    } else {
        from.x -= offset;
    }
    wnd->isSlidingIn = true;
    wnd->slideT = 0;
    wnd->slideFrom = from;
    wnd->slideTo = to;
    uint flags = SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE;
    SetWindowPos(wnd->hwnd, nullptr, from.x, from.y, 0, 0, flags);
    wnd->slideTimerId = SetTimer(wnd->hwnd, kNotifTimerSlideInId, 16, nullptr);
}

// hwndCanvas is the parent of the notification windows
void RelayoutNotifications(HWND hwndCanvas) {
    NotificationWnd* wnds[kMaxNotifs];
    int nWnds = GetForHwnd(hwndCanvas, wnds);
    if (nWnds == 0) {
        return;
    }

    Rect frame = ClientRect(hwndCanvas);
    int dyPadding = DpiScale(hwndCanvas, kPadding);
    bool isRtl = IsUIRtl();
    // running vertical offset per corner so multiple notifications in the same
    // corner stack toward the opposite edge
    int yOffset[4] = {};
    for (int i = 0; i < nWnds; i++) {
        NotificationWnd* wnd = wnds[i];
        if (wnd->delayTimerId != 0) {
            // still in delay period, not yet visible
            continue;
        }
        if (!IsWindowVisible(wnd->hwnd)) {
            // hidden because it's tied to a non-active tab; don't reserve space
            continue;
        }
        int xMargin = DpiScale(hwndCanvas, wnd->xMargin);
        int yMargin = DpiScale(hwndCanvas, wnd->yMargin);
        Rect rect = WindowRect(wnd->hwnd);
        // re-lay-out against the current canvas width so notifications are
        // elastic in both directions: re-wrap when the window was made narrower
        // (issue #2916) and re-expand when it was made wider. keepWidth=false so
        // a stale size kept by the anti-flicker logic doesn't stick. Layout()
        // clamps the window to the canvas width, so it always fits.
        if (frame.dx != wnd->lastParentDx) {
            wnd->Layout(HwndGetTextTemp(wnd->hwnd), false);
            rect = WindowRect(wnd->hwnd);
        }

        NotifCorner corner = wnd->corner;
        bool atRight = (corner == NotifCorner::TopRight) || (corner == NotifCorner::BottomRight);
        bool atBottom = (corner == NotifCorner::BottomLeft) || (corner == NotifCorner::BottomRight);
        if (isRtl) {
            atRight = !atRight; // mirror horizontally for right-to-left UI
        }
        int x = atRight ? (frame.dx - rect.dx - xMargin) : xMargin;
        int idx = (int)corner;
        int y = atBottom ? (frame.dy - rect.dy - yMargin - yOffset[idx]) : (yMargin + yOffset[idx]);
        if (wnd->isSlidingIn) {
            // position is driven by the slide-in timer; only reserve the space
            // for stacking below
            yOffset[idx] += rect.dy + dyPadding;
            continue;
        }
        // SWP_NOCOPYBITS: repaint from scratch instead of copying stale bits, so
        // notifications that shift when another is dismissed draw correctly
        // (OnPaint is double-buffered, so no flicker)
        uint flags = SWP_NOSIZE | SWP_NOZORDER | SWP_NOCOPYBITS;
        SetWindowPos(wnd->hwnd, nullptr, x, y, 0, 0, flags);
        yOffset[idx] += rect.dy + dyPadding;
    }
}

void ShowNotificationsForActiveTab(HWND hwndCanvas, WindowTab* activeTab) {
    NotificationWnd* wnds[kMaxNotifs];
    int nWnds = GetForHwnd(hwndCanvas, wnds);
    for (int i = 0; i < nWnds; i++) {
        NotificationWnd* wnd = wnds[i];
        if (wnd->tab == nullptr) {
            continue; // not tied to a tab, always visible
        }
        if (wnd->delayTimerId != 0) {
            continue; // not yet shown; its delay timer controls visibility
        }
        bool show = (wnd->tab == activeTab);
        ShowWindow(wnd->hwnd, show ? SW_SHOW : SW_HIDE);
    }
    RelayoutNotifications(hwndCanvas);
}

static void NotifsRemoveNotification(NotificationWnd* wnd);

void RemoveNotificationsForTab(WindowTab* tab) {
    if (!tab) {
        return;
    }
    NotificationWnd* toRemove[kMaxNotifs];
    int nRemove = 0;
    for (int i = 0; i < gNotifsCount; i++) {
        if (gNotifs[i]->tab == tab) {
            toRemove[nRemove++] = gNotifs[i];
        }
    }
    for (int i = 0; i < nRemove; i++) {
        NotifsRemoveNotification(toRemove[i]);
    }
}

static void NotifsRemoveNotification(NotificationWnd* wnd) {
    int pos = NotificationIndexOf(wnd);
    if (pos < 0) {
        return;
    }
    gNotifsCount--;
    if (pos < gNotifsCount) {
        gNotifs[pos] = gNotifs[gNotifsCount];
    }
    gNotifs[gNotifsCount] = nullptr;
    RelayoutNotifications(HwndGetParent(wnd->hwnd));
    delete wnd;
}

int GetWndX(NotificationWnd* wnd) {
    Rect rect = WindowRect(wnd->hwnd);
    rect = MapRectToWindow(rect, HWND_DESKTOP, GetParent(wnd->hwnd));
    return rect.x;
}

NotificationWnd::~NotificationWnd() {
    if (delayTimerId != 0) {
        KillTimer(hwnd, delayTimerId);
        delayTimerId = 0;
    }
    if (slideTimerId != 0) {
        KillTimer(hwnd, slideTimerId);
        slideTimerId = 0;
    }
}

HWND NotificationWnd::Create(const NotificationCreateArgs& args) {
    highlight = args.warning;
    noClose = args.noClose;
    ReportIf(noClose && args.timeoutMs <= 0);
    shrinkLimit = args.shrinkLimit;
    if (shrinkLimit < 0.2f) {
        ReportIf(shrinkLimit < 0.2f);
        shrinkLimit = 1.f;
    }
    if (args.onRemoved.IsValid()) {
        wndRemovedCb = args.onRemoved;
    } else {
        wndRemovedCb = MkFunc1Void(NotifsRemoveNotification);
    }
    timeoutMs = args.timeoutMs;
    tab = args.tab;
    corner = args.corner;
    xMargin = args.xMargin;
    yMargin = args.yMargin;

    CreateCustomArgs cargs;
    cargs.parent = args.hwndParent;
    cargs.font = args.font;
    // TODO: was this important?
    // wcex.hCursor = LoadCursor(nullptr, IDC_APPSTARTING);
    cargs.exStyle = WS_EX_TOPMOST;
    cargs.style = WS_CHILD | SS_CENTER;
    cargs.title = args.msg;
    if (cargs.font == nullptr) {
        cargs.font = GetAppBiggerFont();
    }
    cargs.pos = Rect(0, 0, 0, 0);
    cargs.visible = args.delayInMs == 0;
    cargs.isRtl = IsUIRtl();

    CreateCustom(cargs);
    if (!hwnd) {
        return nullptr;
    }

    Layout(args.msg);
    delayInMs = args.delayInMs;
    if (delayInMs > 0) {
        // create hidden, will show after delay
        delayTimerId = SetTimer(hwnd, kNotifTimerDelayId, delayInMs, nullptr);
    } else {
        ShowWindow(hwnd, SW_SHOW);
        if (timeoutMs != 0) {
            SetTimer(hwnd, kNotifTimerTimeoutId, timeoutMs, nullptr);
        }
    }
    return hwnd;
}

// returns 0% - 100%
int CalcPerc(int current, int total) {
    ReportIf(total <= 0 || current < 0);
    ReportIf(total < current);
    if (total <= 0) {
        total = 1;
    }
    int perc = limitValue(100 * current / total, 0, 100);
    return perc;
}

constexpr int kCloseLeftMargin = 16;
constexpr int kProgressDy = 5;

static bool NotificationCloseHitTest(HWND hwnd, const Rect& rClose, Point pt) {
    UnmirrorRtl(hwnd, pt);
    return rClose.Contains(pt);
}

void NotificationWnd::Layout(Str message, bool keepWidth) {
    if (!message) {
        message = StrL("");
    }
    int padX = DpiScale(hwnd, 12);
    int padY = DpiScale(hwnd, 8);
    int closeDx = DpiScale(hwnd, 16);
    int closeLeftMargin = DpiScale(hwnd, kCloseLeftMargin) - padX;

    // limit the width to the parent window so that the close button
    // stays reachable even for very long messages (issue #2916)
    int topLeftMargin = DpiScale(hwnd, kTopLeftMargin);
    Rect rParent = ClientRect(GetParent(hwnd));
    int maxTextDx = rParent.dx - (2 * topLeftMargin) - (2 * padX);
    if (!noClose) {
        maxTextDx -= closeLeftMargin + closeDx + padX;
    }

    bool isRtl = IsUIRtl();

    // parse the message for the extended tip syntax (links, Key/ shortcuts)
    parsedMsg.Reset();
    ParseTip(parsedMsg, message);
    drawRich = TipHasLinks(parsedMsg);

    Size szText;
    if (drawRich) {
        // rich text: lay out words (links). Note: no RTL word reordering, same
        // as the home page tips.
        txtFmt = DT_LEFT | DT_NOPREFIX;
        HDC hdc = GetDC(hwnd);
        MeasureTipWords(parsedMsg, hdc, font);
        int areaWidth = (maxTextDx > 0) ? maxTextDx : (1 << 20);
        LayoutTip(parsedMsg, areaWidth, 0, 0);
        ReleaseDC(hwnd, hdc);
        szText.dx = parsedMsg.totalDx;
        szText.dy = parsedMsg.totalDy;
    } else {
        // plain text: render exactly like before (RTL handling, wrapping). Only
        // substitute the parsed text when there were (Key/...) shortcuts to
        // expand, so ordinary messages keep their original whitespace/newlines.
        if (str::Contains(message, StrL("(Key/"))) {
            message = TipPlainTextTemp(parsedMsg);
            HwndSetText(hwnd, message);
        }
        txtFmt = DT_SINGLELINE | DT_NOPREFIX;
        if (isRtl) {
            txtFmt |= DT_RIGHT | DT_RTLREADING;
        }
        HDC hdc = GetDC(hwnd);
        szText = HdcMeasureText(hdc, message, txtFmt, font);
        if (maxTextDx > 0 && szText.dx > maxTextDx) {
            // too wide: word-wrap the message; DT_WORD_ELLIPSIS truncates
            // words too long to wrap (e.g. long file paths)
            txtFmt = DT_WORDBREAK | DT_WORD_ELLIPSIS | DT_NOPREFIX;
            szText = HdcMeasureText(hdc, message, maxTextDx, txtFmt, font);
            if (szText.dx > maxTextDx) {
                szText.dx = maxTextDx;
            }
        }
        ReleaseDC(hwnd, hdc);
    }

    int dx = padX + szText.dx + padX;
    int dy = padY + szText.dy + padY;
    int closeBlockDx = closeLeftMargin + closeDx + padX;
    if (!noClose) {
        if (isRtl) {
            rClose = {padX, padY, closeDx, closeDx + 2};
            int textX = padX + closeBlockDx;
            rTxt = {textX, padY, szText.dx, szText.dy};
            dx = textX + szText.dx + padX;
        } else {
            rTxt = {padX, padY, szText.dx, szText.dy};
            rClose = {dx + closeLeftMargin, padY, closeDx, closeDx + 2};
            dx += closeBlockDx;
        }
    } else {
        rTxt = {padX, padY, szText.dx, szText.dy};
        rClose = {};
        dx += padX;
    }
    int progressDy = DpiScale(hwnd, kProgressDy);
    rProgress = {rTxt.x, dy, rTxt.dx, progressDy};
    if (HasProgress()) {
        dy += padY + progressDy + padY;
    }

    Rect rCurr = WindowRect(hwnd);
    // for less flicker we don't want to shrink the window when the text shrinks
    // (e.g. progress percentages changing). Only applies when the *content*
    // changed (keepWidth=true); when the parent window was resized
    // (keepWidth=false) we follow the new available width in both directions,
    // which is what makes the notification width elastic.
    if (keepWidth && dx < rCurr.dx) {
        int diff = rCurr.dx - dx;
        if (isRtl) {
            rTxt.dx += diff;
        } else {
            rClose.x += diff;
        }
        dx = rCurr.dx;
    }
    // same for the height: keep the taller size while content changes so
    // re-wrapping doesn't jump the window around; resize lets it collapse.
    if (keepWidth && dy < rCurr.dy) {
        dy = rCurr.dy;
    }
    // but never wider than the parent window (issue #2916)
    int maxDx = rParent.dx - (2 * topLeftMargin);
    if (maxDx > 0 && dx > maxDx) {
        int diff = dx - maxDx;
        if (isRtl) {
            rTxt.dx -= diff;
        } else {
            rClose.x -= diff;
        }
        dx = maxDx;
    }

    // y-center close
    if (!noClose) {
        rClose.y = ((dy - rClose.dx) / 2) + 1;
    }

    // `lastParentDx` is the resize-detection watermark for RelayoutNotifications:
    // it must only track the parent width at the last *resize-driven* layout.
    // Update-message layouts (keepWidth=true, e.g. page-info zoom changes) run
    // during a resize too and would otherwise fool the watermark into thinking
    // the window already matched the new canvas width (and skip the re-wrap).
    if (lastParentDx < 0 || !keepWidth) {
        lastParentDx = rParent.dx;
    }

    if (dx == rCurr.dx && dy == rCurr.dy) {
        return;
    }

    // adjust the window to fit the message (only shrink the window when there's no progress bar)
    uint flags = SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE;
    SetWindowPos(hwnd, nullptr, 0, 0, dx, dy, flags);

    // move the window to the right for a right-to-left layout
    if (IsUIRtl()) {
        HWND parent = GetParent(hwnd);
        Rect r = MapRectToWindow(WindowRect(hwnd), HWND_DESKTOP, parent);
        int cxVScroll = GetSystemMetrics(SM_CXVSCROLL);
        r.x = WindowRect(parent).dx - r.dx - DpiScale(hwnd, kTopLeftMargin) - cxVScroll;
        flags = SWP_NOSIZE | SWP_NOZORDER | SWP_NOREDRAW | SWP_NOACTIVATE | SWP_DEFERERASE;
        SetWindowPos(hwnd, nullptr, r.x, r.y, 0, 0, flags);
    }
}

// TODO: figure out why it flickers
void NotificationWnd::OnPaint(HDC hdcIn, PAINTSTRUCT* ps) {
    Rect rc = ClientRect(hwnd);
    DoubleBuffer buffer(hwnd, rc);
    HDC hdc = buffer.GetDC();
    // HDC hdc = hdcIn;

    ScopedSelectObject fontPrev(hdc, font);

    COLORREF colBg = ThemeNotificationsBackgroundColor();
    COLORREF colTxt = ThemeNotificationsTextColor();
    if (highlight) {
        colBg = ThemeNotificationsHighlightColor();
        colTxt = ThemeNotificationsHighlightTextColor();
    }
    // COLORREF colBg = MkRgb(0xff, 0xff, 0x5c);
    // COLORREF colBg = MkGray(0xff);

    // Phase 2: unified backend path. The rich-text (DrawTipWords) and GDI+ paths
    // below are kept for when gRenderer is unavailable / incompatible.
    bool viaRenderer = !drawRich && !!gRenderer;
    if (viaRenderer) {
        PAINTSTRUCT rps{};
        rps.hdc = hdc;
        rps.rcPaint = ToRECT(rc);
        viaRenderer = gRenderer->BeginPaint(hwnd, &rps);
    }
    if (viaRenderer) {
        gRenderer->FillRect(ToRECT(rc), RgbaColor(colBg));

        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, colTxt);
        TempStr text = HwndGetTextTemp(hwnd);
        RECT rTmp = ToRECT(rTxt);
        gRenderer->DrawText(text, rTmp, RgbaColor(colTxt), font, txtFmt);

        if (!noClose) {
            Point curPos = HwndGetCursorPos(hwnd);
            DrawCloseButtonArgs args;
            args.hdc = gRenderer->GetHDC();
            args.r = rClose;
            args.isHover = NotificationCloseHitTest(hwnd, rClose, curPos);
            DrawCloseButtonViaRenderer(args);
        }

        if (HasProgress()) {
            COLORREF col = ThemeNotificationsProgressColor();
            RECT rOuter = ToRECT(rProgress);
            gRenderer->DrawRect(rOuter, RgbaColor(col), 1.0f);
            RECT rInner = rOuter;
            rInner.left += 2;
            rInner.top += 2;
            rInner.bottom -= 1;
            rInner.right = rInner.left + (rOuter.right - rOuter.left - 3) * progressPerc / 100;
            gRenderer->FillRect(rInner, RgbaColor(col));
        }
        gRenderer->EndPaint();
        buffer.Flush(hdcIn);
        return;
    }

    Graphics graphics(hdc);
    SolidBrush br(GdiRgbFromCOLORREF(colBg));
    auto grc = Gdiplus::Rect(0, 0, rc.dx, rc.dy);
    graphics.FillRectangle(&br, grc);

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, colTxt);
    if (drawRich) {
        // words were laid out at (0,0); shift the origin to rTxt and draw
        POINT oldOrg;
        SetViewportOrgEx(hdc, rTxt.x, rTxt.y, &oldOrg);
        DrawTipWords(hdc, parsedMsg, font, colTxt, ThemeWindowLinkColor());
        SetViewportOrgEx(hdc, oldOrg.x, oldOrg.y, nullptr);
    } else {
        TempStr text = HwndGetTextTemp(hwnd);
        RECT rTmp = ToRECT(rTxt);
        HdcDrawText(hdc, text, &rTmp, txtFmt);
    }

    if (!noClose) {
        Point curPos = HwndGetCursorPos(hwnd);
        DrawCloseButtonArgs args;
        args.hdc = hdc;
        args.r = rClose;
        args.isHover = NotificationCloseHitTest(hwnd, rClose, curPos);
        DrawCloseButton(args);
    }
#if 0
    DrawCloseButtonArgs args;
    args.hdc = hdc;
    args.r = rClose;
    args.r.Inflate(-5, -5);
    args.isHover = isHover;
    DrawCloseButton2(args);
#endif

    if (HasProgress()) {
        rc = rProgress;
        int progressWidth = rc.dx;

        COLORREF col = ThemeNotificationsProgressColor();
        Pen pen(GdiRgbFromCOLORREF(col));
        grc = {rc.x, rc.y, rc.dx, rc.dy};
        graphics.DrawRectangle(&pen, grc);

        rc.x += 2;
        rc.dx = (progressWidth - 3) * progressPerc / 100;
        rc.y += 2;
        rc.dy -= 3;

        br.SetColor(GdiRgbFromCOLORREF(col));
        grc = {rc.x, rc.y, rc.dx, rc.dy};
        graphics.FillRectangle(&br, grc);
    }

    buffer.Flush(hdcIn);
}

void NotificationWnd::UpdateMessage(Str msg, int timeoutMs, bool highlight) {
    HwndSetText(hwnd, msg);
    this->highlight = highlight;
    this->timeoutMs = timeoutMs;
    HwndSetRtl(hwnd, IsUIRtl());
    Layout(msg);
    HwndRepaintNow(hwnd);
    if (timeoutMs != 0) {
        SetTimer(hwnd, kNotifTimerTimeoutId, timeoutMs, nullptr);
    }
}

bool UpdateNotificationProgress(NotificationWnd* wnd, Str msg, int perc) {
    if (NotificationIndexOf(wnd) < 0) {
        return false;
    }
    ReportIf(perc < 0 || perc > 100);
    wnd->progressPerc = perc;
    wnd->UpdateMessage(msg);
    return true;
}

static void NotifRemove(NotificationWnd* wnd) {
    wnd->wndRemovedCb.Call(wnd);
}

static void NotifDelete(NotificationWnd* wnd) {
    delete wnd;
}

void NotificationWnd::OnTimer(UINT_PTR timerId) {
    if (timerId == kNotifTimerSlideInId) {
        // Phase 3: slide-in animation — move from slideFrom to slideTo with ease-out
        slideT += 16.0f / 150.0f;
        if (slideT >= 1.0f) {
            KillTimer(hwnd, slideTimerId);
            slideTimerId = 0;
            isSlidingIn = false;
            uint flags = SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE;
            SetWindowPos(hwnd, nullptr, slideTo.x, slideTo.y, 0, 0, flags);
            // stack the rest of the notifications below the (final) position
            RelayoutNotifications(HwndGetParent(hwnd));
        } else {
            float e = slideT * (2.0f - slideT); // ease-out quadratic
            int x = slideFrom.x + (int)((slideTo.x - slideFrom.x) * e);
            int y = slideFrom.y + (int)((slideTo.y - slideFrom.y) * e);
            uint flags = SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE;
            SetWindowPos(hwnd, nullptr, x, y, 0, 0, flags);
        }
        return;
    }
    if (timerId == kNotifTimerDelayId) {
        // delay elapsed, now show the notification
        KillTimer(hwnd, delayTimerId);
        delayTimerId = 0;
        BringWindowToTop(hwnd);
        ShowWindow(hwnd, SW_SHOW);
        RelayoutNotifications(HwndGetParent(hwnd));
        StartSlideIn(this);
        if (timeoutMs != 0) {
            SetTimer(hwnd, kNotifTimerTimeoutId, timeoutMs, nullptr);
        }
        return;
    }
    ReportIf(kNotifTimerTimeoutId != timerId);
    // TODO a better way to delete myself
    if (wndRemovedCb.IsValid()) {
        auto fn = MkFunc0<NotificationWnd>(NotifRemove, this);
        uitask::Post(fn, "TaskNotifOnTimerRemove");
    } else {
        auto fn = MkFunc0<NotificationWnd>(NotifDelete, this);
        uitask::Post(fn, "TaskNotifOnTimerDelete");
    }
}

LRESULT NotificationWnd::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (WM_SETCURSOR == msg && !noClose) {
        Point pt = HwndGetCursorPos(hwnd);
        if (!pt.IsEmpty() && NotificationCloseHitTest(hwnd, rClose, pt)) {
            SetCursorCached(IDC_HAND);
            return TRUE;
        }
    }

    if (drawRich) {
        if (WM_SETCURSOR == msg) {
            Point pt = HwndGetCursorPos(hwnd);
            if (!pt.IsEmpty() && HitTestTipLink(parsedMsg, pt.x - rTxt.x, pt.y - rTxt.y) >= 0) {
                SetCursorCached(IDC_HAND);
                return TRUE;
            }
        }
        if (WM_LBUTTONUP == msg) {
            int x = GET_X_LPARAM(lp);
            int y = GET_Y_LPARAM(lp);
            int linkIdx = HitTestTipLink(parsedMsg, x - rTxt.x, y - rTxt.y);
            if (linkIdx >= 0) {
                // send commands to the top-level window (the main frame)
                HWND root = GetAncestor(hwnd, GA_ROOT);
                ExecuteTipLink(root, parsedMsg.links[linkIdx].cmd);
                return 0;
            }
        }
    }

    if (WM_ERASEBKGND == msg) {
        // avoid flicker by telling we took care of erasing background
        return TRUE;
    }

    if (WM_MOUSEMOVE == msg) {
        HwndScheduleRepaint(hwnd);

        if (!noClose) {
            Point pt = HwndGetCursorPos(hwnd);
            if (NotificationCloseHitTest(hwnd, rClose, pt)) {
                TrackMouseLeave(hwnd);
            }
        }
        goto DoDefault;
    }

    if (WM_MOUSELEAVE == msg) {
        HwndScheduleRepaint(hwnd);
        return 0;
    }

    if (WM_LBUTTONUP) {
        Point pt = Point(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        if (!noClose && NotificationCloseHitTest(hwnd, rClose, pt)) {
            // TODO a better way to delete myself
            if (wndRemovedCb.IsValid()) {
                auto fn = MkFunc0<NotificationWnd>(NotifRemove, this);
                uitask::Post(fn, "TaskNotifWndProcRemove");
            } else {
                auto fn = MkFunc0<NotificationWnd>(NotifDelete, this);
                uitask::Post(fn, "TaskNotifWndProcDelete");
            }
            return 0;
        }
    }

DoDefault:
    return WndProcDefault(hwnd, msg, wp, lp);
}

static int NotifsRemoveForGroup(NotificationWnd** wnds, int nWnds, Kind groupId) {
    ReportIf(groupId == nullptr);
    NotificationWnd* toRemove[kMaxNotifs];
    int nRemove = 0;
    for (int i = 0; i < nWnds; i++) {
        if (wnds[i]->groupId == groupId) {
            toRemove[nRemove++] = wnds[i];
        }
    }
    for (int i = 0; i < nRemove; i++) {
        NotifsRemoveNotification(toRemove[i]);
    }
    return nRemove;
}

static bool NotifsAdd(NotificationWnd** wnds, int nWnds, NotificationWnd* wnd, Kind groupId) {
    bool skipRemove = (groupId == nullptr) || (groupId == kNotifAdHoc);
    if (!skipRemove) {
        NotifsRemoveForGroup(wnds, nWnds, groupId);
    }
    if (gNotifsCount >= kMaxNotifs) {
        return false;
    }
    wnd->groupId = groupId;
    gNotifs[gNotifsCount] = wnd;
    gNotifsCount++;
    RelayoutNotifications(HwndGetParent(wnd->hwnd));
    return true;
}

static bool NotifsAdd(NotificationWnd* wnd, Kind groupId) {
    NotificationWnd* wnds[kMaxNotifs];
    int nWnds = GetForSameHwnd(wnd, wnds);
    return NotifsAdd(wnds, nWnds, wnd, groupId);
}

NotificationWnd* NotifsGetForGroup(NotificationWnd** wnds, int nWnds, Kind groupId) {
    ReportIf(!groupId);
    for (int i = 0; i < nWnds; i++) {
        if (wnds[i]->groupId == groupId) {
            return wnds[i];
        }
    }
    return nullptr;
}

NotificationWnd* ShowNotification(const NotificationCreateArgs& args) {
    ReportIf(!args.hwndParent);

    NotificationWnd* wnd = new NotificationWnd();
    wnd->Create(args);
    if (!wnd->hwnd) {
        delete wnd;
        return nullptr;
    }
    if (wnd->delayTimerId == 0) {
        BringWindowToTop(wnd->hwnd);
    }
    bool ok = NotifsAdd(wnd, args.groupId);
    if (!ok) {
        delete wnd;
        return nullptr;
    }
    if (wnd->delayTimerId == 0) {
        // Phase 3: slide-in from the corner once positioned by RelayoutNotifications
        StartSlideIn(wnd);
    }
    return wnd;
}

// show a temporary notification that will go away after a timeout
NotificationWnd* ShowTemporaryNotification(HWND hwnd, Str msg, int timeoutMs) {
    if (timeoutMs <= 0) {
        timeoutMs = kNotifDefaultTimeOut;
    }
    NotificationCreateArgs args;
    args.hwndParent = hwnd;
    args.msg = msg;
    args.timeoutMs = timeoutMs;
    return ShowNotification(args);
}

NotificationWnd* ShowWarningNotification(HWND hwndParent, Str msg, int timeoutMs) {
    if (timeoutMs < 0) {
        timeoutMs = kNotifDefaultTimeOut;
    }
    NotificationCreateArgs args;
    args.hwndParent = hwndParent;
    args.msg = msg;
    args.warning = true;
    args.timeoutMs = timeoutMs;
    return ShowNotification(args);
}

void NotificationUpdateMessage(NotificationWnd* wnd, Str msg, int timeoutMs, bool highlight) {
    if (!wnd) {
        return;
    }
    wnd->UpdateMessage(msg, timeoutMs, highlight);
}

TempStr NotificationGetMessageTemp(NotificationWnd* wnd) {
    if (!wnd) {
        return {};
    }
    return HwndGetTextTemp(wnd->hwnd);
}

void RemoveNotification(NotificationWnd* wnd) {
    NotifsRemoveNotification(wnd);
}

bool RemoveNotificationsForGroup(HWND hwnd, Kind kind) {
    NotificationWnd* wnds[kMaxNotifs];
    int nWnds = GetForHwnd(hwnd, wnds);
    int n = NotifsRemoveForGroup(wnds, nWnds, kind);
    return n > 0;
}

NotificationWnd* GetNotificationForGroup(HWND hwnd, Kind kind) {
    NotificationWnd* wnds[kMaxNotifs];
    int nWnds = GetForHwnd(hwnd, wnds);
    return NotifsGetForGroup(wnds, nWnds, kind);
}

static StrNode* AllocStrNode(Str s) {
    size_t n = (size_t)s.len + 1;
    size_t cbAlloc = sizeof(StrNode) + n;
    auto* node = (StrNode*)malloc(cbAlloc);
    u8* dst = (u8*)node + sizeof(StrNode);
    memcpy(dst, s.s, s.len);
    dst[s.len] = 0;
    node->next = nullptr;
    node->s = Str((char*)dst, (int)s.len);
    return node;
}

void MaybeDelayedWarningNotification(Str msg) {
    log(msg);

    HWND hwnd = GetHwndForNotification();
    if (hwnd) {
        ShowWarningNotification(hwnd, msg, kNotifNoTimeout);
    } else {
        StrNode* node = AllocStrNode(msg);
        ListInsertFront(&gDelayedNotifications, node);
    }
}

void ShowMaybeDelayedNotifications(HWND hwndParent) {
    // reverse so notifications show in the order they were added
    ListReverse(&gDelayedNotifications);
    StrNode* curr = gDelayedNotifications;
    while (curr) {
        ShowWarningNotification(hwndParent, curr->s, kNotifNoTimeout);
        curr = curr->next;
    }
    ListDelete(gDelayedNotifications);
    gDelayedNotifications = nullptr;
}
