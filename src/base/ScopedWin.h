/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

// ReportIf (defined in Base.h) must be available for the debug assertions below.
#include "Base.h"

// Debug-only thread-local tracking for lock ordering assertions.
// Incremented when entering a ScopedCritSec (CS), decremented on exit.
// ScopedSRWLockShared/Exclusive check this counter on acquire: if > 0,
// the current thread is trying to acquire an SRW lock while holding a CS.
// In the EngineMupdf lock hierarchy (pagesLock→docLock→renderLock),
// this is the violation pattern "holding renderLock[CS] → acquiring docLock[SRW]",
// which is the lock-order inversion that causes deadlock with path C.
#ifdef DEBUG
// Thread-local depth counter for CRITICAL_SECTION acquisitions.
// Declared extern here; defined in Base.cpp so a single TLS instance
// is shared across all translation units.
__declspec(thread) extern int g_tlsCritSecDepth;
// Main thread ID, set by GpuBackend::GpuBackend() which runs on the UI thread.
// Used by D2D thread-affinity assertions.
extern DWORD g_mainThreadId;
#endif

struct ScopedCritSec {
    CRITICAL_SECTION* cs = nullptr;

    explicit ScopedCritSec(CRITICAL_SECTION* cs) : cs(cs) {
#ifdef DEBUG
        ++g_tlsCritSecDepth;
#endif
        EnterCriticalSection(cs);
    }
    ~ScopedCritSec() {
        LeaveCriticalSection(cs);
#ifdef DEBUG
        --g_tlsCritSecDepth;
#endif
    }
};

// RAII wrappers for Windows Slim Reader/Writer Lock (SRWLock).
// SRWLock is NOT recursive — a thread holding shared lock must not acquire
// exclusive (deadlock), and a thread holding exclusive must not acquire
// shared or exclusive again (undefined behaviour / deadlock).
// IMPORTANT: Acquiring an SRW lock while holding a CRITICAL_SECTION that
// serializes document operations (e.g. renderLock → docLock) is a LOCK
// ORDER VIOLATION and WILL cause deadlock.  The debug assertion below
// catches this pattern at runtime.
struct ScopedSRWLockShared {
    SRWLOCK* lock = nullptr;
    explicit ScopedSRWLockShared(SRWLOCK* lock) : lock(lock) {
#ifdef DEBUG
        // Caught a lock-order violation: trying to acquire an SRW lock
        // (e.g. docLock) while holding a CRITICAL_SECTION (e.g. renderLock).
        // This inverts the pagesLock→docLock→renderLock hierarchy and
        // WILL deadlock with path C (see docs/reports/multithreading-report.md §4.1).
        ReportIf(g_tlsCritSecDepth > 0);
#endif
        AcquireSRWLockShared(lock);
    }
    ~ScopedSRWLockShared() { ReleaseSRWLockShared(lock); }
};

struct ScopedSRWLockExclusive {
    SRWLOCK* lock = nullptr;
    explicit ScopedSRWLockExclusive(SRWLOCK* lock) : lock(lock) {
#ifdef DEBUG
        // Same lock-order violation detection as ScopedSRWLockShared.
        ReportIf(g_tlsCritSecDepth > 0);
#endif
        AcquireSRWLockExclusive(lock);
    }
    ~ScopedSRWLockExclusive() { ReleaseSRWLockExclusive(lock); }
};

class AutoCloseHandle {
    HANDLE handle = nullptr;

  public:
    AutoCloseHandle() = default;

    AutoCloseHandle(HANDLE h) : handle(h) {}

    ~AutoCloseHandle() {
        if (IsValid()) {
            CloseHandle(handle);
        }
    }

    AutoCloseHandle& operator=(HANDLE h) {
        ReportIf(handle != nullptr);
        ReportIf(h == nullptr);
        handle = h;
        return *this;
    }

    operator HANDLE() const { // NOLINT
        return handle;
    }

    bool IsValid() const { return handle != nullptr && handle != INVALID_HANDLE_VALUE; }
};

template <class T>
class ScopedComPtr {
  protected:
    T* ptr = nullptr;

  public:
    ScopedComPtr() = default;

    explicit ScopedComPtr(T* ptr) : ptr(ptr) {}
    ~ScopedComPtr() {
        if (ptr) {
            ptr->Release();
        }
    }
    bool Create(const CLSID clsid) {
        ReportIf(ptr);
        if (ptr) {
            return false;
        }
        HRESULT hr = CoCreateInstance(clsid, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&ptr));
        return SUCCEEDED(hr);
    }
    T* Get() const { return ptr; }
    operator T*() const { // NOLINT
        return ptr;
    }
    T** operator&() { return &ptr; }
    T* operator->() const { return ptr; }
    ScopedComPtr<T>& operator=(T* newPtr) {
        if (ptr) {
            ptr->Release();
        }
        ptr = newPtr;
        return *this;
    }
};

template <class T>
class ScopedComQIPtr {
  protected:
    T* ptr = nullptr;

  public:
    ScopedComQIPtr() = default;

    explicit ScopedComQIPtr(IUnknown* unk) {
        HRESULT hr = unk->QueryInterface(&ptr);
        if (FAILED(hr)) {
            ptr = nullptr;
        }
    }
    ~ScopedComQIPtr() {
        if (ptr) {
            ptr->Release();
        }
    }
    bool Create(const CLSID clsid) {
        ReportIf(ptr);
        if (ptr) return false;
        HRESULT hr = CoCreateInstance(clsid, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&ptr));
        return SUCCEEDED(hr);
    }
    T* operator=(IUnknown* newUnk) {
        if (ptr) ptr->Release();
        HRESULT hr = newUnk->QueryInterface(&ptr);
        if (FAILED(hr)) ptr = nullptr;
        return ptr;
    }
    operator T*() const { // NOLINT
        return ptr;
    }
    T** operator&() { return &ptr; }
    T* operator->() const { return ptr; }
    T* operator=(T* newPtr) {
        if (ptr) ptr->Release();
        return (ptr = newPtr);
    }
};

struct AutoDeleteDC {
    HDC hdc = nullptr;

    explicit AutoDeleteDC(HDC hdc) { this->hdc = hdc; }
    AutoDeleteDC() = default;

    ~AutoDeleteDC() { DeleteDC(hdc); }
    operator HDC() const { // NOLINT
        return hdc;
    }
};

struct AutoReleaseDC {
    HWND hwnd = nullptr;
    HDC hdc = nullptr;

    explicit AutoReleaseDC(HWND hwnd) { hdc = GetWindowDC(hwnd); }
    AutoReleaseDC() = default;

    ~AutoReleaseDC() { ReleaseDC(hwnd, hdc); }
    operator HDC() const { // NOLINT
        return hdc;
    }
};

template <typename T>
class ScopedGdiObj {
    T obj;

  public:
    ScopedGdiObj(T obj) { // NOLINT
        this->obj = obj;
    }
    ~ScopedGdiObj() { DeleteObject(obj); }
    operator T() const { // NOLINT
        return obj;
    }
};
using AutoDeletePen = ScopedGdiObj<HPEN>;
using AutoDeleteBrush = ScopedGdiObj<HBRUSH>;

class ScopedGetDC {
    HDC hdc = nullptr;
    HWND hwnd = nullptr;

  public:
    explicit ScopedGetDC(HWND hwnd) {
        this->hwnd = hwnd;
        this->hdc = GetDC(hwnd);
    }
    ~ScopedGetDC() { ReleaseDC(hwnd, hdc); }
    operator HDC() const { // NOLINT
        return hdc;
    }
};

class ScopedSelectObject {
    HDC hdc = nullptr;
    HGDIOBJ obj = nullptr;
    HGDIOBJ prev = nullptr;

  public:
    ScopedSelectObject(HDC hdc, HGDIOBJ obj, bool alsoDelete = false) {
        this->hdc = hdc;
        this->prev = SelectObject(hdc, obj);
        if (alsoDelete) {
            this->obj = obj;
        }
    }

    ~ScopedSelectObject() {
        SelectObject(hdc, prev);
        if (obj) {
            DeleteObject(obj);
        }
    }
};

class ScopedSelectFont {
    HDC hdc = nullptr;
    HGDIOBJ prev = nullptr;

  public:
    // font can be nullptr
    explicit ScopedSelectFont(HDC hdc, HFONT font) {
        this->hdc = hdc;
        if (font) {
            prev = (HFONT)SelectObject(hdc, font);
        }
    }

    ~ScopedSelectFont() {
        if (prev) {
            SelectObject(hdc, prev);
        }
    }
};

struct ScopedSelectPen {
    HDC hdc = nullptr;
    HPEN prevPen = nullptr;

    explicit ScopedSelectPen(HDC hdc, HPEN pen) : hdc(hdc) { this->prevPen = (HPEN)SelectObject(hdc, pen); }

    ~ScopedSelectPen() { SelectObject(hdc, prevPen); }
};

class ScopedSelectBrush {
    HDC hdc = nullptr;
    HBRUSH prevBrush = nullptr;

  public:
    explicit ScopedSelectBrush(HDC hdc, HBRUSH pen) { prevBrush = (HBRUSH)SelectObject(hdc, pen); }

    ~ScopedSelectBrush() { SelectObject(hdc, prevBrush); }
};
class ScopedCom {
  public:
    ScopedCom() { (void)CoInitialize(nullptr); }
    ~ScopedCom() { CoUninitialize(); }
};

class ScopedOle {
  public:
    ScopedOle() { (void)OleInitialize(nullptr); }
    ~ScopedOle() { OleUninitialize(); }
};

class ScopedGdiPlus {
  protected:
    Gdiplus::GdiplusStartupInput si{};
    Gdiplus::GdiplusStartupOutput so{};
    ULONG_PTR token = 0;
    ULONG_PTR hookToken = 0;
    bool noBgThread = false;

  public:
    // suppress the GDI+ background thread when initiating in WinMain,
    // as that thread causes DDE messages to be sent too early and
    // thus causes unexpected timeouts
    explicit ScopedGdiPlus(bool inWinMain = false) : noBgThread(inWinMain) {
        si.SuppressBackgroundThread = noBgThread;
        Gdiplus::GdiplusStartup(&token, &si, &so);
        if (noBgThread) {
            so.NotificationHook(&hookToken);
        }
    }
    ~ScopedGdiPlus() {
        if (noBgThread) {
            so.NotificationUnhook(hookToken);
        }
        Gdiplus::GdiplusShutdown(token);
    }
};
