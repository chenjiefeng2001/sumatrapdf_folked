/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// Unit tests for the annotation hit-test spatial index (AnnotHitTest.h).
//
// These are pure-math tests running inside test_util.exe (no GUI, no window
// messages): they prove the index replicates the old linear-scan semantics
// (contains → preferred wins → smallest area, ties in list order) while only
// examining a small fraction of the entries.

#include "base/Base.h"
#include "base/AnnotHitTest.h"

// must be last due to assert() over-write
#include "base/UtAssert.h"

// 5x5 grid, 12 non-overlapping annotations → a hit test must only examine the
// entries in the queried cell, never the whole list.
static void BasicHitsAndCandidatesTest() {
    RectF page(0, 0, 500, 500);
    Vec<AnnotHitEntry> entries;
    for (int i = 0; i < 12; i++) {
        RectF r((float)(i % 5) * 100.f, (float)(i / 5) * 100.f, 80, 80);
        entries.Append(AnnotHitEntry{r, (void*)(intptr_t)(i + 1)});
    }

    AnnotHitIndex idx;
    idx.Build(&entries, page);
    utassert(!idx.empty());

    // a point inside one annotation finds exactly it
    for (int i = 0; i < 12; i++) {
        RectF r = entries[i].bounds;
        PointF pt(r.x + 40, r.y + 40);
        void* hit = idx.HitTest(pt, nullptr);
        utassert(hit == entries[i].user);
        // the 4x4 grid has 125pt cells; an 80pt annotation at 100pt pitch can
        // straddle up to 4 cells (2x2), so a cell holds at most a handful of
        // entries — never the whole 12-entry list
        utassert(idx.lastCandidatesExamined() <= 6);
    }

    // a point in empty space hits nothing (the index clamps the query into the
    // edge cell, but the actual bounds-containment check still rejects it)
    utassert(idx.HitTest(PointF(250, 250), nullptr) == nullptr);
    utassert(idx.HitTest(PointF(-10, -10), nullptr) == nullptr);
    utassert(idx.HitTest(PointF(600, 600), nullptr) == nullptr);
}

// preferred wins even when a smaller annotation would otherwise win
static void PreferredWinsTest() {
    RectF page(0, 0, 100, 100);
    Vec<AnnotHitEntry> entries;
    // big rect first, small rect overlapping it later
    entries.Append(AnnotHitEntry{RectF(0, 0, 100, 100), (void*)1});
    entries.Append(AnnotHitEntry{RectF(10, 10, 20, 20), (void*)2});

    AnnotHitIndex idx;
    idx.Build(&entries, page);

    // both contain (15, 15): the small one wins without a preferred
    utassert(idx.HitTest(PointF(15, 15), nullptr) == (void*)2);
    // but the drag target wins unconditionally
    utassert(idx.HitTest(PointF(15, 15), (void*)1) == (void*)1);
    utassert(idx.HitTest(PointF(15, 15), (void*)2) == (void*)2);
    // a preferred that doesn't contain the point changes nothing
    utassert(idx.HitTest(PointF(5, 5), (void*)2) == (void*)1);
}

// smallest area wins, ties resolve to the first entry in list order (the same
// '<' comparison the linear scan used)
static void SmallestAreaAndTieTest() {
    RectF page(0, 0, 100, 100);
    Vec<AnnotHitEntry> entries;
    entries.Append(AnnotHitEntry{RectF(0, 0, 100, 100), (void*)1}); // area 10000
    entries.Append(AnnotHitEntry{RectF(10, 10, 40, 40), (void*)2}); // area 1600
    entries.Append(AnnotHitEntry{RectF(20, 20, 60, 60), (void*)3}); // area 3600
    entries.Append(AnnotHitEntry{RectF(0, 0, 30, 30), (void*)4});   // area 900 → wins

    AnnotHitIndex idx;
    idx.Build(&entries, page);
    utassert(idx.HitTest(PointF(25, 25), nullptr) == (void*)4);

    // equal areas: the first-listed wins
    Vec<AnnotHitEntry> ties;
    ties.Append(AnnotHitEntry{RectF(0, 0, 10, 10), (void*)1});
    ties.Append(AnnotHitEntry{RectF(20, 20, 10, 10), (void*)2});
    AnnotHitIndex idxTies;
    idxTies.Build(&ties, page);
    utassert(idxTies.HitTest(PointF(25, 25), nullptr) == (void*)2); // only one contains
    utassert(idxTies.HitTest(PointF(5, 5), nullptr) == (void*)1);
}

// annotation partially outside the mediabox must still be hittable, and a
// degenerate (zero-area) mediabox must not crash
static void EdgeCasesTest() {
    RectF page(0, 0, 100, 100);
    Vec<AnnotHitEntry> entries;
    entries.Append(AnnotHitEntry{RectF(-50, 20, 100, 60), (void*)1}); // half outside
    entries.Append(AnnotHitEntry{RectF(50, -20, 60, 100), (void*)2}); // half above
    entries.Append(AnnotHitEntry{RectF(200, 200, 0, 0), (void*)3});   // zero area

    AnnotHitIndex idx;
    idx.Build(&entries, page);
    // the out-of-page part is clamped into the edge cells, still hittable
    utassert(idx.HitTest(PointF(-20, 50), nullptr) == (void*)1);
    utassert(idx.HitTest(PointF(80, -5), nullptr) == (void*)2);
    // zero-area annotation never matches anything
    utassert(idx.HitTest(PointF(200, 200), nullptr) == nullptr);

    // empty / degenerate inputs are safe
    AnnotHitIndex empty;
    utassert(empty.empty());
    utassert(empty.HitTest(PointF(0, 0), nullptr) == nullptr);

    Vec<AnnotHitEntry> none;
    AnnotHitIndex noneIdx;
    noneIdx.Build(&none, page);
    utassert(noneIdx.empty());
    utassert(noneIdx.HitTest(PointF(0, 0), nullptr) == nullptr);

    Vec<AnnotHitEntry> some;
    some.Append(AnnotHitEntry{RectF(0, 0, 10, 10), (void*)1});
    AnnotHitIndex degenerate;
    degenerate.Build(&some, RectF(0, 0, 0, 0));
    utassert(degenerate.empty());
    utassert(degenerate.HitTest(PointF(5, 5), nullptr) == nullptr);
}

// a page with many annotations: the index examines far fewer candidates than a
// full linear scan (this is the WM_MOUSEMOVE hot-path regression guard)
static void CandidatePruningTest() {
    RectF page(0, 0, 1000, 1000);
    Vec<AnnotHitEntry> entries;
    for (int i = 0; i < 1000; i++) {
        int c = i % 100, r = i / 100;
        entries.Append(AnnotHitEntry{RectF((float)(c * 10), (float)(r * 10), 8, 8), (void*)(intptr_t)(i + 1)});
    }

    AnnotHitIndex idx;
    idx.Build(&entries, page);
    for (int i = 0; i < 1000; i += 37) {
        RectF b = entries[i].bounds;
        PointF pt(b.x + 4, b.y + 4);
        utassert(idx.HitTest(pt, nullptr) == entries[i].user);
        // cell side is ceil(sqrt(1000)) = 32, page 1000/32 ≈ 31pt per cell;
        // entries sit on a 10pt pitch, so a cell can hold up to 4x4 = 16
        // entries (never anywhere near the 1000-entry list)
        utassert(idx.lastCandidatesExamined() >= 1);
        utassert(idx.lastCandidatesExamined() <= 32);
    }
}

void AnnotHitTestTest() {
    BasicHitsAndCandidatesTest();
    PreferredWinsTest();
    SmallestAreaAndTieTest();
    EdgeCasesTest();
    CandidatePruningTest();
}

