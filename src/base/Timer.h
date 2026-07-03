/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

// Relatively high-precision timer. Can be used e.g. for measuring execution
// time of a piece of code.

inline LARGE_INTEGER TimeGet() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t;
}

inline double TimeSinceInMs(LARGE_INTEGER start) {
    LARGE_INTEGER t = TimeGet();
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    double timeInSecs = (double)(t.QuadPart - start.QuadPart) / (double)freq.QuadPart;
    return timeInSecs * 1000.0;
}

// Microsecond-precision wrappers for measuring short operations (tile compositing).
// Returns microseconds as a 64-bit integer (not double).
inline i64 TimeGetUs() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

inline i64 TimeSinceInUs(i64 start) {
    i64 now = TimeGetUs();
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    if (freq.QuadPart == 0) return 0;
    // convert QPC ticks to microseconds
    return (now - start) * 1000000 / freq.QuadPart;
}