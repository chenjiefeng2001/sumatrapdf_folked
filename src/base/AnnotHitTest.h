/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// Uniform-grid spatial index for annotation / widget hit-testing. EngineMupdf
// uses it so a high annotation density + WM_MOUSEMOVE doesn't turn every mouse
// move into a linear scan of the whole page's annotation list.
//
// Sits in base (payload-agnostic: entries carry an opaque void*) so it can be
// unit-tested headlessly by test_util.
//
// Semantics mirror the previous linear scan exactly (see
// EngineMupdfGetAnnotationAtPos in EngineMupdf.cpp):
//   - an entry is a hit iff its bounds contain the point (RectF::Contains,
//     half-open [x, x+dx) ranges, same as before)
//   - if the "preferred" payload is among the hits it wins unconditionally
//     (used for the annotation under the mouse during a drag)
//   - otherwise the hit with the smallest bounds area wins; ties go to the
//     entry that comes first in the input list (strict '<' keeps that order)
//
// The index is rebuilt wholesale (Build) whenever the entry list or any bounds
// change; it never supports incremental inserts, which keeps the code trivial.

#ifndef ANNOT_HIT_TEST_H
#define ANNOT_HIT_TEST_H

struct PointF;
struct RectF;

template <typename T>
class Vec;

struct AnnotHitEntry {
    RectF bounds;
    void* user = nullptr;
};

class AnnotHitIndex {
  public:
    // Build a grid over `entries` (which must stay alive and unmodified until
    // the next Build — the engine keeps a mirror Vec in FzPageInfo for exactly
    // that). `mediabox` gives the page extent used for cell sizing; entries
    // outside it are clamped into the edge cells so they stay hittable.
    void Build(const Vec<AnnotHitEntry>* entries, RectF mediabox);

    // Winning entry for pos per the semantics above, or nullptr.
    void* HitTest(PointF pos, void* preferred) const;

    // number of entries whose bounds were tested by the last HitTest call
    // (exposed for the unit test to prove the spatial pruning works)
    int lastCandidatesExamined() const { return lastCandidatesExamined_; }

    bool empty() const { return !entries_ || entries_->empty() || cols_ == 0 || rows_ == 0; }

  private:
    struct Cell {
        Vec<const AnnotHitEntry*> els; // entries intersecting this cell
    };

    const Vec<AnnotHitEntry>* entries_ = nullptr;
    RectF extent_ = {};
    float cellDx_ = 1.f;
    float cellDy_ = 1.f;
    int cols_ = 0;
    int rows_ = 0;
    Vec<Cell> cells_;
    mutable int lastCandidatesExamined_ = 0;
};

#endif // ANNOT_HIT_TEST_H
