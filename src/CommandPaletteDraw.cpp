/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

#include "base/Base.h"
#include "base/Dpi.h"
#include "base/File.h"
#include "base/Win.h"

#include "wingui/UIModels.h"
#include "wingui/Layout.h"
#include "wingui/WinGui.h"

#include "Translations.h"
#include "Theme.h"
#include "Accelerators.h"
#include "FilterHighlightDraw.h"
#include "CommandPaletteScoring.h"
#include "CommandPaletteInternal.h"

// forward declaration: defined at the end of this file
static TempStr BuildPaletteAccelKey(int cmdId);

void PositionCommandPalette(HWND hwnd, HWND hwndRelative) {
    Rect rRelative = WindowRect(hwndRelative);
    Rect r = WindowRect(hwnd);
    int x = rRelative.x + (rRelative.dx / 2) - (r.dx / 2);
    int y = rRelative.y + 42;
    Rect rNew = {x, y, r.dx, r.dy};
    rNew = ShiftRectToWorkArea(rNew, hwndRelative, true);
    SetWindowPos(hwnd, nullptr, rNew.x, rNew.y, 0, 0, SWP_NOZORDER | SWP_NOSIZE);
}

static Str GetGroupLabel(int group) {
    switch (group) {
        case PaletteGroup_Commands:
            return _TRA("Commands");
        case PaletteGroup_Tabs:
            return _TRA("Tabs");
        case PaletteGroup_FileHistory:
            return _TRA("File History");
        case PaletteGroup_TOC:
            return _TRA("Table of Contents");
        case PaletteGroup_Favorites:
            return _TRA("Favorites");
        default:
            return StrL("");
    }
}

static bool IsPaletteGroupChange(ItemDataCP* cur, ItemDataCP* prev) {
    if (!cur || !prev) return false;
    int cg = (cur->indent >= kGroupTocOffset) ? PaletteGroup_TOC : cur->indent;
    int pg = (prev->indent >= kGroupTocOffset) ? PaletteGroup_TOC : prev->indent;
    return cg != pg;
}

// 12x12 monochrome category glyph drawn next to each group header.
static void DrawGroupIcon(HDC hdc, int group, int x, int y, COLORREF col) {
    int cx = x + 6, cy = y + 6;
    HPEN pen = CreatePen(PS_SOLID, 1, col);
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    HBRUSH br = CreateSolidBrush(col);
    HGDIOBJ oldBr = SelectObject(hdc, br);

    switch (group) {
        case PaletteGroup_Commands: { // ">_" prompt
            MoveToEx(hdc, x + 2, y + 3, nullptr);
            LineTo(hdc, cx, cy);
            LineTo(hdc, x + 2, y + 9);
            MoveToEx(hdc, cx + 1, y + 9, nullptr);
            LineTo(hdc, x + 11, y + 9);
            break;
        }
        case PaletteGroup_Tabs: { // two overlapping tab strips
            Rectangle(hdc, x + 1, y + 2, x + 9, y + 7);
            Rectangle(hdc, x + 4, y + 6, x + 12, y + 11);
            break;
        }
        case PaletteGroup_FileHistory: { // clock
            Ellipse(hdc, x + 1, y + 1, x + 12, y + 12);
            MoveToEx(hdc, cx, cy, nullptr);
            LineTo(hdc, cx, y + 3);
            MoveToEx(hdc, cx, cy, nullptr);
            LineTo(hdc, x + 9, y + 8);
            break;
        }
        case PaletteGroup_TOC: { // tree outline
            MoveToEx(hdc, x + 3, y + 2, nullptr);
            LineTo(hdc, x + 3, y + 10);
            MoveToEx(hdc, x + 3, y + 4, nullptr);
            LineTo(hdc, x + 8, y + 4);
            MoveToEx(hdc, x + 3, y + 7, nullptr);
            LineTo(hdc, x + 8, y + 7);
            Rectangle(hdc, x + 8, y + 2, x + 12, y + 5);
            Rectangle(hdc, x + 8, y + 5, x + 12, y + 8);
            break;
        }
        case PaletteGroup_Favorites: { // 5-point star
            POINT pts[10] = {
                {6, 1}, {7, 4}, {11, 4}, {8, 7}, {9, 10}, {6, 8}, {3, 10}, {4, 7}, {1, 4}, {5, 4},
            };
            for (auto& pt : pts) {
                pt.x += x;
                pt.y += y;
            }
            Polygon(hdc, pts, 10);
            break;
        }
        default:
            break;
    }

    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBr);
    DeleteObject(pen);
    DeleteObject(br);
}

static void DrawGroupHeaderRect(HDC hdc, RECT rc, int group, Str label, COLORREF colBg, COLORREF colTxt, HFONT font) {
    COLORREF headerBg = AccentColor(colBg, -10);
    SetBkColor(hdc, headerBg);
    ExtTextOutW(hdc, 0, 0, ETO_OPAQUE, &rc, nullptr, 0, nullptr);

    HPEN sepPen = CreatePen(PS_SOLID, 1, AccentColor(colTxt, 60));
    HGDIOBJ oldPen = SelectObject(hdc, sepPen);
    MoveToEx(hdc, rc.left, rc.top, nullptr);
    LineTo(hdc, rc.right, rc.top);
    SelectObject(hdc, oldPen);
    DeleteObject(sepPen);

    SetTextColor(hdc, AccentColor(colTxt, 60));
    SetBkMode(hdc, TRANSPARENT);
    if (font) SelectFont(hdc, font);
    rc.left += 8;
    DrawGroupIcon(hdc, group, rc.left, rc.top + (rc.bottom - rc.top) / 2 - 6, AccentColor(colTxt, 60));
    rc.left += 16;
    DrawTextW(hdc, ToWStrTemp(label).s, -1, &rc, DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_LEFT);
    if (font) SelectFont(hdc, font);
}

void CommandPaletteWnd::DrawListBoxItem(ListBox::DrawItemEvent* ev) {
    ListBox* lb = ev->listBox;
    auto m = (ListBoxModelCP*)lb->model;
    if (ev->itemIndex < 0 || ev->itemIndex >= m->ItemsCount()) return;

    HDC hdc = ev->hdc;
    RECT rc = ev->itemRect;

    COLORREF colBg = lb->bgColor;
    if (IsSpecialColor(colBg)) colBg = GetSysColor(COLOR_WINDOW);
    COLORREF colText = lb->textColor;
    if (IsSpecialColor(colText)) colText = GetSysColor(COLOR_WINDOWTEXT);

    ItemDataCP* data = m->Data(ev->itemIndex);

    // "(no matching items)" placeholder
    if (data && data->relevanceScore == -1) {
        SetBkColor(hdc, colBg);
        ExtTextOutW(hdc, 0, 0, ETO_OPAQUE, &rc, nullptr, 0, nullptr);
        SetTextColor(hdc, AccentColor(colText, 40));
        SetBkMode(hdc, TRANSPARENT);
        if (lb->font) SelectFont(hdc, lb->font);
        rc.left += DpiScale(lb->hwnd, 8);
        DrawTextW(hdc, ToWStrTemp(m->Item(ev->itemIndex)).s, -1, &rc,
                  DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_CENTER);
        if (lb->font) SelectFont(hdc, lb->font);
        return;
    }

    // Group header separator
    ItemDataCP* prevData = ev->itemIndex > 0 ? m->Data(ev->itemIndex - 1) : nullptr;
    if (prevData && IsPaletteGroupChange(data, prevData)) {
        int curGroup = PaletteGroup_Commands;
        if (data) curGroup = (data->indent >= kGroupTocOffset) ? PaletteGroup_TOC : data->indent;
        Str label = ::GetGroupLabel(curGroup);
        if (label.len > 0) DrawGroupHeaderRect(hdc, rc, curGroup, label.s, colBg, colText, lb->font);
    }

    if (ev->selected) colBg = AccentColor(colBg, 30);

    SetBkColor(hdc, colBg);
    ExtTextOutW(hdc, 0, 0, ETO_OPAQUE, &rc, nullptr, 0, nullptr);

    bool isRtl = HwndIsRtl(lb->hwnd);
    if (isRtl) SetLayout(hdc, 0);

    Str itemText = m->Item(ev->itemIndex);

    TempStr rightStr = nullptr;
    if (data && data->cmdId != 0) {
        // Shortcut strings are built once per (command, language) and reused
        // across redraws/scrolls instead of re-running AppendAccelKeyToMenuStringTemp
        // (string building + linear accelerator-table lookup) per row per paint.
        Str accel = accelKeyCache.Get(data->cmdId, trans::GetCurrentLangCode(), BuildPaletteAccelKey);
        if (accel && accel.s[0]) rightStr = accel;
    } else if (data && data->pageNo > 0) {
        rightStr = fmt("p%d", data->pageNo);
    } else if (data && data->filePath) {
        rightStr = path::GetDirTemp(data->filePath);
    }

    SetTextColor(hdc, colText);
    SetBkMode(hdc, TRANSPARENT);

    HFONT oldFont = lb->font ? SelectFont(hdc, lb->font) : nullptr;

    int padX = DpiScale(lb->hwnd, 4);
    rc.left += padX;
    rc.right -= padX;

    // TOC indent by tree depth
    if (data && data->indent >= kGroupTocOffset) {
        int depth = data->indent - kGroupTocOffset;
        int indentW = depth * DpiScale(lb->hwnd, 16);
        if (isRtl)
            rc.right -= indentW;
        else
            rc.left += indentW;
    }

    RECT rcText = rc;
    TempWStr rightStrW = nullptr;
    int rightW = 0;
    if (rightStr && rightStr.s[0]) {
        rightStrW = ToWStrTemp(rightStr);
        int gap = DpiScale(lb->hwnd, 8);
        SIZE szRight{};
        GetTextExtentPoint32W(hdc, rightStrW.s, len(rightStrW), &szRight);
        rightW = szRight.cx;
        if (isRtl)
            rcText.left += rightW + gap;
        else
            rcText.right -= rightW + gap;
    }

    {
        uint drawFmt = DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS;
        drawFmt |= isRtl ? (DT_RIGHT | DT_RTLREADING) : DT_LEFT;
        DrawMaybeHighlightedText(hdc, rcText, itemText, filterWords, highlighted, colBg, isRtl, false, drawFmt);
    }

    if (rightStrW) {
        RECT rcRight = rc;
        uint fmt = DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX;
        if (isRtl) {
            rcRight.right = rc.left + rightW;
            fmt |= DT_LEFT | DT_RTLREADING;
        } else {
            rcRight.left = rc.right - rightW;
            fmt |= DT_RIGHT;
        }
        SetTextColor(hdc, AccentColor(colText, 80));
        DrawTextW(hdc, rightStrW.s, -1, &rcRight, fmt);
        SetTextColor(hdc, colText);
    }

    if (oldFont) SelectFont(hdc, oldFont);
}

// Raw shortcut text for a command item, in the palette's "right-side label"
// form (the part after the "\t" of a menu string, e.g. "Ctrl+K"), as a temp
// string. The text is localized (German "Strg +" vs "Ctrl +"), so the language
// code is part of the cache key; a language switch therefore rebuilds instead
// of serving a stale string.
static TempStr BuildPaletteAccelKey(int cmdId) {
    TempStr s = AppendAccelKeyToMenuStringTemp("", cmdId);
    if (s && s.s[0] == '\t') {
        return Str(s.s + 1);
    }
    return StrL("");
}