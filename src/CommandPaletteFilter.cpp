/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

#include "base/Base.h"

#include "wingui/UIModels.h"
#include "wingui/Layout.h"
#include "wingui/WinGui.h"

#include "base/Win.h"
#include "FilterHighlightDraw.h"
#include "CommandPalette.h"
#include "CommandPaletteScoring.h"
#include "CommandPaletteInternal.h"

// ---------------------------------------------------------------------------
// Relevance scoring for search results
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

static void FilterAndSortStrings(StrVecCP& src, const StrVec& words, StrVecCP& dst) {
    int n = len(src);
    if (n == 0) return;
    Vec<ScoredIdx> scored;
    scored.capacityHint = n;
    for (int i = 0; i < n; i++) {
        // ScorePaletteItem merges the old FilterMatches + ComputeRelevanceScore
        // into one scan: matching and scoring are decided per word in a single
        // pass, with an early exit for non-matching items (see CommandPaletteScoring.h).
        bool matched = false;
        int score = ScorePaletteItem(src.At(i), words, &matched);
        if (!matched) continue;
        scored.Append({i, score});
    }
    if (len(scored) == 0) return;
    if (PaletteShouldSkipSort(len(scored))) {
        // Very broad query (e.g. every TOC entry matching "*c"): ranking barely
        // adds anything, so keep the source order and skip the scoring sort.
        for (int i = 0; i < len(scored); i++) {
            dst.AppendFrom(&src, scored[i].idx);
        }
        return;
    }
    if (len(scored) <= 200) {
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

static void FilterStrings(StrVecCP& strs, const StrVec& words, StrVecCP& matchedOut, int groupTag) {
    int startLen = len(matchedOut);
    FilterAndSortStrings(strs, words, matchedOut);
    int nAdded = len(matchedOut) - startLen;
    // StrVec is paged, so the items are NOT contiguous in memory: access each
    // item individually instead of treating data as an array.
    for (int i = 0; i < nAdded; i++) {
        ItemDataCP* d = matchedOut.AtData(startLen + i);
        ReportIf(!d);
        if (groupTag == PaletteGroup_TOC) {
            int realDepth = d->indent;
            d->indent = kGroupTocOffset + realDepth;
        } else {
            d->indent = groupTag;
        }
    }
}

void CommandPaletteWnd::FilterStringsWithRelevance(StrVecCP& src, const StrVec& words, StrVecCP& dst) {
    // The words must be pre-lowercased (the palette pre-lowercases its query
    // words into filterWordsLower, see FilterStringsForQuery): the merged
    // scoring in ScorePaletteItem assumes the needle is already folded.
    FilterAndSortStrings(src, words, dst);
}

void CommandPaletteWnd::FilterStringsForQuery(Str filter, StrVecCP& strings) {
    strings.Reset();
    if (!filter) filter = StrL("");

    bool searchTabs = false, searchHistory = false, searchCommands = false, searchToc = false, searchFavorites = false;
    if (str::StartsWith(filter, kPalettePrefixEverything)) {
        filter = Str(filter.s + 1);
        searchTabs = searchHistory = searchCommands = true;
    } else if (str::StartsWith(filter, kPalettePrefixTabs)) {
        filter = Str(filter.s + 1);
        searchTabs = true;
    } else if (str::StartsWith(filter, kPalettePrefixFileHistory)) {
        filter = Str(filter.s + 1);
        searchHistory = true;
    } else if (str::StartsWith(filter, kPalettePrefixTOC)) {
        filter = Str(filter.s + 1);
        searchToc = true;
    } else if (str::StartsWith(filter, kPalettePrefixFavorites)) {
        filter = Str(filter.s + 1);
        searchFavorites = true;
    } else {
        if (str::StartsWith(filter, kPalettePrefixCommands)) {
            filter = Str(filter.s + 1);
        }
        searchCommands = true;
    }

    filterWords.Reset();
    SplitFilterToWords(filter, filterWords);

    // Pre-lowercase the query words into temp arena storage (freed at the next
    // message loop) so ScorePaletteSingleWord doesn't fold the needle on every
    // character comparison. filterWords itself is left untouched: its Str slices
    // may alias the edit control's text buffer and it's also used verbatim by
    // the highlight drawing (DrawMaybeHighlightedText case-folds internally).
    filterWordsLower.Reset();
    for (int i = 0; i < len(filterWords); i++) {
        Str w = filterWords.At(i);
        TempStr lower = str::DupTemp(w);
        str::ToLowerInPlace(lower);
        filterWordsLower.Append(lower);
    }

    if (searchCommands) FilterStrings(commands, filterWordsLower, strings, PaletteGroup_Commands);
    if (searchTabs) FilterStrings(tabs, filterWordsLower, strings, PaletteGroup_Tabs);
    if (searchHistory) FilterStrings(fileHistory, filterWordsLower, strings, PaletteGroup_FileHistory);
    if (searchToc) FilterStrings(toc, filterWordsLower, strings, PaletteGroup_TOC);
    if (searchFavorites) FilterStrings(favorites, filterWordsLower, strings, PaletteGroup_Favorites);

    // Show a placeholder when no items match the query
    if (len(strings) == 0 && len(filterWords) > 0) {
        ItemDataCP placeholder;
        placeholder.cmdId = 0;
        placeholder.relevanceScore = -1; // sentinel: renders as "no match" message
        strings.Append(StrL("(no matching items)"), placeholder);
    }
}

void CommandPaletteWnd::QueryChanged() {
    Str filter = CommandPaletteSkipWS(Str(editQuery->GetTextTemp()));
    int currSelIdx = 0;
    auto m = (ListBoxModelCP*)listBox->model;
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
    UpdateResultCount();
    if (nItems == 0) return;
    // If the only item is the "(no matching items)" placeholder, select it
    if (nItems == 1 && m->Data(0) && m->Data(0)->relevanceScore == -1) {
        CommandPaletteSetCurrentSelection(this, 0);
        return;
    }
    if (stickyMode && nItemsPrev == nItems) {
        CommandPaletteSetCurrentSelection(this, currSelIdx);
        return;
    }
    if (str::StartsWith(filter, kPalettePrefixTOC) && len(filterWords) == 0) {
        int idx = (currTocIdx >= 0 && currTocIdx < nItems) ? currTocIdx : 0;
        CommandPaletteSetCurrentSelection(this, idx);
        return;
    }
    CommandPaletteSetCurrentSelection(this, 0);
}

void CommandPaletteWnd::UpdateResultCount() {
    if (!listBox || !staticInfo) {
        return;
    }
    auto m = (ListBoxModelCP*)listBox->model;
    int real = 0;
    for (int i = 0; i < m->ItemsCount(); i++) {
        ItemDataCP* d = m->Data(i);
        if (!d || d->relevanceScore != -1) {
            real++;
        }
    }
    TempStr newText = fmt("%d results", real);
    if (!str::Eq(staticInfo->GetTextTemp(), newText)) {
        staticInfo->SetText(newText);
        // The count text changes the width of the results static. Re-run the
        // layout so the hints row re-centers and the static doesn't keep a stale
        // size/position (it was laid out at 0x0 and could end up over the query
        // edit). The palette window keeps its size; only children are
        // repositioned. Skip the layout when the count text didn't change (e.g.
        // extra keystrokes that don't narrow the results): it's pure overhead.
        if (layout) {
            Rect rc = ClientRect(hwnd);
            LayoutToSize(layout, rc.Size());
        }
    }

    if (clearButton) {
        bool hasQuery = len(CommandPaletteSkipWS(Str(editQuery->GetTextTemp()))) > 0;
        if (hasQuery != clearButton->IsVisible()) {
            clearButton->SetIsVisible(hasQuery);
        }
    }
}

void CommandPaletteWnd::ClearQuery() {
    if (!editQuery) {
        return;
    }
    editQuery->SetText(StrL(""));
    HwndSetFocus(editQuery->hwnd);
    // SetText triggers QueryChanged(), which repopulates the list and
    // updates the result count + clear-button visibility.
}
