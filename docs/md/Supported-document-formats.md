# Supported document formats

SumatraPDF reader supports the following document types:

- PDF (.pdf, .ai)

  Interactive forms (AcroForm) can be filled in. XFA ("dynamic") forms are not rendered: a hybrid XFA form, which carries both an XFA and an AcroForm layer, is filled in on its AcroForm layer and reported as **XFA Forms: using AcroForm layer** in document properties; a pure XFA form has no AcroForm layer at all, so it opens read-only and SumatraPDF reports **XFA not supported**.

  For a tagged (structured) PDF, text is returned in reading order - the order of the document's structure tree - so copying, extracting, reading aloud and searching follow how the document is meant to be read rather than the order its page draws the text in. Document Properties reports the state of the structure tree (**Structure Tree:** no problems found / problems repaired / broken / contains a cycle / has attributes). Optional content groups (layers) a PDF declares can be turned on and off from **View → PDF Layers...**; that is view state and is not saved into the file.

  A **PDF portfolio** (a PDF whose catalog has `/Collection`, i.e. a bundle of files rather than a document to read) lists its embedded files in the sidebar, which is captioned **Attachments** instead of **Bookmarks**. The files appear in the order the document asks for, each shows the description the document gives it on hover, and clicking one opens it. Right-click still offers **Save Attachment...**.

- eBook formats:
  - unencrypted EPUB (.epub)
  - MOBI / Kindle (.mobi, and un-encrypted .azw, .azw3, .prc); Print Replica (.azw4) opens as the wrapped PDF
  - FictionBook (.fb2, .fb2z, .fbz, .zfb2, .fb2.zip)
  - Palm DOC (.pdb)
  - plain text (.txt, .log, .nfo, .tcr, …)
- Markdown (.md, .markdown), rendered as GitHub Flavored Markdown
- comic book files: .cbz, .cbr, .cbt, .cb7 (and .ora) — see [Comics and manga](Comics-and-manga.md)
- archive files (.zip, .rar, .7z, .tar) containing images
- DjVu (.djvu, .djv)
- Microsoft Compiled HTML Help (.chm)
- XPS (.xps, .oxps, .xod, .dwfx)
- SVG (.svg)
- images:
  - PNG (.png)
  - JPEG (.jpg, .jpeg, .jfif)
  - GIF (.gif), including animations
  - TIFF (.tif, .tiff), including multi-page
  - BMP (.bmp, .dib)
  - TGA (.tga)
  - WebP (.webp) — also images inside EPUB, MOBI and HTML
  - JPEG XR (.jxr, .hdp, .wdp)
  - JPEG 2000 (.jp2, .j2k, .jpx, .jpf, .jpm, .j2c)
  - AVIF (.avif)
  - JPEG XL (.jxl)
  - HEIF / HEIC (.heic, .heif) — built-in decoder; see below

If [Ghostscript](https://ghostscript.com/) is installed, we support PostScript (.ps, .eps) and PJL (Printer Job Language) files.

## HEIF / HEIC support

SumatraPDF can open [HEIF images](https://nokiatech.github.io/heif/) (.heic, .heif) and AVIF (.avif) with a built-in decoder:

- **HEIC** (typical phone stills, HEVC-coded): decoded in-process; no Windows codec required for most files
- **AVIF** (AV1-coded): decoded in-process via [dav1d](https://code.videolan.org/videolan/dav1d)
- Grids, overlays, and common transforms (crop / rotate / mirror) are handled for still images

If built-in decode fails, on Windows we still try the system [WIC](https://learn.microsoft.com/en-us/windows/win32/wic/-wic-about-windows-imaging-codec) path (for example if a third-party HEIF codec is installed). That fallback is optional for typical HEIC/AVIF files.

You can make SumatraPDF the [default program for handling those file types](Set-as-default-pdf-viewer.md).
