/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

// SEH-protected COM Release() and third-party DLL detection. Used to defend
// against heap corruption caused by injection hooks — most notably MacType,
// which replaces IDWriteFactory / ID2D1RenderTarget vtables for font
// beautification and can corrupt the process heap while its hooked Release()
// cleanup runs (the app then dies with STATUS_HEAP_CORRUPTION in ntdll.dll
// during window teardown / shutdown).

// Call obj->Release() and swallow the exception when a third-party hook
// (e.g. MacType) raises an access violation / heap corruption from inside
// Release(). *ppObj is nulled before the call so a failed release can't be
// retried / double-freed. Only intended for teardown paths (destructors,
// cache clears, shutdown flushes) — runtime paths should use plain Release()
// so genuine bugs are still caught by the crash handler.
template <typename T>
void SafeReleaseSeh(T** ppObj) {
    if (!ppObj || !*ppObj) {
        return;
    }
    T* obj = *ppObj;
    *ppObj = nullptr;
#ifdef _MSC_VER
    __try {
        obj->Release();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        OutputDebugStringA("SafeReleaseSeh: suppressed exception inside Release() (third-party hook?)\n");
    }
#else
    obj->Release();
#endif
}

// True when a known MacType DLL is loaded into this process (usually via
// AppInit_DLLs / SetWindowsHookEx injection). When detected, SumatraPDF
// skips the deep D2D/DWrite teardown at exit (fast exit) so MacType's
// heap-corrupting cleanup never runs.
inline bool IsMacTypeLoaded() {
    return GetModuleHandleW(L"MacType64.dll") != nullptr || GetModuleHandleW(L"MacType.dll") != nullptr ||
           GetModuleHandleW(L"MacType32.dll") != nullptr;
}
