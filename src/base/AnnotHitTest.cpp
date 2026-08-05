/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "base/Base.h"

// own header last (project convention: headers are not self-sufficient)
#include "AnnotHitTest.h"

// Target grid density: ~sqrt(n) cells per axis for n entries. For a typical
// annotated page (a handful to a few dozen annotations) this means most cells
// hold 0-2 entries, so a hit test examines a small constant number of bounds.
// Clamped so pathological pages (thousands of annotations) don't allocate
// millions of empty cells.
static void GetGridSize(int n, int* colsOut, int* rowsOut) {
    int side = 1;
    while (side * side < n && side < 64) {
        side++;
    }
    *colsOut = side;
    *rowsOut = side;
}

void AnnotHitIndex::Build(const Vec<AnnotHitEntry>* entries, RectF mediabox) {
    entries_ = entries;
    cells_ = Vec<Cell>();
    cols_ = 0;
    rows_ = 0;
    extent_ = mediabox;
    if (!entries || entries->empty() || mediabox.dx <= 0 || mediabox.dy <= 0) {
        return;
    }

    int n = len(*entries);
    GetGridSize(n, &cols_, &rows_);
    cellDx_ = mediabox.dx / cols_;
    cellDy_ = mediabox.dy / rows_;
    cells_.AppendBlanks(cols_ * rows_);

    for (const AnnotHitEntry& e : *entries) {
        RectF b = e.bounds;
        if (b.dx <= 0 || b.dy <= 0) {
            // a zero-area bounds can never contain a point; skip (matches the
            // old linear scan which RectF::Contains rejects)
            continue;
        }
        // cell range covered by the bounds, clamped to the grid edges so
        // out-of-mediabox annotations stay hittable
        int c0 = (int)((b.x - extent_.x) / cellDx_);
        int c1 = (int)((b.x + b.dx - extent_.x) / cellDx_);
        int r0 = (int)((b.y - extent_.y) / cellDy_);
        int r1 = (int)((b.y + b.dy - extent_.y) / cellDy_);
        c0 = std::clamp(c0, 0, cols_ - 1);
        c1 = std::clamp(c1, 0, cols_ - 1);
        r0 = std::clamp(r0, 0, rows_ - 1);
        r1 = std::clamp(r1, 0, rows_ - 1);
        // cover the whole range: an annotation can straddle many cells, but
        // annotations are typically small relative to the page so this stays
        // a few cells. Degenerate input (a full-page highlight) degenerates to
        // the old linear scan for that page, which is the right trade-off.
        for (int r = r0; r <= r1; r++) {
            for (int c = c0; c <= c1; c++) {
                cells_[r * cols_ + c].els.Append(&e);
            }
        }
    }
}

void* AnnotHitIndex::HitTest(PointF pos, void* preferred) const {
    lastCandidatesExamined_ = 0;
    if (empty() || cols_ == 0 || rows_ == 0) {
        return nullptr;
    }
    int c = (int)((pos.x - extent_.x) / cellDx_);
    int r = (int)((pos.y - extent_.y) / cellDy_);
    c = std::clamp(c, 0, cols_ - 1);
    r = std::clamp(r, 0, rows_ - 1);

    void* best = nullptr;
    float bestArea = 0.f;
    bool haveBest = false;
    for (const AnnotHitEntry* e : cells_[r * cols_ + c].els) {
        lastCandidatesExamined_++;
        if (!e->bounds.Contains(pos)) {
            continue;
        }
        if (e->user == preferred) {
            // drag/resize target wins unconditionally, same as the linear scan
            return preferred;
        }
        float area = e->bounds.dx * e->bounds.dy;
        if (!haveBest || area < bestArea) {
            best = e->user;
            bestArea = area;
            haveBest = true;
        }
    }
    return best;
}
