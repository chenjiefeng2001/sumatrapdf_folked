/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// Unit tests for the elastic layout support:
//   1. HdcMeasureWrappedText: word-wrapping text measurement (narrower width ->
//      more lines -> taller result), the "width constraint -> height feedback"
//      loop behind elastic text layout.
//   2. The layout engine propagates the width constraint down the tree so a
//      wrap-aware leaf (e.g. Static with wrap=true, simulated by FakeWrapBox)
//      reports a taller minimum height in a narrow container than in a wide one.
//
// Runs inside test_util.exe without any GUI: text measurement uses an in-memory
// DC with the stock GUI font, the layout test uses a pure-logic box.

#include "base/Base.h"
#include "base/Win.h"

#include "gui/Layout.h"

// must be last due to assert() over-write
#include "base/UtAssert.h"

// A layout leaf that simulates a word-wrapping text control: below `wrapWidth`
// the text needs two lines, otherwise one. This exercises the exact contract
// Static relies on (MinIntrinsicHeight(width) answers with the wrapped height).
struct FakeWrapBox : ILayout {
    int wrapWidth = 100;
    int lineH = 10;
    int width = 120;

    void SetVisibility(Visibility) override {}
    Visibility GetVisibility() override { return Visibility::Visible; }

    int MinIntrinsicHeight(int width) override {
        if (width > 0 && width < wrapWidth) {
            return 2 * lineH;
        }
        return lineH;
    }
    int MinIntrinsicWidth(int) override { return width; }
    Size Layout(Constraints bc) override {
        return bc.Constrain(Size{MinIntrinsicWidth(0), MinIntrinsicHeight(bc.max.dx)});
    }
    void SetBounds(Rect) override {}
};

// word-wrapping text measurement: narrower width -> more lines -> taller result
static Size MeasureWrapped(HDC hdc, Str txt, int maxDx, HFONT font) {
    if (len(txt) == 0) {
        return {};
    }
    return HdcMeasureText(hdc, txt, maxDx, DT_WORDBREAK | DT_NOPREFIX | DT_WORD_ELLIPSIS, font);
}

static void WrappedTextMeasureTest() {
    HDC hdc = CreateCompatibleDC(nullptr);
    HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

    Str txt = StrL("The quick brown fox jumps over the lazy dog while the sun is shining");
    Size wide = MeasureWrapped(hdc, txt, 600, font);
    Size narrow = MeasureWrapped(hdc, txt, 80, font);
    utassert(wide.dx > 0 && wide.dy > 0);
    utassert(narrow.dx > 0 && narrow.dy > 0);
    // wrapped text is taller than single-line text
    utassert(narrow.dy > wide.dy);
    // the narrow measure never exceeds the requested width
    utassert(narrow.dx <= 80 + 1);
    // heights are monotonic: a wider box never needs more lines than a narrower one
    int prevH = Inf;
    for (int w = 30; w <= 600; w += 30) {
        int h = MeasureWrapped(hdc, txt, w, font).dy;
        utassert(h <= prevH);
        prevH = h;
    }

    // a single long unbreakable word is truncated to fit (DT_WORD_ELLIPSIS)
    Size word = MeasureWrapped(hdc, StrL("supercalifragilisticexpialidocious"), 40, font);
    utassert(word.dx > 0 && word.dx <= 40);
    // min-content width: wrapping at the narrowest width reports the widest
    // unbreakable word (what Static::MinIntrinsicWidth uses)
    Size wordMin = HdcMeasureText(hdc, StrL("supercalifragilisticexpialidocious"), 1, DT_WORDBREAK | DT_NOPREFIX, font);
    utassert(wordMin.dx > 40);

    // empty text measures as empty
    Size empty = MeasureWrapped(hdc, StrL(""), 80, font);
    utassert(empty.dx == 0 && empty.dy == 0);

    DeleteDC(hdc);
}

static void LayoutEngineConstraintPropagationTest() {
    // A VBox (alignCross = Stretch, as used by the command palette and dialogs)
    // with a wrap-aware child: the width constraint must reach the child's
    // MinIntrinsicHeight, so a narrow container reports a taller height.
    VBox vbox;
    vbox.alignCross = CrossAxisAlign::Stretch;
    vbox.AddChild(new FakeWrapBox());
    utassert(vbox.MinIntrinsicHeight(200) == 10);
    utassert(vbox.MinIntrinsicHeight(60) == 20);
    utassert(vbox.MinIntrinsicWidth(0) == 120);

    // Layout in a narrow box returns the wrapped (taller) height. Loose() keeps
    // an upper bound so the result isn't clamped to a tight height.
    Size s = vbox.Layout(Loose(Size{60, 1000}));
    utassert(s.dy == 20);
    // Layout in a wide box returns the single-line height.
    s = vbox.Layout(Loose(Size{200, 1000}));
    utassert(s.dy == 10);

    // The same through a Padding wrapper (dialogs wrap Statics in Padding).
    Padding pad(new FakeWrapBox(), Insets{4, 4, 4, 4});
    utassert(pad.MinIntrinsicHeight(200) == 10 + 8);
    utassert(pad.MinIntrinsicHeight(60) == 20 + 8);
    s = pad.Layout(Loose(Size{60, 1000}));
    utassert(s.dy == 20 + 8 && s.dx == 60);

    // Unbounded width keeps the single-line behavior (e.g. a Static inside an
    // HBox, which does not hand a width down).
    utassert(vbox.MinIntrinsicHeight(Inf) == 10);
    s = vbox.Layout(ExpandInf());
    utassert(s.dy == 10);
}

void ElasticLayoutTest() {
    WrappedTextMeasureTest();
    LayoutEngineConstraintPropagationTest();
}
