/* Copyright 2024 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

// Compile-time FNV-1a hash for replacing runtime strcmp() with O(1) integer ops.
// Include after Base.h (needs u32).

#ifndef Hash_h
#define Hash_h

// FNV-1a hash for a NUL-terminated string literal, computed at compile time.
constexpr u32 HashLiteral(const char* str) {
    u32 hash = 2166136261u;
    for (int i = 0; str[i] != '\0'; ++i) {
        hash ^= (u8)str[i];
        hash *= 16777619u;
    }
    return hash;
}

// FNV-1a hash for a Str (length-known view), computed at runtime (fast).
inline u32 HashStr(Str s) {
    u32 hash = 2166136261u;
    for (int i = 0; i < s.len; ++i) {
        hash ^= (u8)s.s[i];
        hash *= 16777619u;
    }
    return hash;
}

// Hash a runtime C string (NUL-terminated), e.g. from settings lookup.
inline u32 HashCStr(const char* s) {
    if (!s) return 0;
    u32 hash = 2166136261u;
    for (; *s; ++s) {
        hash ^= (u8)*s;
        hash *= 16777619u;
    }
    return hash;
}

// Macro: yields a compile-time integral constant for a string literal.
#define HASH_LIT(s) std::integral_constant<u32, HashLiteral(s)>::value

// FNV-1a for case-insensitive matching (lowercases ASCII).
constexpr u32 HashLiteralI(const char* str) {
    u32 hash = 2166136261u;
    for (int i = 0; str[i] != '\0'; ++i) {
        u8 c = (u8)str[i];
        if (c >= 'A' && c <= 'Z') c += 0x20;
        hash ^= c;
        hash *= 16777619u;
    }
    return hash;
}

inline u32 HashStrI(Str s) {
    u32 hash = 2166136261u;
    for (int i = 0; i < s.len; ++i) {
        u8 c = (u8)s.s[i];
        if (c >= 'A' && c <= 'Z') c += 0x20;
        hash ^= c;
        hash *= 16777619u;
    }
    return hash;
}

#define HASH_LIT_I(s) std::integral_constant<u32, HashLiteralI(s)>::value

// Fast-path StartsWith using compile-time hash: hash the first N bytes of `s`
// (where N = len(lit) - 1) and compare to HASH_LIT(lit). Falls through to full
// comparison on hash collision (theoretically possible but practically nonexistent).
#define StartsWithLit(s, lit)                                 \
    ([]() -> bool {                                           \
        constexpr u32 kHash = HASH_LIT(lit);                  \
        constexpr int kLen = dimofi(lit) - 1;                 \
        if ((s).len < kLen) return false;                     \
        u32 h = 2166136261u;                                  \
        for (int i = 0; i < kLen; ++i) {                      \
            h ^= (u8)(s).s[i];                                \
            h *= 16777619u;                                   \
        }                                                     \
        return h == kHash && memcmp((s).s, (lit), kLen) == 0; \
    }())

#endif // Hash_h
