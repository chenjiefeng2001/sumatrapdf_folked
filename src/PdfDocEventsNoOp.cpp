/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

// Stand-in for PdfDocEvents.cpp in the UI-less builds (Windows Search filter,
// Explorer preview handler, engine unit tests). Those link EngineMupdf.cpp but
// have no MainWindow, no command table and no notifications, so a document's
// JavaScript asking to print or open a URL has nowhere to go.

#include "base/Base.h"

#include "gui/UIModels.h"
#include "EngineBase.h"
#include "PdfDocEvents.h"

void PdfInstallDocEventCallback(EngineBase* engine) {
    (void)engine;
}

static void NoOpDocAction(EngineBase* engine) {
    (void)engine;
}

void PdfDocEventWillPrint(EngineBase* engine) {
    NoOpDocAction(engine);
}

void PdfDocEventDidPrint(EngineBase* engine) {
    NoOpDocAction(engine);
}

void PdfDocEventWillSave(EngineBase* engine) {
    NoOpDocAction(engine);
}

void PdfDocEventDidSave(EngineBase* engine) {
    NoOpDocAction(engine);
}

void PdfDocEventWillClose(EngineBase* engine) {
    NoOpDocAction(engine);
}