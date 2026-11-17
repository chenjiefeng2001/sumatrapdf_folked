/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "base/Base.h"
#include "base/Win.h"
#include "Translations.h"
#include "base/GuessFileType.h"

#include <uiautomationcore.h>

#include "gui/UIModels.h"

#include "Settings.h"
#include "DocController.h"
#include "EngineBase.h"
#include "DisplayModel.h"
#include "uia/PageProvider.h"
#include "uia/Constants.h"
#include "uia/DocumentProvider.h"
#include "uia/Provider.h"
#include "uia/StructureProvider.h"
#include "EngineAll.h"

SumatraUIAutomationPageProvider::SumatraUIAutomationPageProvider(int pageNum, HWND canvasHwnd, DisplayModel* dm,
                                                                 SumatraUIAutomationDocumentProvider* root)
    : refCount(1),
      pageNum(pageNum),
      canvasHwnd(canvasHwnd),
      dm(dm),
      root(root),
      sibling_prev(nullptr),
      sibling_next(nullptr),
      struct_first(nullptr),
      struct_last(nullptr),
      released(false) {
    // root->AddRef(); Don't add refs to our parent & owner.
    BuildStructureElements();
}

SumatraUIAutomationPageProvider::~SumatraUIAutomationPageProvider() {
    SumatraUIAutomationStructureProvider* it = struct_first;
    while (it) {
        SumatraUIAutomationStructureProvider* next = it->GetNext();
        it->Release();
        it = next;
    }
    struct_first = nullptr;
    struct_last = nullptr;
}

// A tagged PDF's structure elements become children of their page, so that a
// screen reader's heading navigation (NVDA's H, Narrator's heading list) finds
// the document's headings instead of nothing. An untagged page gets none, and
// its provider tree is exactly as before.
void SumatraUIAutomationPageProvider::BuildStructureElements() {
    struct_first = nullptr;
    struct_last = nullptr;
    EngineBase* engine = dm ? dm->GetEngine() : nullptr;
    if (!engine) {
        return;
    }
    Vec<PageTextSpan> spans;
    EngineMupdfGetPageStructure(engine, pageNum, &spans);
    SumatraUIAutomationStructureProvider* prev = nullptr;
    for (int i = 0; i < len(spans); i++) {
        auto* current = new SumatraUIAutomationStructureProvider(pageNum, i, canvasHwnd, dm, root, this);
        current->sibling_prev = prev;
        if (prev) {
            prev->sibling_next = current;
        }
        prev = current;
        if (i == 0) {
            struct_first = current;
        }
    }
    struct_last = prev;
    for (PageTextSpan& span : spans) {
        str::Free(span.type);
    }
    spans.Reset();
}

int SumatraUIAutomationPageProvider::GetPageNum() const {
    return pageNum;
}

SumatraUIAutomationPageProvider* SumatraUIAutomationPageProvider::GetNextPage() {
    return sibling_next;
}

SumatraUIAutomationPageProvider* SumatraUIAutomationPageProvider::GetPreviousPage() {
    return sibling_prev;
}

SumatraUIAutomationStructureProvider* SumatraUIAutomationPageProvider::GetNextStruct() {
    return struct_first;
}

SumatraUIAutomationStructureProvider* SumatraUIAutomationPageProvider::GetPreviousStruct() {
    return struct_last;
}

HRESULT STDMETHODCALLTYPE SumatraUIAutomationPageProvider::QueryInterface(REFIID riid, void** ppv) {
    static const QITAB qit[] = {QITABENT(SumatraUIAutomationPageProvider, IRawElementProviderSimple),
                                QITABENT(SumatraUIAutomationPageProvider, IRawElementProviderFragment),
                                QITABENT(SumatraUIAutomationPageProvider, IValueProvider),
                                {nullptr}};
    return QISearch(this, qit, riid, ppv);
}

ULONG STDMETHODCALLTYPE SumatraUIAutomationPageProvider::AddRef() {
    return AtomicIntInc(&refCount);
}

ULONG STDMETHODCALLTYPE SumatraUIAutomationPageProvider::Release() {
    LONG res = InterlockedDecrement(&refCount);
    ReportIf(res < 0);
    if (0 == res) {
        delete this;
    }
    return res;
}

HRESULT STDMETHODCALLTYPE SumatraUIAutomationPageProvider::Navigate(enum NavigateDirection direction,
                                                                    IRawElementProviderFragment** pRetVal) {
    if (pRetVal == nullptr) {
        return E_POINTER;
    }

    // disallow traverse if we are lingering
    if (released) {
        return E_FAIL;
    }

    *pRetVal = nullptr;
    if (direction == NavigateDirection_PreviousSibling) {
        *pRetVal = sibling_prev;
    } else if (direction == NavigateDirection_NextSibling) {
        *pRetVal = sibling_next;
    } else if (direction == NavigateDirection_FirstChild) {
        *pRetVal = struct_first;
    } else if (direction == NavigateDirection_LastChild) {
        *pRetVal = struct_last;
    } else if (direction == NavigateDirection_Parent) {
        *pRetVal = root;
    } else {
        return E_INVALIDARG;
    }

    if (*pRetVal) {
        (*pRetVal)->AddRef();
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE SumatraUIAutomationPageProvider::GetRuntimeId(SAFEARRAY** pRetVal) {
    if (pRetVal == nullptr) {
        return E_POINTER;
    }

    SAFEARRAY* psa = SafeArrayCreateVector(VT_I4, 0, 2);
    if (!psa) {
        return E_OUTOFMEMORY;
    }

    // RuntimeID magic, use hwnd to differentiate providers of different windows
    int rId[] = {HandleToLong(canvasHwnd), SUMATRA_UIA_PAGE_RUNTIME_ID(pageNum)};
    for (LONG i = 0; i < 2; i++) {
        HRESULT hr = SafeArrayPutElement(psa, &i, (void*)&(rId[i]));
        ReportIf(FAILED(hr));
    }

    *pRetVal = psa;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE SumatraUIAutomationPageProvider::GetEmbeddedFragmentRoots(SAFEARRAY** pRetVal) {
    if (pRetVal == nullptr) {
        return E_POINTER;
    }

    // no other roots => return nullptr
    *pRetVal = nullptr;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE SumatraUIAutomationPageProvider::SetFocus() {
    return S_OK;
}

HRESULT STDMETHODCALLTYPE SumatraUIAutomationPageProvider::get_BoundingRectangle(struct UiaRect* pRetVal) {
    if (pRetVal == nullptr) {
        return E_POINTER;
    }

    if (released) {
        return E_FAIL;
    }

    // some engines might not support GetPageInfo
    const PageInfo* page = dm->GetPageInfo(pageNum);
    if (!page) {
        return E_FAIL;
    }

    Rect canvasRect = HwndWindowRect(canvasHwnd);

    pRetVal->left = canvasRect.x + page->pageOnScreen.x;
    pRetVal->top = canvasRect.y + page->pageOnScreen.y;
    pRetVal->width = page->pageOnScreen.dx;
    pRetVal->height = page->pageOnScreen.dy;

    return S_OK;
}

HRESULT STDMETHODCALLTYPE SumatraUIAutomationPageProvider::get_FragmentRoot(IRawElementProviderFragmentRoot** pRetVal) {
    if (released) {
        return E_FAIL;
    }

    // let our parent to handle this
    return root->get_FragmentRoot(pRetVal);
}

HRESULT STDMETHODCALLTYPE SumatraUIAutomationPageProvider::GetPatternProvider(PATTERNID patternId, IUnknown** pRetVal) {
    if (pRetVal == nullptr) {
        return E_POINTER;
    }

    if (patternId == UIA_ValuePatternId) {
        *pRetVal = static_cast<IValueProvider*>(this);
        AddRef();
        return S_OK;
    }

    *pRetVal = nullptr;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE SumatraUIAutomationPageProvider::GetPropertyValue(PROPERTYID propertyId, VARIANT* pRetVal) {
    if (pRetVal == nullptr) {
        return E_POINTER;
    }

    if (propertyId == UIA_NamePropertyId) {
        pRetVal->vt = VT_BSTR;
        TempStr s = fmt(_TRA("Page %d").s, pageNum);
        pRetVal->bstrVal = SysAllocString(CWStrTemp(s));
        return S_OK;
    } else if (propertyId == UIA_IsValuePatternAvailablePropertyId) {
        // VARIANT_TRUE (-1), not TRUE (1) - see DocumentProvider::GetPropertyValue
        pRetVal->vt = VT_BOOL;
        pRetVal->boolVal = VARIANT_TRUE;
        return S_OK;
    }

    pRetVal->vt = VT_EMPTY;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE
SumatraUIAutomationPageProvider::get_HostRawElementProvider(IRawElementProviderSimple** pRetVal) {
    if (pRetVal == nullptr) {
        return E_POINTER;
    }
    *pRetVal = nullptr;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE SumatraUIAutomationPageProvider::get_ProviderOptions(ProviderOptions* pRetVal) {
    if (pRetVal == nullptr) {
        return E_POINTER;
    }
    *pRetVal = ProviderOptions_ServerSideProvider;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE SumatraUIAutomationPageProvider::SetValue(__unused LPCWSTR val) {
    return E_ACCESSDENIED;
}

HRESULT STDMETHODCALLTYPE SumatraUIAutomationPageProvider::get_Value(BSTR* pRetVal) {
    if (pRetVal == nullptr) {
        return E_POINTER;
    }
    if (released) {
        return E_FAIL;
    }

    Str pageContent = dm->GetEngine()->GetTextForPage(pageNum);
    if (!pageContent) {
        *pRetVal = nullptr;
        return S_OK;
    }

    TempWStr pageContentW = ToWStrTemp(pageContent);
    *pRetVal = SysAllocString(pageContentW.s);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE SumatraUIAutomationPageProvider::get_IsReadOnly(BOOL* pRetVal) {
    if (pRetVal == nullptr) {
        return E_POINTER;
    }

    *pRetVal = TRUE;
    return S_OK;
}
