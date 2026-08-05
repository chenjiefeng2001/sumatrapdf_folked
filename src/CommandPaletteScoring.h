/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

// Pure relevance-scoring / match-detection helpers for the command palette
// filter (CommandPaletteFilter.cpp). Kept header-only so that the unit tests
// (test_util, which links src/base but not the app's CommandPalette*.cpp files)
// exercise exactly the code the app runs.
//
// Words passed to ScorePaletteSingleWord / ScorePaletteItem must be
// pre-lowercased: FilterStringsForQuery lowercases the query words once into
// temp arena storage (Str::ToLowerInPlace on str::DupTemp copies), so the
// inner comparison loops don't have to fold the needle on every character.

struct Str;
struct StrVec;

constexpr int kPaletteSortSkipThreshold = 1000;

inline bool PaletteIsWordBoundary(char c) {
    return c == ' ' || c == '/' || c == '\\' || c == '-' || c == '_' || c == '.' || c == ',' || c == '\0';
}

// Relevance score for a single query word inside a text: 1000 for a full-word
// match, 500 for a match starting at a word boundary, 200 for any substring,
// 50 for a letter-by-letter subsequence match, 0 for no match.
inline int ScorePaletteSingleWord(const char* text, int textLen, const char* word, int wordLen) {
    if (wordLen <= 0 || textLen < wordLen) return 0;
    int score = 0;
    for (int i = 0; i <= textLen - wordLen; i++) {
        bool substrMatch = true;
        for (int j = 0; j < wordLen; j++) {
            char tc = text[i + j];
            // word is pre-lowercased by the caller; only fold the haystack
            if (tc >= 'A' && tc <= 'Z') tc += 0x20;
            if (tc != word[j]) {
                substrMatch = false;
                break;
            }
        }
        if (!substrMatch) continue;
        bool isWordStart = (i == 0) || PaletteIsWordBoundary(text[i - 1]);
        int end = i + wordLen;
        bool isWordEnd = (end >= textLen) || PaletteIsWordBoundary(text[end]);
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
            char wc = word[wi]; // already lowercased
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

// Combined match detection + relevance scoring for one palette item. Returns
// the relevance score (which includes the text-length penalty and can be
// negative) and sets *matched = true when the item matches. Match semantics
// mirror the previous two-pass code (FilterMatches + ComputeRelevanceScore):
// every word must be a case-insensitive substring of the text.
// ScorePaletteSingleWord already detects ASCII substring matches
// (score >= 200); non-ASCII text is confirmed with str::ContainsI (which
// case-folds via CharLowerBuffW, e.g. 'É' vs 'é'), preserving the Unicode
// matching the old FilterMatches provided.
inline int ScorePaletteItem(Str text, const StrVec& lowerWords, bool* matched) {
    if (matched) *matched = false;
    if (str::IsEmpty(text)) return 0;
    if (len(lowerWords) == 0) { // empty query matches everything, fixed score
        if (matched) *matched = true;
        return 100;
    }
    const char* s = text.s;
    int textLen = text.len;
    int totalScore = 0;
    for (int wi = 0; wi < len(lowerWords); wi++) {
        Str w = lowerWords.At(wi);
        if (str::IsEmpty(w)) continue;
        int ws = ScorePaletteSingleWord(s, textLen, w.s, w.len);
        if (ws < 200 && !str::ContainsI(text, w)) {
            return 0;
        }
        totalScore += ws;
    }
    if (matched) *matched = true;
    return totalScore - textLen / 8;
}

// Very broad queries (e.g. every TOC entry matching "*c") can yield hundreds
// or thousands of results; ranking barely matters there, so skip the sort and
// keep the source order (see FilterAndSortStrings).
inline bool PaletteShouldSkipSort(int matchedCount) {
    return matchedCount > kPaletteSortSkipThreshold;
}

// Tiny keyed cache mapping (command id, language code) -> persistent shortcut
// string, used by the palette list drawing so a command's localized shortcut
// is built once per (command, language) instead of on every row paint. The
// build callback returns a temp (arena) string; the cache owns its copies.
// Kept in this header so unit tests exercise the cache invariants without
// linking the app (Accelerators.cpp / Translations.cpp).
struct PaletteAccelCache {
    struct Entry {
        i32 cmdId = 0;
        Str langCode;
        Str key;
    };
    Vec<Entry> entries;

    ~PaletteAccelCache() {
        for (auto& e : entries) {
            str::Free(e.key);
            str::Free(e.langCode);
        }
    }

    Str Get(int cmdId, Str langCode, Str (*build)(int cmdId)) {
        for (auto& e : entries) {
            if (e.cmdId == cmdId && str::Eq(e.langCode, langCode)) {
                return e.key;
            }
        }
        Str key = str::Dup(build(cmdId));
        entries.Append({cmdId, str::Dup(langCode), key});
        return key;
    }
};
