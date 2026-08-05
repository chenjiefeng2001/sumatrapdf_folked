/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

// Unit tests for the command palette filter's pure scoring / matching helpers
// (CommandPaletteScoring.h):
//   1. ScorePaletteSingleWord: 1000 full word / 500 word-start / 200 substring /
//      50 subsequence / 0 no-match, ASCII case-insensitive
//   2. ScorePaletteItem: merged FilterMatches + relevance scoring, including the
//      Unicode (CharLowerBuffW) match path that the ASCII-only scorer misses
//   3. PaletteShouldSkipSort: very broad queries keep source order
//
// Header-only helpers are exercised directly; test_util doesn't link the
// app's CommandPalette*.cpp files.

#include "base/Base.h"
#include "CommandPaletteScoring.h"

// must be last due to assert() over-write
#include "base/UtAssert.h"

static void ScoreSingleWordTests() {
    // full word match at word start + word end -> 1000
    utassert(ScorePaletteSingleWord("open file", 9, "open", 4) == 1000);
    // match at word start but followed by more word chars -> 500
    utassert(ScorePaletteSingleWord("opening", 7, "open", 4) == 500);
    utassert(ScorePaletteSingleWord("re-open", 7, "open", 4) == 1000);        // '-' is a boundary
    utassert(ScorePaletteSingleWord("open/closed", 11, "closed", 6) == 1000); // '/' is a boundary
    // substring match not starting at a word boundary -> 200
    utassert(ScorePaletteSingleWord("unopened", 8, "open", 4) == 200);
    // no match -> 0
    utassert(ScorePaletteSingleWord("closing", 7, "open", 4) == 0);
    // ASCII case-insensitive (word must be pre-lowercased, text can be any case)
    utassert(ScorePaletteSingleWord("OPEN FILE", 9, "open", 4) == 1000);
    utassert(ScorePaletteSingleWord("OpEnInG", 7, "open", 4) == 500);
    // letter-by-letter subsequence match -> 50
    utassert(ScorePaletteSingleWord("o-p-e-n", 7, "open", 4) == 50);
    utassert(ScorePaletteSingleWord("oXpYeZn", 7, "open", 4) == 50);
    // single-char words have no subsequence branch
    utassert(ScorePaletteSingleWord("qwerty", 6, "x", 1) == 0);
    // word longer than text -> 0
    utassert(ScorePaletteSingleWord("abc", 3, "abcd", 4) == 0);
    // empty word -> 0
    utassert(ScorePaletteSingleWord("abc", 3, "", 0) == 0);
}

static void ScorePaletteItemTests() {
    bool matched = false;
    // empty query matches everything with the fixed 100 score
    StrVec emptyWords;
    utassert(ScorePaletteItem(StrL("anything"), emptyWords, &matched) == 100 && matched);
    // empty text never matches a non-empty query
    StrVec w;
    w.Append(StrL("open"));
    utassert(ScorePaletteItem(StrL(""), w, &matched) == 0 && !matched);

    // single word: score includes the length penalty (len/8)
    int score = ScorePaletteItem(StrL("open file"), w, &matched);
    utassert(matched && score == 1000 - 9 / 8);
    matched = true;
    utassert(ScorePaletteItem(StrL("closing"), w, &matched) == 0 && !matched);

    // multi-word query: every word must match (AND semantics)
    StrVec w2;
    w2.Append(StrL("open"));
    w2.Append(StrL("file"));
    utassert(ScorePaletteItem(StrL("open the file"), w2, &matched) >= 0 && matched);
    matched = true;
    utassert(ScorePaletteItem(StrL("open the door"), w2, &matched) == 0 && !matched);

    // subsequence-only match does NOT match (mirrors old FilterMatches which
    // required a case-insensitive substring per word)
    StrVec w3;
    w3.Append(StrL("open"));
    matched = true;
    utassert(ScorePaletteItem(StrL("o-p-e-n"), w3, &matched) == 0 && !matched);

    // non-ASCII text: ScorePaletteSingleWord only folds ASCII, so the Unicode
    // (CharLowerBuffW) path via str::ContainsI must confirm the match; 'É'
    // (0xC3 0x89) vs 'é' (0xC3 0xA9) exercises it. A matched-but-zero ASCII
    // score (0 - len/8) must still be reported as a match.
    StrVec w4;
    w4.Append(StrL("pr\xC3\xA9s"));      // "prés" (lowercase é)
    Str uniText = "Pr\xC3\x89Sentation"; // "PrÉSentation" (uppercase É)
    matched = false;
    int unicodeScore = ScorePaletteItem(uniText, w4, &matched);
    utassert(matched);
    utassert(unicodeScore < 200); // must not be the ASCII substring path

    // ASCII query still matches a text with non-ASCII word chars byte-wise
    StrVec w5;
    w5.Append(StrL("pr\xC3\xA9")); // "pré" needs no folding, matches "Pré..."
    matched = false;
    utassert(ScorePaletteItem(StrL("Pr\xC3\xA9sent"), w5, &matched) >= 0 && matched);
}

static void ShouldSkipSortTests() {
    utassert(!PaletteShouldSkipSort(0));
    utassert(!PaletteShouldSkipSort(999));
    utassert(!PaletteShouldSkipSort(kPaletteSortSkipThreshold));
    utassert(PaletteShouldSkipSort(kPaletteSortSkipThreshold + 1));
    utassert(PaletteShouldSkipSort(5000));
}

static int gTestBuildCalls = 0;
static TempStr TestAccelBuild(int cmdId) {
    gTestBuildCalls++;
    return fmt("accel-%d", cmdId);
}

static TempStr TestEmptyAccelBuild([[maybe_unused]] int cmdId) {
    gTestBuildCalls++;
    return StrL("");
}

static void PaletteAccelCacheTests() {
    PaletteAccelCache cache;
    gTestBuildCalls = 0;
    Str a1 = cache.Get(42, StrL("en"), TestAccelBuild);
    utassert(gTestBuildCalls == 1 && str::Eq(a1, StrL("accel-42")));
    // same (command, language): cache hit, same string, no rebuild
    Str a2 = cache.Get(42, StrL("en"), TestAccelBuild);
    utassert(gTestBuildCalls == 1 && a1.s == a2.s);
    // different language code rebuilds (localized shortcut text)
    Str a3 = cache.Get(42, StrL("de"), TestAccelBuild);
    utassert(gTestBuildCalls == 2 && str::Eq(a3, StrL("accel-42")) && a3.s != a1.s);
    // different command id is an independent entry
    Str b1 = cache.Get(43, StrL("en"), TestAccelBuild);
    utassert(gTestBuildCalls == 3 && str::Eq(b1, StrL("accel-43")));
    // an empty build result is cached as well (no repeated building)
    gTestBuildCalls = 0;
    Str e1 = cache.Get(7, StrL("en"), TestEmptyAccelBuild);
    Str e2 = cache.Get(7, StrL("en"), TestEmptyAccelBuild);
    utassert(gTestBuildCalls == 1 && e1.s == e2.s);
    // lookups of other keys don't disturb existing entries
    utassert(str::Eq(cache.Get(42, StrL("en"), TestAccelBuild), StrL("accel-42")));
    utassert(gTestBuildCalls == 1);
}

void CommandPaletteScoringTest() {
    ScoreSingleWordTests();
    ScorePaletteItemTests();
    ShouldSkipSortTests();
    PaletteAccelCacheTests();
}
