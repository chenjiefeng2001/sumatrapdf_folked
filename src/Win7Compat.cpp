// Copyright 2024 the SumatraPDF project authors (see AUTHORS file).
// License: GPLv3

// Win7Compat.cpp — Windows 7 compatibility shim for MSVC 2022 CRT.
//
// The MSVC 2022 CRT static libraries (LIBCMT/LIBCPMT) include Event
// Tracing for Windows (ETW) provider registration code.  This code
// imports EventSetInformation, EventWriteTransfer and EventUnregister
// from advapi32.dll at link time via __imp_* symbols.
//
// Those three functions do not exist on Windows 7.  When the PE loader
// walks the IAT of a statically-linked executable (built with /MT) it
// tries to resolve every __imp_* entry — including the CRT's ETW ones
// — against the loaded advapi32.dll.  On Win7 the lookup fails and the
// process dies with:
//
//   "无法定位程序输入点 EventSetInformation 于动态链接库 ADVAPI32.dll 上"
//
// This file supplies strong data definitions of the __imp_EventSetInformation,
// __imp_EventWriteTransfer and __imp_EventUnregister symbols, pointing to
// no-op stubs.  Because the linker processes project .obj files before
// import libraries, our definitions win over advapi32.lib's import objects,
// so the final PE has no IAT entries for those three functions.
//
// The CRT then calls our stubs, which return ERROR_SUCCESS.  SumatraPDF
// does not use ETW, so silently discarding those calls is harmless.

#include "base/Base.h"

// --- Stub implementations ---

// EventSetInformation (also called EventProviderSetUserData) controls
// ETW provider traits.  Our stub is a no-op.
static ULONG WINAPI Stub_EventSetInformation(HANDLE /*hProvider*/, ULONG /*InfoClass*/, void* /*pvInfo*/,
                                             ULONG /*cbInfo*/) {
    return ERROR_SUCCESS;
}

// EventWriteTransfer emits an ETW event.  Our stub is a no-op.
static ULONG WINAPI Stub_EventWriteTransfer(HANDLE /*hRegHandle*/, const void* /*pEventDescriptor*/,
                                            const void* /*pActivityId*/, const void* /*pRelatedActivityId*/,
                                            ULONG /*cUserDataCount*/, const void* /*pUserData*/) {
    return ERROR_SUCCESS;
}

// EventUnregister tears down an ETW registration.  Our stub is a no-op.
static ULONG WINAPI Stub_EventUnregister(HANDLE /*hRegHandle*/) {
    return ERROR_SUCCESS;
}

// --- Override the __imp_ IAT symbols ---
//
// The CRT references __imp_EventSetInformation (and friends) as if they
// were imported from advapi32.dll.  By defining them as plain data
// pointers in an object file compiled into the executable, the linker
// uses our definition instead of creating an import-table entry.
// This works because .obj files are processed *before* .lib files,
// and our strong (non-COMDAT) symbol takes priority.

extern "C" {

void* __imp_EventSetInformation = (void*)Stub_EventSetInformation;
void* __imp_EventWriteTransfer = (void*)Stub_EventWriteTransfer;
void* __imp_EventUnregister = (void*)Stub_EventUnregister;

} // extern "C"
