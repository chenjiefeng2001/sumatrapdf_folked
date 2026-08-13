/* Copyright 2024 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

#include "base/Base.h"
#include "base/Win.h"
#include "base/ScopedWin.h"

#include "wingui/UIModels.h"

#include "wingui/Layout.h"
#include "wingui/WinGui.h"

//- Static

// https://docs.microsoft.com/en-us/windows/win32/controls/static-controls

Kind kindStatic = "static";

Static::Static() {
    kind = kindStatic;
}

HWND Static::Create(const CreateArgs& args) {
    CreateControlArgs cargs;
    cargs.className = WC_STATICW;
    cargs.parent = args.parent;
    cargs.font = args.font;
    // default is a single line with clipping (SS_LEFTNOWORDWRAP). With wrap we
    // clear that bit so the control word-wraps to its window width; with
    // ellipsis we keep the single line but truncate with "…" when too narrow.
    cargs.style = WS_CHILD | WS_VISIBLE | SS_NOTIFY | SS_LEFTNOWORDWRAP;
    if (args.wrap) {
        cargs.style &= ~SS_LEFTNOWORDWRAP;
    }
    if (args.ellipsis) {
        cargs.style |= SS_ENDELLIPSIS;
    }
    cargs.text = args.text;
    cargs.isRtl = args.isRtl;

    wrap = args.wrap;
    ellipsis = args.ellipsis;

    Wnd::CreateControl(cargs);
    SizeToIdealSize(this);

    return hwnd;
}

Size Static::GetIdealSize() {
    ReportIf(!hwnd);
    TempStr txt = HwndGetTextTemp(hwnd);
    HFONT hfont = GetWindowFont(hwnd);
    return HwndMeasureText(hwnd, txt, hfont);
}

// The "min-content" width of a wrapped text: the width of the longest
// unbreakable word. Measured by word-wrapping at the narrowest possible width
// (DrawText wraps and the returned width is the widest word).
int Static::MinIntrinsicWidth(int height) {
    ReportIf(!hwnd);
    if (!wrap) {
        return Wnd::MinIntrinsicWidth(height);
    }
    TempStr txt = HwndGetTextTemp(hwnd);
    HFONT hfont = GetWindowFont(hwnd);
    AutoReleaseDC dc(hwnd);
    ScopedSelectFont prev(dc, hfont);
    Size s = HdcMeasureText(dc, txt, 1, DT_WORDBREAK | DT_NOPREFIX, hfont);
    if (s.dx <= 0) {
        return Wnd::MinIntrinsicWidth(height);
    }
    return s.dx;
}

// The height the text needs when word-wrapped to `width` pixels. `width` is the
// constraint the layout engine hands down (Wnd::Layout passes it to
// MinIntrinsicHeight), so this is the elastic feedback loop: a narrower width
// produces more lines and thus a taller control.
int Static::MinIntrinsicHeight(int width) {
    ReportIf(!hwnd);
    if (!wrap || width <= 0 || width == Inf) {
        return Wnd::MinIntrinsicHeight(width);
    }
    TempStr txt = HwndGetTextTemp(hwnd);
    HFONT hfont = GetWindowFont(hwnd);
    AutoReleaseDC dc(hwnd);
    ScopedSelectFont prev(dc, hfont);
    Size s = HdcMeasureWrappedText(dc, txt, width, hfont);
    if (s.dy <= 0) {
        return Wnd::MinIntrinsicHeight(width);
    }
    return s.dy;
}

Size Static::Layout(Constraints bc) {
    if (!wrap) {
        return Wnd::Layout(bc);
    }
    auto hinset = insets.left + insets.right;
    auto vinset = insets.top + insets.bottom;
    auto innerConstraints = bc.Inset(hinset, vinset);
    if (!innerConstraints.HasBoundedWidth()) {
        // unconstrained width (e.g. a Static inside an HBox): fall back to the
        // base single-line behavior, like CSS min-content in a horizontal flow
        return Wnd::Layout(bc);
    }
    // the text fills the given width and wraps; the height follows from the wrap
    int width = std::max(innerConstraints.max.dx, 0);
    childSize = innerConstraints.Constrain(Size{width, MinIntrinsicHeight(width)});
    return Size{childSize.dx + hinset, childSize.dy + vinset};
}

void Static::SetText(Str s) {
    Wnd::SetText(s);
    if (onTextChanged.IsValid()) {
        onTextChanged.Call();
    }
}

bool Static::OnCommand(WPARAM wparam, LPARAM lparam) {
    auto code = HIWORD(wparam);
    if (code == STN_CLICKED && onClick.IsValid()) {
        onClick.Call();
        return true;
    }
    return false;
}

LRESULT Static::OnMessageReflect(UINT msg, WPARAM wp, LPARAM lparam) {
    if (msg == WM_CTLCOLORSTATIC) {
        HDC hdc = (HDC)wp;
        if (!IsSpecialColor(textColor)) {
            SetTextColor(hdc, textColor);
        }
        if (!IsSpecialColor(bgColor)) {
            SetBkColor(hdc, bgColor);
        }
        auto br = BackgroundBrush();
        return (LRESULT)br;
    }
    return 0;
}
