/* Copyright 2024 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

#include "base/Base.h"
#include "base/Win.h"
#include "base/Dpi.h"
#include "wingui/UIModels.h"
#include "Settings.h"
#include "DocController.h"
#include "DisplayMode.h"
#include "EngineBase.h"
#include "DisplayModel.h"
#include "WindowTab.h"
#include "MainWindow.h"
#include "Theme.h"
#include "ThumbnailPanel.h"
#include "wingui/Renderer.h"

// Window class for the thumbnail panel
static WStr GetThumbClass() {
    return WStrL(L"SumatraPDF_ThumbnailPanel");
}

static LRESULT CALLBACK WndProcThumbnail(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    ThumbnailPanel* panel = (ThumbnailPanel*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (!panel && msg != WM_CREATE) {
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    switch (msg) {
        case WM_CREATE: {
            auto* cs = (CREATESTRUCTW*)lp;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
            return 0;
        }

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);

            // Phase 2: unified backend path. Direct2D + DirectWrite when the
            // backend is available, plain GDI otherwise. The GDI path below is
            // kept as the fallback (e.g. D2D binding fails mid-paint).
            if (gRenderer && gRenderer->BeginPaint(hwnd, &ps)) {
                gRenderer->FillRect(ps.rcPaint, RgbaColor(ThemeControlBackgroundColor()));
                if (!panel || panel->selectedPage < 0) {
                    gRenderer->DrawTextW(WStrL(L"No thumbnails"), rc, RgbaColor(ThemeWindowTextColor()),
                                         GetDefaultGuiFont(), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                } else {
                    int y = -panel->scrollPos;
                    for (int i = 0; i < 10 && y < rc.bottom; i++, y += panel->itemHeight) {
                        RECT itemRc = {0, y, rc.right, y + panel->itemHeight - 4};
                        if (i == panel->selectedPage) {
                            RECT selRc = itemRc;
                            InflateRect(&selRc, -2, -2);
                            gRenderer->FillRect(selRc, RgbaColor(ThemeWindowLinkColor()));
                        }
                        WCHAR buf[32];
                        int cch = swprintf_s(buf, L"Page %d", i + 1);
                        gRenderer->DrawTextW(WStr(buf, cch), itemRc, RgbaColor(ThemeWindowTextColor()),
                                             GetDefaultGuiFont(), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                    }
                }
                gRenderer->EndPaint();
                EndPaint(hwnd, &ps);
                return 0;
            }

            // Legacy GDI path (also the fallback when D2D is unavailable).
            HBRUSH bgBrush = CreateSolidBrush(ThemeControlBackgroundColor());
            FillRect(hdc, &ps.rcPaint, bgBrush);
            DeleteObject(bgBrush);

            // Draw placeholder text when no thumbs
            if (!panel || panel->selectedPage < 0) {
                SetBkMode(hdc, TRANSPARENT);
                SetTextColor(hdc, ThemeWindowTextColor());
                TempWStr text = ToWStrTemp(StrL("No thumbnails"));
                DrawTextW(hdc, text.s, text.len, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            } else {
                int y = -panel->scrollPos;
                for (int i = 0; i < 10 && y < rc.bottom; i++, y += panel->itemHeight) {
                    RECT itemRc = {0, y, rc.right, y + panel->itemHeight - 4};
                    if (i == panel->selectedPage) {
                        RECT selRc = itemRc;
                        InflateRect(&selRc, -2, -2);
                        HBRUSH selBrush = CreateSolidBrush(ThemeWindowLinkColor());
                        FillRect(hdc, &selRc, selBrush);
                        DeleteObject(selBrush);
                    }
                    WCHAR buf[32];
                    int cch = swprintf_s(buf, L"Page %d", i + 1);
                    SetBkMode(hdc, TRANSPARENT);
                    SetTextColor(hdc, ThemeWindowTextColor());
                    DrawTextW(hdc, buf, cch, &itemRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                }
            }
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_LBUTTONDOWN: {
            if (!panel) break;
            int y = GET_Y_LPARAM(lp) + panel->scrollPos;
            int page = y / panel->itemHeight;
            panel->selectedPage = page;
            InvalidateRect(hwnd, nullptr, TRUE);
            MainWindow* win = FindMainWindowByHwnd(panel->hwndOwner);
            if (win && win->ctrl) {
                win->ctrl->GoToPage(page + 1, true);
            }
            return 0;
        }

        case WM_MOUSEMOVE: {
            if (!panel) break;
            int y = GET_Y_LPARAM(lp) + panel->scrollPos;
            panel->hoveredPage = y / panel->itemHeight;
            return 0;
        }

        case WM_MOUSEWHEEL: {
            if (!panel) break;
            int delta = GET_WHEEL_DELTA_WPARAM(wp);
            panel->scrollPos -= delta / WHEEL_DELTA * 40;
            panel->scrollPos = std::max(0, std::min(panel->scrollPos, panel->totalHeight));
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        }
    }

    return DefWindowProcW(hwnd, msg, wp, lp);
}

void ThumbnailPanel::Create(HWND parent) {
    hwndOwner = parent;
    WStr clsName = GetThumbClass();

    if (!FindWindowW(clsName.s, nullptr)) {
        WNDCLASSEXW wcex = {};
        wcex.cbSize = sizeof(wcex);
        wcex.style = CS_HREDRAW | CS_VREDRAW;
        wcex.lpfnWndProc = WndProcThumbnail;
        wcex.hInstance = GetModuleHandleW(nullptr);
        wcex.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wcex.hbrBackground = nullptr;
        wcex.lpszClassName = clsName.s;
        RegisterClassExW(&wcex);
    }

    int panelWidth = DpiScale(parent, width);
    int panelHeight = DpiScale(parent, 400);
    hwnd = CreateWindowExW(WS_EX_NOACTIVATE, clsName.s, nullptr, WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, 0, 0,
                           panelWidth, panelHeight, parent, nullptr, GetModuleHandleW(nullptr), this);
    if (hwnd) {
        ShowWindow(hwnd, SW_HIDE);
    }
}

void ThumbnailPanel::Show(bool show, bool animated) {
    if (show == visible) return;
    visible = show;
    if (hwnd) {
        ShowWindow(hwnd, show ? SW_SHOW : SW_HIDE);
    }
}

void ThumbnailPanel::Toggle() {
    Show(!visible);
}

void ThumbnailPanel::ReloadThumbnails() {
    MainWindow* win = FindMainWindowByHwnd(hwndOwner);
    if (win && win->ctrl) {
        totalHeight = win->ctrl->PageCount() * itemHeight;
    }
    Invalidate();
}

void ThumbnailPanel::UpdateLayout(int parentY, int parentHeight) {
    if (!hwnd) return;
    int panelWidth = DpiScale(hwndOwner, width);
    SetWindowPos(hwnd, nullptr, 0, parentY, panelWidth, parentHeight, SWP_NOZORDER | SWP_NOACTIVATE);
}

void ThumbnailPanel::Invalidate() {
    if (hwnd) {
        InvalidateRect(hwnd, nullptr, TRUE);
    }
}