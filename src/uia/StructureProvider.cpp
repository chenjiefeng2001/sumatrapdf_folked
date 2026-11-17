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
#include "EngineAll.h"
#include "DisplayModel.h"
#include "uia/Constants.h"
#include "uia/DocumentProvider.h"
#include "uia/PageProvider.h"
#include "uia/Provider.h"
#include "uia/StructureProvider.h"

SumatraUIAutomationStructureProvider::SumatraUIAutomationStructureProvider(int pageNum, int spanIndex, HWND canvasHwnd,
                                                                           DisplayModel* dm,
                                                                           SumatraUIAutomationDocumentProvider* root,
                                                                           SumatraUIAutomationPageProvider* parentPage)
    : refCount(1),
      pageNum(pageNum),
      spanIndex(spanIndex),
      canvasHwnd(canvasHwnd),
      dm(dm),
      root(root),
      parentPage(parentPage),
      sibling_prev(nullptr),
      sibling_next(nullptr),
      released(false) {
    // don't add refs to our parent & owner, same as the page providers
}

SumatraUIAutomationStructureProvider::~SumatraUIAutomationStructureProvider() = default;

SumatraUIAutomationStructureProvider* SumatraUIAutomationStructureProvider::GetNext() {
    return sibling_next;
}

SumatraUIAutomationStructureProvider* SumatraUIAutomationStructureProvider::GetPrevious() {
    return sibling_prev;
}

// The span describing this element, and the page text it indexes into. Both are
// owned by the caller; use WithSpanAndText().
namespace {
struct SpanAndText {
    Vec<PageTextSpan> spans;
    PageText text;
};

// Loads what this element needs and hands it to `fn`. The span list owns the
// tag strings and the PageText owns the string and the glyph boxes, so both are
// released afterwards - copying the span would leave `type` dangling.
template <typename T>
void WithSpanAndText(SumatraUIAutomationStructureProvider* self, DisplayModel* dm, int pageNum, int spanIndex, T&& fn) {
    EngineBase* engine = dm ? dm->GetEngine() : nullptr;
    if (!engine || pageNum < 1 || pageNum > engine->PageCount() || spanIndex < 0) {
        return;
    }
    SpanAndText st;
    EngineMupdfGetPageStructure(engine, pageNum, &st.spans);
    if (spanIndex >= len(st.spans)) {
        for (PageTextSpan& span : st.spans) {
            str::Free(span.type);
        }
        st.spans.Reset();
        return;
    }
    st.text = engine->ExtractPageText(pageNum);
    fn(st.spans[spanIndex], st.text);
    FreePageText(&st.text);
    for (PageTextSpan& span : st.spans) {
        str::Free(span.type);
    }
    st.spans.Reset();
}
} // namespace

// The text of this structure element: the run of the page's extracted text the
// span covers. Empty when the page's text is not available (extraction is
// deferred until the page is visited), which is not an error - the element
// simply has no name until then.
TempStr SumatraUIAutomationStructureProvider::GetText() {
    if (released) {
        return {};
    }
    TempStr res;
    WithSpanAndText(this, dm, pageNum, spanIndex, [&res](PageTextSpan& span, PageText& text) {
        int nCodepoints = span.endCp - span.startCp;
        if (nCodepoints > 0 && text.text) {
            res = str::DupTemp(Utf8SliceByCodepoints(text.text, span.startCp, nCodepoints));
        }
    });
    return res;
}

HRESULT STDMETHODCALLTYPE SumatraUIAutomationStructureProvider::QueryInterface(REFIID riid, void** ppv) {
    static const QITAB qit[] = {QITABENT(SumatraUIAutomationStructureProvider, IRawElementProviderSimple),
                                QITABENT(SumatraUIAutomationStructureProvider, IRawElementProviderFragment),
                                {nullptr}};
    return QISearch(this, qit, riid, ppv);
}

ULONG STDMETHODCALLTYPE SumatraUIAutomationStructureProvider::AddRef() {
    return AtomicIntInc(&refCount);
}

ULONG STDMETHODCALLTYPE SumatraUIAutomationStructureProvider::Release() {
    LONG res = InterlockedDecrement(&refCount);
    ReportIf(res < 0);
    if (0 == res) {
        delete this;
    }
    return res;
}

HRESULT STDMETHODCALLTYPE SumatraUIAutomationStructureProvider::Navigate(enum NavigateDirection direction,
                                                                         IRawElementProviderFragment** pRetVal) {
    if (pRetVal == nullptr) {
        return E_POINTER;
    }
    if (released) {
        return E_FAIL;
    }
    *pRetVal = nullptr;
    if (direction == NavigateDirection_PreviousSibling) {
        *pRetVal = sibling_prev;
    } else if (direction == NavigateDirection_NextSibling) {
        *pRetVal = sibling_next;
    } else if (direction == NavigateDirection_Parent) {
        *pRetVal = parentPage;
    } else if (direction == NavigateDirection_FirstChild || direction == NavigateDirection_LastChild) {
        // a structure element is a leaf
    } else {
        return E_INVALIDARG;
    }
    if (*pRetVal) {
        (*pRetVal)->AddRef();
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE SumatraUIAutomationStructureProvider::GetRuntimeId(SAFEARRAY** pRetVal) {
    if (pRetVal == nullptr) {
        return E_POINTER;
    }
    SAFEARRAY* psa = SafeArrayCreateVector(VT_I4, 0, 3);
    if (!psa) {
        return E_OUTOFMEMORY;
    }
    // hwnd separates windows, then page, then which element on it, so every
    // element in the document has a unique runtime id
    int rId[] = {HandleToLong(canvasHwnd), pageNum, spanIndex};
    for (LONG i = 0; i < 3; i++) {
        HRESULT hr = SafeArrayPutElement(psa, &i, (void*)&(rId[i]));
        ReportIf(FAILED(hr));
    }
    *pRetVal = psa;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE SumatraUIAutomationStructureProvider::GetEmbeddedFragmentRoots(SAFEARRAY** pRetVal) {
    if (pRetVal == nullptr) {
        return E_POINTER;
    }
    *pRetVal = nullptr;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE SumatraUIAutomationStructureProvider::SetFocus() {
    return S_OK;
}

HRESULT STDMETHODCALLTYPE SumatraUIAutomationStructureProvider::get_BoundingRectangle(struct UiaRect* pRetVal) {
    if (pRetVal == nullptr) {
        return E_POINTER;
    }
    if (released) {
        return E_FAIL;
    }
    // the union of the element's glyph boxes, in screen coordinates: this is
    // what a screen reader highlights while reading, and what a braille client
    // uses to find the text
    bool ok = false;
    RectF bounds{};
    WithSpanAndText(this, dm, pageNum, spanIndex, [&](PageTextSpan& span, PageText& text) {
        for (int cp = span.startCp; cp < span.endCp && cp < text.nCodepoints; cp++) {
            RectF glyph = ToRectF(text.coords[cp]);
            if (glyph.dx <= 0 || glyph.dy <= 0) {
                continue; // soft-join space: no box of its own
            }
            bounds = ok ? bounds.Union(glyph) : glyph;
            ok = true;
        }
    });
    if (!ok) {
        return E_FAIL;
    }
    Rect screenRect = dm->CvtToScreen(pageNum, bounds);
    if (screenRect.IsEmpty()) {
        return E_FAIL;
    }
    Point tl = HwndClientToScreen(canvasHwnd, screenRect.TL());
    pRetVal->left = tl.x;
    pRetVal->top = tl.y;
    pRetVal->width = screenRect.dx;
    pRetVal->height = screenRect.dy;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE
SumatraUIAutomationStructureProvider::get_FragmentRoot(IRawElementProviderFragmentRoot** pRetVal) {
    if (released) {
        return E_FAIL;
    }
    return root->get_FragmentRoot(pRetVal);
}

HRESULT STDMETHODCALLTYPE SumatraUIAutomationStructureProvider::GetPatternProvider(PATTERNID patternId,
                                                                                   IUnknown** pRetVal) {
    if (pRetVal == nullptr) {
        return E_POINTER;
    }
    // no patterns of its own: the text is reached through the document's
    // TextPattern, which covers the whole page anyway
    *pRetVal = nullptr;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE SumatraUIAutomationStructureProvider::GetPropertyValue(PROPERTYID propertyId,
                                                                                 VARIANT* pRetVal) {
    if (pRetVal == nullptr) {
        return E_POINTER;
    }
    if (propertyId == UIA_NamePropertyId) {
        TempStr name = GetText();
        TempStr trimmed = str::TrimSuffixWhitespace(name);
        pRetVal->vt = VT_BSTR;
        pRetVal->bstrVal = SysAllocString(CWStrTemp(trimmed));
        return S_OK;
    }
    if (propertyId == UIA_ControlTypePropertyId) {
        pRetVal->vt = VT_I4;
        pRetVal->lVal = UIA_GroupControlTypeId;
        return S_OK;
    }
    if (propertyId == UIA_IsContentElementPropertyId || propertyId == UIA_IsControlElementPropertyId) {
        pRetVal->vt = VT_BOOL;
        pRetVal->boolVal = VARIANT_TRUE;
        return S_OK;
    }
    if (propertyId == UIA_HeadingLevelPropertyId) {
        // only a heading has a level; anything else reports "none" so a screen
        // reader's heading list does not list every paragraph
        int level = 0;
        WithSpanAndText(this, dm, pageNum, spanIndex,
                        [&level](PageTextSpan& span, PageText&) { level = span.headingLevel; });
        pRetVal->vt = VT_I4;
        pRetVal->lVal = level > 0 ? (UIA_HeadingLevel1 + level - 1) : UIA_HeadingLevel_None;
        return S_OK;
    }
    pRetVal->vt = VT_EMPTY;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE
SumatraUIAutomationStructureProvider::get_HostRawElementProvider(IRawElementProviderSimple** pRetVal) {
    if (pRetVal == nullptr) {
        return E_POINTER;
    }
    *pRetVal = nullptr;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE SumatraUIAutomationStructureProvider::get_ProviderOptions(ProviderOptions* pRetVal) {
    if (pRetVal == nullptr) {
        return E_POINTER;
    }
    *pRetVal = ProviderOptions_ServerSideProvider;
    return S_OK;
}
