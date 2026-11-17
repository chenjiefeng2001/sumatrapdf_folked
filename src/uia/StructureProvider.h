/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// One structural element of a tagged PDF, exposed to screen readers.
//
// A tagged PDF says what its text *is*: this run is a heading, that one is a
// table cell, the other is a list item. Without that, a screen reader reads the
// page as one undifferentiated stream and its heading navigation (the H key in
// NVDA, the heading list in Narrator) finds nothing. This provider turns each
// such run into its own element, hanging off the page, carrying the heading
// level when it is a heading.
//
// The run itself is addressed as codepoint offsets into the page's extracted
// text, which is exactly what SumatraUIAutomationTextRange speaks, so the
// element's name and bounding box come from the same source as the text.

struct DisplayModel;
struct PageTextSpan;
class SumatraUIAutomationDocumentProvider;
class SumatraUIAutomationStructureProvider : public IRawElementProviderFragment, public IRawElementProviderSimple {
    AtomicInt refCount;
    int pageNum;
    int spanIndex;
    HWND canvasHwnd;
    DisplayModel* dm;
    SumatraUIAutomationDocumentProvider* root;
    SumatraUIAutomationPageProvider* parentPage;
    SumatraUIAutomationStructureProvider* sibling_prev;
    SumatraUIAutomationStructureProvider* sibling_next;

    bool released;

    // the page provider links its structure elements together; only it (and
    // this class, to reach the page back) may touch those links
    friend class SumatraUIAutomationPageProvider;

  public:
    SumatraUIAutomationStructureProvider(int pageNum, int spanIndex, HWND canvasHwnd, DisplayModel* dm,
                                         SumatraUIAutomationDocumentProvider* root,
                                         SumatraUIAutomationPageProvider* parentPage);
    ~SumatraUIAutomationStructureProvider();

    SumatraUIAutomationStructureProvider* GetNext();
    SumatraUIAutomationStructureProvider* GetPrevious();

    // the text of this structure element, as it appears in the page's text
    TempStr GetText();

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(const IID&, void**);
    ULONG STDMETHODCALLTYPE AddRef();
    ULONG STDMETHODCALLTYPE Release();

    // IRawElementProviderFragment
    HRESULT STDMETHODCALLTYPE Navigate(enum NavigateDirection direction, IRawElementProviderFragment** pRetVal);
    HRESULT STDMETHODCALLTYPE GetRuntimeId(SAFEARRAY** pRetVal);
    HRESULT STDMETHODCALLTYPE GetEmbeddedFragmentRoots(SAFEARRAY** pRetVal);
    HRESULT STDMETHODCALLTYPE SetFocus();
    HRESULT STDMETHODCALLTYPE get_BoundingRectangle(struct UiaRect* pRetVal);
    HRESULT STDMETHODCALLTYPE get_FragmentRoot(IRawElementProviderFragmentRoot** pRetVal);

    // IRawElementProviderSimple
    HRESULT STDMETHODCALLTYPE GetPatternProvider(PATTERNID patternId, IUnknown** pRetVal);
    HRESULT STDMETHODCALLTYPE GetPropertyValue(PROPERTYID propertyId, VARIANT* pRetVal);
    HRESULT STDMETHODCALLTYPE get_HostRawElementProvider(IRawElementProviderSimple** pRetVal);
    HRESULT STDMETHODCALLTYPE get_ProviderOptions(ProviderOptions* pRetVal);
};
