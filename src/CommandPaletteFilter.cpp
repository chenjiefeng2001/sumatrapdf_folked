/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

#include "base/Base.h"

#include "gui/UIModels.h"
#include "gui/Layout.h"
#include "gui/win/WinGui.h"
#include "gui/PlatformFont.h"
#include "gui/Gfx.h"
#include "gui/VirtCtrl.h"

#include "FilterHighlightDraw.h"
#include "CommandPalette.h"
#include "CommandPaletteScoring.h"
#include "CommandPaletteInternal.h"

// ---------------------------------------------------------------------------
// Relevance-scored filtering: matching and scoring are decided per word in a
// single pass per item (ScorePaletteItem, see CommandPaletteScoring.h) and the
// matches are sorted by score so the best candidates are at the top.
// ---------------------------------------------------------------------------

struct ScoredIdx {
    int idx;
    int score;
};

static int ScoredIdxCmp(const void* a, const void* b) {
    const ScoredIdx* sa = (const ScoredIdx*)a;
    const ScoredIdx* sb = (const ScoredIdx*)b;
    if (sa->score != sb->score) return sb->score - sa->score;
    return sa->idx - sb->idx;
}

static void FilterAndSortStrings(StrVecCP& src, const StrVec& lowerWords, StrVecCP& dst) {
    int n = len(src);
    if (n == 0) {
        return;
    }
    Vec<ScoredIdx> scored;
    for (int i = 0; i < n; i++) {
        Str s = src[i];
        if (len(s) == 0) {
            continue;
        }
        bool matched = false;
        int score = ScorePaletteItem(s, lowerWords, &matched);
        if (!matched) {
            continue;
        }
        scored.Append({i, score});
    }
    if (len(scored) == 0) {
        return;
    }
    if (PaletteShouldSkipSort(len(scored))) {
        // Very broad query (e.g. every TOC entry matching "*c"): ranking barely
        // adds anything, so keep the source order and skip the scoring sort.
        for (int i = 0; i < len(scored); i++) {
            dst.AppendFrom(&src, scored[i].idx);
        }
        return;
    }
    if (len(scored) <= 200) {
        // insertion sort: stable and fastest for the small match counts of a
        // narrowed-down query
        for (int i = 1; i < len(scored); i++) {
            ScoredIdx key = scored[i];
            int j = i - 1;
            while (j >= 0 &&
                   (scored[j].score < key.score || (scored[j].score == key.score && scored[j].idx > key.idx))) {
                scored[j + 1] = scored[j];
                j--;
            }
            scored[j + 1] = key;
        }
    } else {
        qsort(scored.els, len(scored), sizeof(ScoredIdx), ScoredIdxCmp);
    }
    for (int i = 0; i < len(scored); i++) {
        dst.AppendFrom(&src, scored[i].idx);
    }
}

static void FilterStrings(StrVecCP& strs, const StrVec& lowerWords, StrVecCP& matchedOut) {
    FilterAndSortStrings(strs, lowerWords, matchedOut);
}

void CommandPaletteWnd::FilterStringsForQuery(Str filter, StrVecCP& strings) {
    strings.Reset();
    if (!filter) {
        filter = StrL("");
    }

    bool searchTabs = false, searchHistory = false, searchCommands = false, searchToc = false, searchFavorites = false;
    if (str::TrimPrefix(filter, kPalettePrefixEverything)) {
        searchTabs = searchHistory = searchCommands = true;
    } else if (str::TrimPrefix(filter, kPalettePrefixTabs)) {
        searchTabs = true;
    } else if (str::TrimPrefix(filter, kPalettePrefixFileHistory)) {
        searchHistory = true;
    } else if (str::TrimPrefix(filter, kPalettePrefixTOC) || str::TrimPrefix(filter, kPalettePrefixTOCLegacy)) {
        searchToc = true;
    } else if (str::TrimPrefix(filter, kPalettePrefixFavorites)) {
        searchFavorites = true;
    } else {
        str::TrimPrefix(filter, kPalettePrefixCommands);
        searchCommands = true;
    }

    filterWords.Reset();
    SplitFilterToWords(filter, filterWords);

    // Pre-lowercase the query words into temp arena storage (freed at the next
    // message loop) so ScorePaletteSingleWord doesn't fold the needle on every
    // character comparison. filterWords itself is left untouched: its Str slices
    // may alias the edit control's text buffer and it's also used verbatim by
    // the highlight drawing (which case-folds internally).
    StrVec lowerWords;
    for (int i = 0; i < len(filterWords); i++) {
        TempStr lower = str::DupTemp(filterWords[i]);
        str::ToLowerInPlace(lower);
        lowerWords.Append(lower);
    }

    if (searchTabs) {
        FilterStrings(tabs, lowerWords, strings);
    }
    if (searchHistory) {
        FilterStrings(fileHistory, lowerWords, strings);
    }
    if (searchCommands) {
        FilterStrings(commands, lowerWords, strings);
    }
    if (searchToc) {
        FilterStrings(toc, lowerWords, strings);
    }
    if (searchFavorites) {
        FilterStrings(favorites, lowerWords, strings);
    }
}

void CommandPaletteWnd::QueryChanged() {
    Str filter = CommandPaletteSkipWS(Str(editQuery->GetTextTemp()));
    int currSelIdx = 0;
    auto* m = (ListBoxModelCP*)listBox->model;
    int nItemsPrev = m->ItemsCount();
    if (smartTabMode) {
        if (!stickyMode) {
            if (len(filter) > 1) {
                stickyMode = true;
                currSelIdx = listBox->GetCurrentSelection();
            }
        }
    }
    FilterStringsForQuery(filter, m->strings);
    listBox->SetModel(m);
    int nItems = m->ItemsCount();
    if (nItems == 0) {
        return;
    }
    if (stickyMode && nItemsPrev == nItems) {
        CommandPaletteSetCurrentSelection(this, currSelIdx);
        return;
    }
    if ((str::StartsWith(filter, kPalettePrefixTOC) || str::StartsWith(filter, kPalettePrefixTOCLegacy)) &&
        len(filterWords) == 0) {
        int idx = (currTocIdx >= 0 && currTocIdx < nItems) ? currTocIdx : 0;
        CommandPaletteSetCurrentSelection(this, idx);
        return;
    }
    CommandPaletteSetCurrentSelection(this, 0);
}
