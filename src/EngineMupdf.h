/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

struct Annotation;

struct FitzPageImageInfo {
    fz_rect rect = fz_unit_rect;
    fz_matrix transform;
    IPageElement* imageElement = nullptr;
    ~FitzPageImageInfo() { delete imageElement; }
};

struct FzPageInfo {
    int pageNo = 0; // 1-based
    fz_page* page = nullptr;

    // each containz fz_link for this page
    Vec<PageElementDestination*> links;
    // have to keep them alive because they are reverenced in links
    fz_link* retainedLinks = nullptr;

    Vec<Annotation*> annotations;
    // form fields (widgets). kept separate from annotations so they are
    // hit-testable for form filling without polluting the annotation list
    // (comments, edit-annotations panel) with form fields.
    Vec<Annotation*> widgets;
    // annotations + widgets are loaded together on first access; this guards
    // that (annotations.Size()==0 can't, since a page may have only widgets).
    bool annotsLoaded = false;
    // auto-detected links
    Vec<IPageElement*> autoLinks;
    // comments are made out of annotations
    Vec<IPageElement*> comments;

    Vec<IPageElement*> allElements;
    bool elementsNeedRebuilding = true;

    RectF mediabox{};
    Vec<FitzPageImageInfo*> images;

    // if false, only loaded page (fast: page ptr + annotations/minimal metadata)
    // if true, loaded expensive info (extracted text etc.)
    bool fullyLoaded = false;
    // true once text extraction has been performed (lazy, after fullyLoaded)
    // Extracting text is deferred outside pagesLock+renderLock to avoid
    // blocking the UI thread for hundreds of milliseconds.
    bool textExtracted = false;
    // cached stext page for reuse; owned by this FzPageInfo, freed in ~FzPageInfo
    fz_stext_page* stextPage = nullptr;

    // cached "View" rendering of the page; built lazily under
    // EngineMupdf::renderLock. fz_display_list is safe to *replay* across
    // cloned contexts in principle, but the image objects it references are
    // not -- shared images (notably JBIG2 with shared dictionaries) trigger
    // races inside mupdf's image store on concurrent decode. So renderLock
    // is engine-wide, not per-page.
    fz_display_list* displayList = nullptr;

    // generation counters for page versioning: every annotation modification
    // increments annotGeneration; the render thread compares
    // displayListGeneration to detect stale display lists and rebuilds.
    // Under pagesLock for increment, under renderLock for the check/rebuild.
    int annotGeneration = 0;        // bumped on each annotation change
    int displayListGeneration = -1; // generation captured when displayList was built
};

class EngineMupdf : public EngineBase {
  public:
    EngineMupdf();
    ~EngineMupdf() override;
    EngineBase* Clone() override;

    RectF PageMediabox(int pageNo) override;
    RectF PageContentBox(int pageNo, RenderTarget target = RenderTarget::View) override;

    Pixmap* RenderPage(RenderPageArgs& args) override;

    RectF Transform(const RectF& rect, int pageNo, float zoom, int rotation, bool inverse = false) override;

    Str GetFileData() override;
    bool SaveFileAs(Str copyFileName) override;
    PageText ExtractPageText(int pageNo) override;
    void ReleaseTextExtractionThreadContext() override;

    bool HasClipOptimizations(int pageNo) override;
    TempStr GetPropertyTemp(Str name) override;
    void GetProperties(StrVec& keyValOut) override;

    bool BenchLoadPage(int pageNo) override;

    Vec<IPageElement*> GetElements(int pageNo) override;
    IPageElement* GetElementAtPos(int pageNo, PointF pt) override;
    bool HandleLink(IPageDestination*, ILinkHandler*) override;

    RenderedBitmap* GetImageForPageElement(IPageElement*) override;

    IPageDestination* GetNamedDest(Str name) override;
    TocTree* GetToc() override;

    TempStr GetPageLabeTemp(int pageNo) const override;
    int GetPageByLabel(Str label) const override;

    fz_context* Ctx() const;

    // The base context the document and its MuJS engine are bound to (JS is
    // enabled on _ctx, so doc->js->ctx == _ctx). Operations that can run form
    // JavaScript -- i.e. field-value changes that regenerate widget appearances
    // -- MUST use this, NOT a per-thread Ctx() clone. pdf_js_execute() always
    // runs JS (and rethrows JS errors) on doc->js->ctx == _ctx; if the enclosing
    // fz_try frames live on a clone instead, a JS error rethrows on _ctx with no
    // matching handler and hits mupdf's uncaught-error abort. Safe to use from
    // the UI thread under docLock (nothing else drives _ctx's error stack).
    fz_context* BaseCtx() const { return _ctx; }

    // Lock hierarchy (acquire in this order; never go upward):
    //   pagesLock           - protects the pages[] vector / FzPageInfo lookup
    //   renderLock          - serializes any mupdf call that may run a page
    //                         or replay a display list, i.e. anything that
    //                         can decode an image. Engine-wide (not per-page)
    //                         because shared image objects (e.g. JBIG2 with
    //                         shared dictionaries) race in mupdf's image
    //                         store under concurrent decode -- crashes
    //                         in template_image_compose_opt with use-after-
    //                         free on the source pixmap. Also acquired under
    //                         pagesLock inside GetFzPageInfo.
    //   docLock             - Slim Reader/Writer Lock for document-scope
    //                         mupdf operations: outline, fonts, info, named
    //                         dests, page-tree access, annotation mutations.
    //                         * Shared lock:  render path (Build + Replay),
    //                           annotation read-only queries (GetBounds,
    //                           GetContents, etc.)
    //                         * Exclusive lock: annotation mutations,
    //                           form-field writes, document save.
    //                         Independent of renderLock; never acquire
    //                         pagesLock while holding docLock.
    //                         SRWLock is NOT recursive — a thread holding
    //                         shared lock must not acquire exclusive, and
    //                         vice versa.
    //
    // docLock must NOT alias one of fz_locks[] -- mupdf takes those briefly
    // for its own internal coordination, and reusing one as a long-held outer
    // lock would serialize every cloned-context allocation across all threads.
    CRITICAL_SECTION pagesLock;
    CRITICAL_SECTION renderLock;
    SRWLOCK docLock;

    // per-FZ_LOCK-index critical sections used by mupdf via fz_locks_ctx
    // callbacks. Mupdf holds these only momentarily; do not hold them across
    // your own code.
    CRITICAL_SECTION fz_locks[FZ_LOCK_MAX];

    fz_context* _ctx = nullptr;
    fz_locks_context fz_locks_ctx;
    int displayDPI{96};
    fz_document* _doc = nullptr;
    pdf_document* pdfdoc = nullptr;
    Vec<FzPageInfo*> pages;
    fz_outline* outline = nullptr;
    fz_outline* attachments = nullptr;
    pdf_obj* pdfInfo = nullptr;
    StrVec* pageLabels = nullptr;

    TocTree* tocTree = nullptr;

    // password used to decrypt the document (needed for re-encryption/decryption)
    TempStr pdfPassword;

    // used to track "dirty" state of annotations. not perfect because if we add and delete
    // the same annotation, we should be back to 0
    bool modifiedAnnotations = false;

    bool Load(Str filePath, PasswordUI* pwdUI = nullptr);
    bool Load(IStream* stream, Str nameHint, PasswordUI* pwdUI = nullptr);
    // TODO(port): fz_stream can no-longer be re-opened (fz_clone_stream)
    // bool Load(fz_stream* stm, PasswordUI* pwdUI = nullptr);
    bool LoadFromStream(fz_stream* stm, Str nameHing, PasswordUI* pwdUI = nullptr);
    bool FinishLoading();
    RenderedBitmap* GetPageImage(int pageNo, RectF rect, int imageIdx);

    FzPageInfo* GetFzPageInfoCanFail(int pageNo);
    FzPageInfo* GetFzPageInfoFast(int pageNo);
    FzPageInfo* GetFzPageInfo(int pageNo, bool loadQuick, fz_cookie* cookie = nullptr);
    fz_matrix viewctm(int pageNo, float zoom, int rotation);
    fz_matrix viewctm(fz_page* page, float zoom, int rotation) const;
    TocItem* BuildTocTree(TocItem* parent, fz_outline* outline, int& idCounter, bool isAttachment);
    TempStr ExtractFontListTemp();

    Str LoadStreamFromPDFFile(Str filePath);
};

EngineMupdf* AsEngineMupdf(EngineBase* engine);

fz_rect ToFzRect(RectF rect);
RectF ToRectF(fz_rect rect);
void MarkNotificationAsModified(EngineMupdf*, Annotation*, AnnotationChange = AnnotationChange::Modify);
// Public wrapper that acquires docLock Exclusive internally (for callers that
// don't already hold docLock). Prefer MakeAnnotationWrapperLocked when already
// holding docLock (e.g. inside GetFzPageInfo's batch load path).
Annotation* MakeAnnotationWrapper(EngineMupdf* engine, pdf_annot* annot, int pageNo);
// No-lock variant: caller MUST hold docLock (Shared or Exclusive) before
// calling.  Used inside GetFzPageInfo so docLock can be acquired at the
// pagesLock→docLock→renderLock outer level instead of nested inside renderLock.
Annotation* MakeAnnotationWrapperLocked(EngineMupdf* engine, pdf_annot* annot, int pageNo);
// Deferred text extraction for a page.  Call WITHOUT holding pagesLock or
// renderLock; only docLock [Shared] is acquired internally.  Sets
// pageInfo->stextPage and pageInfo->textExtracted on success.
void ExtractTextLazy(EngineMupdf* engine, FzPageInfo* pageInfo, fz_cookie* cookie = nullptr);
