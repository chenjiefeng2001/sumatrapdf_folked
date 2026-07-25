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
#include "CommandPaletteInternal.h"

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

static void DrawGroupHeaderRect(HDC hdc, RECT rc, Str label, COLORREF colBg, COLORREF colTxt, HFONT font) {
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
        if (label.len > 0) DrawGroupHeaderRect(hdc, rc, label.s, colBg, colText, lb->font);
    }

    if (ev->selected) colBg = AccentColor(colBg, 30);

    SetBkColor(hdc, colBg);
    ExtTextOutW(hdc, 0, 0, ETO_OPAQUE, &rc, nullptr, 0, nullptr);

    bool isRtl = HwndIsRtl(lb->hwnd);
    if (isRtl) SetLayout(hdc, 0);

    Str itemText = m->Item(ev->itemIndex);

    TempStr rightStr = nullptr;
    if (data && data->cmdId != 0) {
        TempStr withAccel = AppendAccelKeyToMenuStringTemp("", data->cmdId);
        if (withAccel && withAccel.s[0] == '\t') rightStr = Str(withAccel.s + 1);
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