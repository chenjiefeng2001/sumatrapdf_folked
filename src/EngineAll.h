/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

enum class FileType : u8;
struct Annotation;
enum class AnnotationType;
struct PasswordUI;
struct FileArgs;
struct AnnotCreateArgs;
struct PropValue;

bool IsEngineDjVuSupportedFileType(FileType kind);
EngineBase* CreateEngineDjvuDecFromFile(Str path);
EngineBase* CreateEngineDjvuDecFromData(Str data);
extern bool gMemoryMapLargeFiles;

EngineBase* CreateEngineEpubFromFile(Str fileName);
EngineBase* CreateEngineEpubFromData(Str data);
EngineBase* CreateEngineFb2FromFile(Str fileName);
EngineBase* CreateEngineFb2FromData(Str data);
EngineBase* CreateEngineMobiFromFile(Str fileName);
EngineBase* CreateEngineMobiFromData(Str data);
Str ExtractPdfFromPrintReplicaFile(Str path);
Str ExtractPdfFromPrintReplicaData(Str data);
EngineBase* CreateEnginePdbFromFile(Str fileName);
EngineBase* CreateEngineChmFromFile(Str fileName);
EngineBase* CreateEngineHtmlFromFile(Str fileName);
EngineBase* CreateEngineTxtFromFile(Str fileName);

void SetDefaultEbookFont(Str name, float size);
void SetDefaultChmFont(Str name);
// Reject characters that would break out of a quoted CSS font-family value.
inline bool IsSafeEbookFontName(Str name) {
    if (!name) {
        return false;
    }
    for (int i = 0; i < len(name); i++) {
        unsigned char c = (unsigned char)name.s[i];
        if (c < 32 || c == '"' || c == '\'' || c == ';' || c == '{' || c == '}' || c == '\\') {
            return false;
        }
    }
    return true;
}
// Drop one matching pair of wrapping ' or " quotes, then check IsSafeEbookFontName.
inline Str EbookFontNameFromSetting(Str name) {
    int n = len(name);
    if (n >= 2) {
        char q = name.s[0];
        if ((q == '"' || q == '\'') && name.s[n - 1] == q) {
            name = Str(name.s + 1, n - 2);
        }
    }
    if (!IsSafeEbookFontName(name)) {
        return {};
    }
    return name;
}
void EngineEbookCleanup();

// the ebook font this reflowable document asked for and we couldn't load, so
// the text came out in the default font; null if that didn't happen (#4600)
Str EngineEbookFontUnavailable(EngineBase* engine);
bool EngineMupdfWasRepaired(EngineBase* engine);

// the user CSS generated from ebook settings (font family + line height), so
// the Ebook Settings dialog can show exactly what the engine will apply
TempStr EbookGeneratedCssTemp(Str fontName, const Vec<float>* margin, float lineSpacing, int displayDPI);

/* EngineImages.cpp */

// how many leading bytes of an image file are enough, in practice, to parse the
// image size out of its header (JPEG SOF can sit past EXIF / ICC segments)
constexpr int kImageSizeFromDataPartialSize = 64 * 1024;

bool IsEngineImageSupportedFileType(FileType);
EngineBase* CreateEngineImageFromFile(Str fileName);
EngineBase* CreateEngineImageFromData(Str data);

bool IsEngineImageDirSupportedFile(Str fileName, bool sniff = false);
EngineBase* CreateEngineImageDirFromFile(Str fileName);

bool IsEngineCbxSupportedFileType(FileType kind);
EngineBase* CreateEngineCbxFromFile(Str path, PasswordUI* pwdUI = nullptr, FileType hintType = FileType::Unknown,
                                    Str realPath = {});
EngineBase* CreateEngineCbxFromData(Str data);

bool IsEngineImages(EngineBase*);
void EngineImagesGetImageProperties(EngineBase*, int pageNo, Vec<PropValue>& propsOut);
bool EngineImagesGetPageFileInfo(EngineBase*, int pageNo, TempStr* nameOut, i64* sizeOut);
// Non-owning raw image bytes for a page (valid until engine is destroyed). Empty if
// not an image-collection engine or the page has no extractable file data.
Str EngineImagesGetImageData(EngineBase*, int pageNo);

/* EngineMupdf.cpp */

using ShowErrorCb = Func1<Str>;

bool IsEngineMupdfSupportedFileType(FileType);
EngineBase* CreateEngineMupdfFromFile(Str path, FileType kind, int displayDPI, PasswordUI* pwdUI = nullptr);
EngineBase* CreateEngineMupdfFromData(Str data, Str nameHint, PasswordUI* pwdUI);
Str LoadEmbeddedPDFFile(Str path);
TempStr ParseEmbeddedStreamNumber(Str path, int* streamNoOut);
Annotation* EngineMupdfCreateAnnotation(EngineBase*, int pageNo, PointF pos, AnnotCreateArgs* args);
void EngineMupdfGetAnnotations(EngineBase*, Vec<Annotation*>&);
void EngineMupdfGetLoadedAnnotations(EngineBase*, Vec<Annotation*>&);
bool EngineMupdfTryGetLoadedAnnotations(EngineBase*, Vec<Annotation*>&);
void EngineMupdfStartLoadAllAnnotations(EngineBase*, const Vec<int>& firstPages, const Func0& onProgress);
void EngineMupdfCancelLoadAllAnnotations(EngineBase*);
bool EngineMupdfHasUnsavedAnnotations(EngineBase*);
bool EngineMupdfSupportsAnnotations(EngineBase*);
bool EngineMupdfIsPdf(EngineBase* engine);
bool EngineMupdfIsEncrypted(EngineBase* engine);
bool EngineMupdfHeadingTocPending(EngineBase* engine);
void EngineMupdfStartHeadingToc(EngineBase* engine, const Func0& onDone);
void EngineMupdfCancelHeadingToc(EngineBase* engine);
Str EngineMupdfGetPassword(EngineBase* engine);
bool EngineMupdfSaveUpdated(EngineBase* engine, Str path, const ShowErrorCb& showErrorFunc);
bool EngineMupdfSaveCopy(EngineBase* engine, Str path);

// How to destroy content under a redaction annotation. Mirrors mupdf's
// pdf_redact_options; the defaults here are stricter than mupdf's own, which
// leaves vector graphics alone unless asked.
struct PdfRedactOptions {
    bool drawBlackBoxes = true;
    // 0 = leave images, 1 = remove an image that intrudes at all, 2 = black out
    // the intruding pixels, 3 = remove unless the intrusion is invisible
    int imageMethod = 2;
    // 0 = leave vector art, 1 = remove art that is fully covered, 2 = remove art
    // that is touched at all
    int lineArtMethod = 1;
    // 0 = remove overlapping text (the safe choice), 1 = leave text alone
    // (insecure), 2 = only remove text hidden via render mode
    int textMethod = 0;
};

// Applies the redaction annotations on `pageNo` (or on every page when
// pageNo < 0), permanently removing the content they cover. Irreversible:
// saves the document as a full rewrite with garbage collection.
bool EngineMupdfApplyRedactions(EngineBase*, const PdfRedactOptions&, int pageNo);

// Undo/redo of annotation and form edits, via mupdf's journal. Only PDF
// documents have them, and only after the first edit (the journal is enabled
// lazily - see PdfJournal.h).
bool EngineMupdfCanUndo(EngineBase*);
bool EngineMupdfCanRedo(EngineBase*);
bool EngineMupdfUndo(EngineBase*);
bool EngineMupdfRedo(EngineBase*);

// True when the document carries document-level JavaScript that runs on load
// (and only when JavaScript is enabled).
bool EngineMupdfHasDocumentJavaScript(EngineBase*);

// PDF optional content (OCG) layers, as the document's own layer panel lists
// them. Layer visibility is view state, not a document edit: nothing here marks
// the document dirty or records an undo step.
struct PdfLayerUiItem {
    Str text;
    int depth = 0;
    // 0 = a plain label (not interactive), 1 = checkbox, 2 = radiobox
    int type = 0;
    bool selected = false;
    bool locked = false;
};

int EngineMupdfGetLayerUiCount(EngineBase*);
bool EngineMupdfGetLayerUiItem(EngineBase*, int idx, PdfLayerUiItem& out);
bool EngineMupdfSetLayerUiSelected(EngineBase*, int idx, bool selected);
// go back to the configuration the document was authored with
void EngineMupdfResetLayerUi(EngineBase*);

// digitally signing a PDF (SignDocumentDialog.cpp drives this)
// appearance flag bits match mupdf's PDF_SIGNATURE_SHOW_* (logo is never used)
constexpr int kPdfSignShowLabels = 1;
constexpr int kPdfSignShowDN = 2;
constexpr int kPdfSignShowDate = 4;
constexpr int kPdfSignShowTextName = 8;
constexpr int kPdfSignShowGraphicName = 16;
constexpr int kPdfSignDefaultAppearance =
    kPdfSignShowLabels | kPdfSignShowDN | kPdfSignShowDate | kPdfSignShowTextName | kPdfSignShowGraphicName;

struct PdfSignArgs {
    Str certPath;             // .pfx / .p12 holding the certificate + private key
    Str certPassword;         // password protecting it, may be empty
    Str certThumbprint;       // SHA-1 of a CurrentUser\MY cert; if set, used instead of certPath
    Str reason;               // optional, shown in the signature
    Str location;             // optional, shown in the signature
    Str fieldName;            // existing unsigned signature field to fill in
    int pageNo = 1;           // page for a new field, when fieldName is empty
    RectF rect;               // where the new field goes; empty uses mupdf's default box
    int appearanceFlags = -1; // -1 = kPdfSignDefaultAppearance
    Str imagePath;            // optional PNG/JPEG drawn on the left of the appearance
};

#if OS_WIN
void EngineMupdfGetUnsignedSignatureFields(EngineBase*, StrVec& names, Vec<int>& pageNos);
bool IsUnsignedSignatureWidget(Annotation*, TempStr* fieldNameOut);
bool EngineMupdfSignDocument(EngineBase*, const PdfSignArgs&, Str* errOut);
void ListWindowsSigningCertificates(StrVec& thumbprints, StrVec& labels);
void SetEutlLookupFn(bool (*fn)(const u8* der, int derLen));
struct PdfSigCert {
    Str label;
    Str der;
};
void EngineMupdfGetSignatureCerts(EngineBase*, Vec<PdfSigCert>& out);
void FreePdfSigCerts(Vec<PdfSigCert>&);
#endif
Annotation* EngineMupdfGetAnnotationAtPos(EngineBase*, int pageNo, PointF pos, Annotation*);
Annotation* EngineMupdfGetWidgetAtPos(EngineBase*, int pageNo, PointF pos);
Annotation* EngineMupdfGetAdjacentWidget(EngineBase*, Annotation* cur, bool forward);
void EngineMupdfGetFormFieldHighlightRects(EngineBase*, int pageNo, Annotation* skip, Vec<RectF>& out);
void EngineMupdfSetDisableJavaScript(bool disable);
float EngineMupdfSetEbookLayoutAspect(float dyOverDx);
void EngineMupdfSetAllowExternalImages(bool allow);
void EngineMupdfToggleCadEnhance(EngineBase* engine);
bool EngineMupdfCadEnhanceActive(EngineBase* engine);
void EngineMupdfInvalidateDarkMode(EngineBase* engine);
bool EngineSupportsSmartDarkMode(EngineBase* engine);
Str EngineMupdfLoadAttachment(EngineBase*, int attachmentNo);
Str EngineMupdfLoadAnnotAttachment(EngineBase*, int objNum);
// the file is a PDF portfolio (/Root/Collection): its embedded files are the
// content, listed in the order the document asks for
bool EngineMupdfIsPortfolio(EngineBase*);
// the tagged-PDF structure of a page, as runs of its extracted text. Only
// headings are reported today; empty for an untagged page.
void EngineMupdfGetPageStructure(EngineBase* engine, int pageNo, Vec<PageTextSpan>* out);
TempStr EngineMupdfGetPdfInfo(Str path);
TempStr EngineMupdfGetPdfOutline(Str path);

bool IsEnginePsAvailable();
bool IsEnginePsSupportedFileType(FileType);
EngineBase* CreateEnginePsFromFile(Str fileName);

bool IsSupportedFileType(FileType kind, bool enableEngineEbooks);

EngineBase* CreateEngineFromFile(Str filePath, PasswordUI* pwdUI, bool enableChmEngine);

bool IsOpenCachePath(Str path);
TempStr MaybeCopyEphemeralHostFile(Str path);

bool EngineSupportsAnnotations(EngineBase*);
bool EngineGetAnnotations(EngineBase*, Vec<Annotation*>&);
bool EngineHasUnsavedAnnotations(EngineBase*);
Annotation* EngineGetAnnotationAtPos(EngineBase*, int pageNo, PointF pos, Annotation*);
Annotation* EngineGetWidgetAtPos(EngineBase*, int pageNo, PointF pos);
