/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

#include "base/Base.h"

#include "wingui/UIModels.h"
#include "wingui/Layout.h"
#include "wingui/WinGui.h"

#include "base/Win.h"
#include "FilterHighlightDraw.h"
#include "CommandPalette.h"
#include "CommandPaletteInternal.h"

// ---------------------------------------------------------------------------
// Relevance scoring for search results
// ---------------------------------------------------------------------------

static bool IsWordBoundary(char c) {
    return c == ' ' || c == '/' || c == '\\' || c == '-' || c == '_' || c == '.' || c == ',' || c == '\0';
}

static int ScoreSingleWord(const char* text, int textLen, const char* word, int wordLen) {
    if (wordLen <= 0 || textLen < wordLen) return 0;
    int score = 0;
    for (int i = 0; i <= textLen - wordLen; i++) {
        bool substrMatch = true;
        for (int j = 0; j < wordLen; j++) {
            char tc = text[i + j], wc = word[j];
            if (tc >= 'A' && tc <= 'Z') tc += 0x20;
            if (wc >= 'A' && wc <= 'Z') wc += 0x20;
            if (tc != wc) {
                substrMatch = false;
                break;
            }
        }
        if (!substrMatch) continue;
        bool isWordStart = (i == 0) || IsWordBoundary(text[i - 1]);
        int end = i + wordLen;
        bool isWordEnd = (end >= textLen) || IsWordBoundary(text[end]);
        if (isWordStart && isWordEnd) {
            score = 1000;
            break;
        }
        if (isWordStart && score < 500)
            score = 500;
        else if (score < 200)
            score = 200;
    }
    if (score == 0 && wordLen > 1) {
        int ti = 0, matched = 0;
        for (int wi = 0; wi < wordLen && ti < textLen; wi++) {
            char wc = word[wi];
            if (wc >= 'A' && wc <= 'Z') wc += 0x20;
            while (ti < textLen) {
                char tc = text[ti++];
                if (tc >= 'A' && tc <= 'Z') tc += 0x20;
                if (tc == wc) {
                    matched++;
                    break;
                }
            }
        }
        if (matched == wordLen) score = 50;
    }
    return score;
}

static int ComputeRelevanceScore(Str itemText, const StrVec& words) {
    if (len(words) == 0) return 100;
    int totalScore = 0;
    const char* text = itemText.s;
    int textLen = itemText.len;
    for (int wi = 0; wi < len(words); wi++) {
        Str w = words.At(wi);
        totalScore += ScoreSingleWord(text, textLen, w.s, w.len);
    }
    totalScore -= textLen / 8;
    return totalScore;
}

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
        Str s = src.At(i);
        if (str::IsEmpty(s)) continue;
        if (!FilterMatches(s, words)) continue;
        int score = ComputeRelevanceScore(s, words);
        scored.Append({i, score});
    }
    if (len(scored) == 0) return;
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

    if (searchCommands) FilterStrings(commands, filterWords, strings, PaletteGroup_Commands);
    if (searchTabs) FilterStrings(tabs, filterWords, strings, PaletteGroup_Tabs);
    if (searchHistory) FilterStrings(fileHistory, filterWords, strings, PaletteGroup_FileHistory);
    if (searchToc) FilterStrings(toc, filterWords, strings, PaletteGroup_TOC);
    if (searchFavorites) FilterStrings(favorites, filterWords, strings, PaletteGroup_Favorites);

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
    staticInfo->SetText(fmt("%d results", real));
    ::SizeToIdealSize(staticInfo);

    if (clearButton) {
        bool hasQuery = len(CommandPaletteSkipWS(Str(editQuery->GetTextTemp()))) > 0;
        clearButton->SetIsVisible(hasQuery);
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
