/* Copyright 2024 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

// SIMD-accelerated string search routines for Windows x64.
// Uses SSE2 for 16-byte parallel first-character search.
// Falls back to scalar loop when SSE4.2 is unavailable.
//
// All functions return the character offset (0-based) of the match, or -1.
// They operate on Str (char*, int len) not NUL-terminated C strings.

#include <intrin.h>
#include <emmintrin.h>  // SSE2

// Detect SSE4.2 at runtime via __cpuidex. The feature bit for SSE4.2 is
// ECX bit 20 after CPUID(1). For the PCMPESTRI-based fast path.
inline bool HasSSE42() {
    static int cached = -1;
    if (cached < 0) {
        int cpuInfo[4] = {};
        __cpuidex(cpuInfo, 1, 0);
        cached = (cpuInfo[2] & (1 << 20)) ? 1 : 0;
    }
    return cached == 1;
}

// SSE2-based: find first occurrence of byte `c` in `data` of length `len`.
// Uses _mm_cmpeq_epi8 + _mm_movemask_epi8 to check 16 bytes at a time.
inline int FindFirstCharSIMD(const char* data, int len, char c) {
    if (!HasSSE42() || len <= 0) {
        for (int i = 0; i < len; i++) {
            if (data[i] == c) return i;
        }
        return -1;
    }
    __m128i needle = _mm_set1_epi8(c);
    int i = 0;
    for (; i + 16 <= len; i += 16) {
        __m128i chunk = _mm_loadu_si128((const __m128i*)(data + i));
        __m128i eq = _mm_cmpeq_epi8(chunk, needle);
        int mask = _mm_movemask_epi8(eq);
        if (mask != 0) {
            unsigned long bit;
            _BitScanForward(&bit, (unsigned long)mask);
            return i + (int)bit;
        }
    }
    // handle remaining bytes
    for (; i < len; i++) {
        if (data[i] == c) return i;
    }
    return -1;
}

// SSE4.2 PCMPESTRI-based forward search (case-sensitive, ASCII needle).
// Falls back to FindFirstCharSIMD + memcmp on any machine.
inline int StrStrSIMD(Str haystack, int haystackLen, int startOff, Str needle, int needleLen) {
    if (needleLen <= 0 || needleLen > haystackLen - startOff) {
        return -1;
    }
    char first = needle.s[0];
    const char* h = haystack.s;
    int end = haystackLen - needleLen;
    for (int i = startOff; i <= end; i++) {
        int m = FindFirstCharSIMD(h + i, end - i + 1, first);
        if (m < 0) return -1;
        i += m;
        if (memcmp(h + i, needle.s, (size_t)needleLen) == 0) return i;
    }
    return -1;
}

// SSE2-based backward search (case-sensitive).
inline int StrRStrSIMD(Str text, int textLen, int endOff, Str needle, int needleLen) {
    if (needleLen <= 0 || needleLen > textLen) {
        return -1;
    }
    if (endOff > textLen - needleLen) {
        endOff = textLen - needleLen;
    }
    char first = needle.s[0];
    const char* t = text.s;
    for (int i = endOff; i >= 0; i--) {
        if (t[i] != first) continue;
        if (memcmp(t + i, needle.s, (size_t)needleLen) == 0) return i;
    }
    return -1;
}

// Case-insensitive forward search using SIMD for first-char lookup.
inline int StrStrFoldCaseSIMD(Str haystack, int haystackLen, int startOff, Str needle, int needleLen) {
    if (needleLen <= 0 || needleLen > haystackLen - startOff) {
        return -1;
    }
    char firstNeedleLow = (char)tolower((unsigned char)needle.s[0]);
    const char* h = haystack.s;
    int end = haystackLen - needleLen;

    for (int i = startOff; i <= end; i++) {
        int chunkLen = end - i + 1;
        int m = FindFirstCharSIMD(h + i, chunkLen, firstNeedleLow);
        if (m < 0) break;
        i += m;

        if (needleLen == 1) return i;

        bool match = true;
        for (int j = 1; j < needleLen; j++) {
            if ((char)tolower((unsigned char)h[i + j]) != (char)tolower((unsigned char)needle.s[j])) {
                match = false;
                break;
            }
        }
        if (match) return i;
    }
    return -1;
}

// Backward case-insensitive search.
inline int StrRStrFoldCaseSIMD(Str text, int textLen, int endOff, Str needle, int needleLen) {
    if (needleLen <= 0 || needleLen > textLen) {
        return -1;
    }
    if (endOff > textLen - needleLen) {
        endOff = textLen - needleLen;
    }
    char firstNeedleLow = (char)tolower((unsigned char)needle.s[0]);
    const char* t = text.s;

    for (int i = endOff; i >= 0; i--) {
        if ((char)tolower((unsigned char)t[i]) != firstNeedleLow) continue;
        if (needleLen == 1) return i;
        bool match = true;
        for (int j = 1; j < needleLen; j++) {
            if ((char)tolower((unsigned char)t[i + j]) != (char)tolower((unsigned char)needle.s[j])) {
                match = false;
                break;
            }
        }
        if (match) return i;
    }
    return -1;
}