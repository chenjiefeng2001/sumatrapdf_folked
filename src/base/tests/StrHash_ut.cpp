/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// Unit tests for the constexpr string hashing (base/StrHash.h). The
// static_asserts prove the hashes are computed at compile time; the runtime
// checks prove the compile-time results match the same algorithm evaluated at
// run time.

#include "base/Base.h"
#include "base/StrHash.h"

// must be last due to assert() over-write
#include "base/UtAssert.h"

// compile-time evaluation must be possible for string literals
static_assert(HashStrFnv1a32("") == 2166136261u);
static_assert(HashStrFnv1a32("a") == 3826002220u);
static_assert(HashStrFnv1a32("ab") == 1294271946u);
static_assert(HashStrFnv1a32("SumatraPDF") == 2098754730u);
static_assert(HashStrFnv1a32("CmdCommandPalette") == 879944967u);

static void HashMatchesRuntimeTest() {
    // the same input hashed through a runtime loop gives the same value
    const char* s = "page";
    utassert(HashStrFnv1a32(s) == 2170419830u);
    utassert(HashStrFnv1a32(StrL("CmdCommandPalette").s) == 879944967u);

    // 64-bit variant is a superset: values must differ from the 32-bit one
    utassert(HashStrFnv1a64("a") != HashStrFnv1a32("a"));
    utassert(HashStrFnv1a64("a") == 12638187200555641996ull);
    utassert(HashStrFnv1a64("") == 14695981039346656037ull);

    // deterministic: repeated calls are stable
    utassert(HashStrFnv1a32("SumatraPDF") == HashStrFnv1a32("SumatraPDF"));
}

void StrHashTest() {
    HashMatchesRuntimeTest();
}
