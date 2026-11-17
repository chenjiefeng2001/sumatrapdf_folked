/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

struct MainWindow;
struct WindowTab;

// True when the open PDF declares layers (optional content groups) with
// something the user can toggle. Used to decide whether to offer the command.
bool PdfLayersAvailable(MainWindow* win);

void ShowPdfLayersDialog(MainWindow* win);

// closes the layers window if it was showing this tab
void PdfLayersDialogOnTabClosed(WindowTab* tab);