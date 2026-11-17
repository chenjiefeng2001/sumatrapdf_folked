# sumatrapdf-tool redact

or [SumatraPDF.exe redact](Tools.md)

**Available in [pre-release 3.7](https://www.sumatrapdfreader.org/prerelease)**

**Usage:** `sumatrapdf-tool redact [options] input.pdf output.pdf`

Removes, permanently, the content covered by the document's redaction annotations (`/Subtype /Redact`) and then deletes the annotations themselves.

This is what makes a redaction a redaction rather than a black rectangle. A redaction annotation on its own only draws a mark: the text or image underneath is still in the file and can be read by anyone. Only after applying it does the content actually leave the document.

Because that cannot be undone, the result always goes to a **separate output file**; the input is never modified. In the GUI, SumatraPDF applies redactions to the open document and asks you to save afterwards (`Ctrl+Shift+S`), which likewise leaves the original file alone.

The output is a full rewrite with garbage collection. That is not an optimisation choice: mupdf refuses to save a redacted document incrementally, and without garbage collection the removed objects would still be in the file - which would defeat the point.

## Examples

Redact everything the document marks:

```
sumatrapdf-tool redact input.pdf output.pdf
```

Only pages 1-3 and 7:

```
sumatrapdf-tool redact -pages 1-3,7 input.pdf output.pdf
```

Remove images that a mark touches at all, instead of blanking the intruding pixels:

```
sumatrapdf-tool redact -image remove input.pdf output.pdf
```

Remove vector graphics that a mark touches at all, not just those it fully covers:

```
sumatrapdf-tool redact -lineart touched input.pdf output.pdf
```

Leave overlapping text alone (**not safe** - the text stays readable under the mark):

```
sumatrapdf-tool redact -text none input.pdf output.pdf
```

## All options

```
Usage: sumatrapdf-tool redact [options] <input.pdf> <output.pdf>

Applies every redaction annotation in the document: the content they
cover is removed and the annotations themselves are deleted. This is
irreversible, so the result always goes to a separate output file.

options:
  -text <remove|none|invisible>  how to treat overlapping text
        (default: remove; 'none' leaves text readable under the mark)
  -image <none|remove|pixels|remove-invisible>
        (default: pixels)
  -lineart <none|covered|touched>
        (default: covered)
  -no-black-boxes  don't draw a black box over each redaction
  -pages <spec>     only these pages, e.g. '1-3,7' (default: all)
```

### What each option means

- `-text remove` (default) drops any text whose glyphs overlap a mark. This is the only setting that is actually safe.
- `-text invisible` additionally removes text that was hidden with a render mode (e.g. OCR text under a scanned image), but _not_ white-on-white text.
- `-image pixels` (default) replaces the intruding pixels with black, which keeps the rest of the image. It is not a true redaction of an image, because compression artifacts can leave traces; use `remove` when the image must not survive.
- `-lineart covered` (default) removes vector graphics that a mark fully covers. `touched` also removes graphics the mark merely clips, `none` leaves them (they are then only hidden by the black box).
- `-no-black-boxes` omits the drawn rectangle. Useful when you want to see what was actually removed.

If the document has no redaction annotations, `redact` writes nothing and exits with a non-zero status, so it is safe to run in a batch script.
