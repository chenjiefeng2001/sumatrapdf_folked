# Editing annotations

**Available in version 3.3 or later.**

You can add / edit annotations in PDF files.

## Highlight text with `a`

Most common annotation is highlighting of text: select text and press `a`. This creates a highlight annotation in yellow color:

![Unsaved Annotation Changes](img/annot-unsaved-changes.png)

Here I highlighted word `USAGE` and pressed `a`.

## Saving annotations back to PDF file

Notice in toolbar (upper right) we show: You have unsaved annotations.

When you close the document (or exit the app) and have unsaved annotations, SumatraPDF will ask if you want to save them:

![Unsaved Annotations Dialog](img/unsaved-annotations-dialog.png)

If you close the dialog or choose `Discard`, annotations will be lost.

`Save changes to existing PDF` will over-write the PDF with newly added annotations.

`Save changes to a new PDF` will allow you to save as a new file.

## Open annotation editor

If you press `A` (upper case i.e. `Shift-a`) we will create a highlight annotation (same as lower case `a`) but also open annotation editor window.

Another way to open annotation editor is to use context menu (right mouse click or `Shift-F10`) and use `Edit Annotations`:

![Annotation Context Menu](img/annot-context-menu.png)

## Other annotations for selected text

When you selected a text, you can create the following annotations from the selection:

- highlight
- underline
- strike out
- squiggly underline

![Context Menu Annotation From Selection](img/context-menu-annot-from-sel.png)

We also copy selected text to a clipboard so that e.g. you can use `Ctrl-V` to paste it into `Contents` property of the annotation.

## Other annotation types

You can also create annotation object at your mouse location:

- text
- free text
- stamp
- caret
- image from the clipboard
- image from a file (`Insert Image...`)
- a file attachment (`File Attachment...`)
- a polygon, polyline or ink stroke (see below)

To attach a file to the document, choose **right-click → Create annotation under cursor → File Attachment...** (or run `CmdCreateAnnotFileAttachment` from the command palette) and pick a file. A paperclip is placed at the mouse position and the file's contents are embedded in the PDF, so the attachment travels with the document - anyone who opens it can save the file out again.

To put a picture of your signature (or any other image) on a PDF page:

- **File → Insert Image...**, or
- right-click the page → **Document → Insert Image...**, or
- right-click the page → **Create annotation under cursor → Image From File...**

Pick a PNG (or JPEG, etc.). The image is stamped on the page and you can drag or resize it. Save with **File → Save Annotations to existing PDF** (or save as a new PDF). This is an electronic signature image, not a cryptographic digital signature (`File → Sign Document...`, which uses a certificate from the Windows store or a `.pfx` / `.p12` file). **Sign Document** can also draw a PNG or JPEG inside the digital signature itself, and you can turn off the "Digitally signed by" labels and the other lines.

![Context Menu Annotation Under Cursor](img/context-menu-annot-under-cursor.png)

## Drawing a polygon, polyline or ink stroke

Polygon, Polyline and Ink are drawn rather than placed: their shape is the list
of points you drag out, so picking one of them from **Create annotation under
cursor** (or running `CmdCreateAnnotPolygon`, `CmdCreateAnnotPolyLine` or
`CmdCreateAnnotInk` from the command palette) arms a drawing mode instead of
dropping an annotation right away. The status bar then says to drag on the page.

- hold the left button and drag to draw the shape; the stroke is previewed as you drag
- `Esc` cancels
- a click without a drag creates nothing

Polygon is closed and drawn as an outline, Polyline is left open, and Ink is a
freehand stroke. Once created, each can be moved and resized like the other
annotations, and a whole stroke undoes in one step.

## Redacting content

To mark something for removal, select it and choose **right-click → Create annotation from selection → Redact**, or run `CmdCreateAnnotRedact` from the command palette.

A redaction mark on its own is **not** a redaction: it only draws a black box, and the text underneath is still in the file. To actually remove the content:

- **right-click → Apply Redactions...** (or `CmdPdfRedact`) in the app, then save the document (`Ctrl+Shift+S`), or
- from a command line, [`sumatrapdf-tool redact`](Tool-redact.md), which writes the result to a new file

This is **irreversible**. There is no undo, and the original file is not touched - but if you overwrite it with the redacted version, the content is gone for good.

## Annotation editor

All those commands will open annotation editor:

![Annotation Editor](img/annotation-editor.png)

Here we have a text annotation in the editor.

You can delete annotations, change color, text and other attributes.

## Select annotation from page in editor

When you have many annotations on the page, it's hard to locate the desired annotation in the editor list.

To select an annotation in the editor, place the mouse cursor over an annotation in the page, right click for context menu and use `Select Annotation in Editor`.

![Context Menu Select Annotation In Editor](img/context-menu-select-in-editor.png)

## Moving annotations

To move an annotation on the page, left click with mouse and drag to new location.

## Default colors, size, and opacity

Open **Settings → Advanced Options...** and edit the `Annotations` block:

| Setting                                     | Used for                                       |
| ------------------------------------------- | ---------------------------------------------- |
| `HighlightColor`                            | New highlights (`a`) — default `#ffff00`       |
| `UnderlineColor`                            | Underline annotations                          |
| `SquigglyColor`                             | Squiggly underline                             |
| `StrikeOutColor`                            | Strike-out                                     |
| `FreeTextColor` / `FreeTextBackgroundColor` | Free text annotations                          |
| `FreeTextSize`                              | Default font size for free text (default `12`) |
| `FreeTextBorderWidth`                       | Border width for free text                     |
| `FreeTextOpacity`                           | `0`–`100` percent opacity for free text        |
| `TextIconColor` / `TextIconType`            | Sticky-note style icons                        |
| `DefaultAuthor`                             | Author name written into new annotations       |

Highlight, underline, squiggly and strike-out colors accept `#aarrggbb` so you can set a default opacity: `#80ffff00` is a half-transparent yellow highlight. `#rrggbb` (6 digits) is fully opaque. You can still change one annotation's opacity in the annotation editor.

Free-text opacity is the separate `FreeTextOpacity` percent (0–100).

See [Advanced options / settings](Advanced-options-settings.md) for the full list.

### Keyboard shortcuts with custom colors

**Ver 3.6+:** `CmdCreateAnnotHighlight` accepts a color argument. You can bind e.g. green highlights to a key — see [Customize keyboard shortcuts](Customize-keyboard-shortcuts.md).

### Toolbar buttons

Add annotation commands to the toolbar via the `Shortcuts` array — see [Customize toolbar](Customize-toolbar.md). Example commands: `CmdCreateAnnotHighlight`, `CmdCreateAnnotUnderline`, `CmdSaveAnnotations`.

## Saving workflow

| Action                   | Shortcut / command                                                                                                     |
| ------------------------ | ---------------------------------------------------------------------------------------------------------------------- |
| Save annotations to file | `Ctrl + Shift + S` (`CmdSaveAnnotations`)                                                                              |
| Save to a new PDF        | `CmdSaveAnnotationsNewFile` (tab context menu or command palette)                                                      |
| Discard unsaved changes  | `CmdDiscardChanges` (tab context menu **Discard changes**, or `Ctrl + K` command palette) — reloads the file from disk |
| Save when closing        | Prompt dialog — choose existing file, new file, or discard                                                             |

Annotation edits can be undone with `Ctrl + Z` and redone with `Ctrl + Shift + Z` (`CmdUndo` / `CmdRedo`). This covers creating, editing, moving, resizing and deleting annotations, and filling in form fields. Undoing stops at the last save: saving the document starts a new history. Deleting an annotation also still works with `Delete` when the cursor is over it, or from the annotation editor.

To avoid the save prompt on every close, save explicitly with `Ctrl + Shift + S` before closing. To drop unsaved work without closing the tab, use **Discard Changes** (`CmdDiscardChanges`).

## Annotations from other programs

SumatraPDF can display most standard PDF annotations created in Acrobat, Foxit, etc. Some proprietary annotation types may show only an icon. Free-text presets and appearance vary between editors.

## Missing features

We don't yet support every annotation type or every editing operation other PDF apps offer.

The future will be driven by your feedback. If there are features missing or there are better ways of doing things, let me know in [Discussions](https://github.com/sumatrapdfreader/sumatrapdf/discussions)

When providing feedback:

- tell us what
- tell us why. Context is important for prioritizing features. Is new feature / idea something that you absolutely need or is it just a nice improvement
- you might be familiar how other PDF editors work. When referencing feature or UI ideas coming from other apps, tell us which app it is. Screenshots are better than words when describing UI ideas.
