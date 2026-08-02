/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

// Compile-time string hashing (FNV-1a). Where the key is a string literal the
// hash can be computed at compile time (static_assert in tests proves it) and
// needs no runtime cost. The values match the classic FNV-1a algorithm, so a
// runtime lookup computed with HashStrFnv1a32 is interchangeable.
//
// Requires the integer types (u32/u64) from base/Base.h to be visible.

inline constexpr u32 HashStrFnv1a32(const char* s) {
    u32 h = 2166136261u;
    for (const char* p = s; *p; p++) {
        h = (h ^ (unsigned char)*p) * 16777619u;
    }
    return h;
}

inline constexpr u64 HashStrFnv1a64(const char* s) {
    u64 h = 14695981039346656037ull;
    for (const char* p = s; *p; p++) {
        h = (h ^ (unsigned char)*p) * 1099511628211ull;
    }
    return h;
}
