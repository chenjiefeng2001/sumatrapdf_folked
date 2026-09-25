/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "base/Base.h"
#include "base/CmdLineArgsIter.h"
#include "gui/Dpi.h"
#include "base/File.h"
#include "base/Win.h"
#include "base/UITask.h"

#include "gui/UIModels.h"
#include "gui/Layout.h"
#include "gui/PlatformFont.h"
#include "gui/win/WinGui.h"
#include "gui/win/WebView.h"

#include "Settings.h"
#include "DocController.h"
#include "EngineBase.h"
#include "GlobalPrefs.h"
#include "AppSettings.h"
#include "MainWindow.h"
#include "WindowTab.h"
#include "SumatraPDF.h"
#include "Translations.h"
#include "Theme.h"

#include "base/GuessFileType.h"

#include "AIChatCommon.h"
#include "EngineAll.h"
#include "Installer.h"

static Str kAIChatMarkerFileName = StrL("AIChat.enabled");

TempStr AIChatMarkerPathTemp(Str dir) {
    return path::JoinTemp(dir, kAIChatMarkerFileName);
}

bool IsAIChatInstallEnabled() {
    TempStr marker = AIChatMarkerPathTemp(GetSelfExeDirTemp());
    return file::Exists(marker);
}

bool IsAIChatAvailable() {
    // the chat UI is a WebView
    if (!HasWebView()) {
        return false;
    }
    // uninstalled (dev / portable) copies keep AI chat as before; installed
    // copies only when the installer checkbox opted in (marker file)
    if (!IsOurExeInstalled()) {
        return true;
    }
    return IsAIChatInstallEnabled();
}

bool IsAIChatSupportedForFile(Str filePath, Kind engineKind) {
    if (!filePath) {
        return false;
    }
    // Comics, image folders, single images, and DjVu have no useful text/agent
    // payload for chat (menu/context hide these via IsAIChatSupportedForTab).
    if (engineKind == kindEngineComicBooks || engineKind == kindEngineImageDir || engineKind == kindEngineImage ||
        engineKind == kindEngineDjVu) {
        return false;
    }
    FileType kind = GuessFileTypeFromName(filePath);
    return kind == FileType::PDF;
}

bool IsAIChatSupportedForTab(WindowTab* tab) {
    if (!tab || tab->IsAboutTab() || !tab->filePath) {
        return false;
    }
    return IsAIChatSupportedForFile(tab->filePath, tab->GetEngineType());
}

TempStr AIChatJsEscapeTemp(Str s) {
    if (!s) {
        return str::DupTemp("");
    }
    str::Builder buf;
    for (int i = 0; i < s.len; i++) {
        char c = s.s[i];
        switch (c) {
            case '\\':
                buf.Append("\\\\");
                break;
            case '\'':
                buf.Append("\\'");
                break;
            case '\n':
                buf.Append("\\n");
                break;
            case '\r':
                buf.Append("\\r");
                break;
            case '\t':
                buf.Append("\\t");
                break;
            default:
                buf.AppendChar(c);
                break;
        }
    }
    return ToStrTemp(buf);
}

TempStr AIChatJsonStrTemp(Str json, Str key) {
    TempStr pattern = fmt("\"%s\":\"", key);
    Str rest;
    if (!str::Cut(json, pattern, nullptr, &rest)) {
        return {};
    }
    str::Builder buf;
    for (int i = 0; i < rest.len; i++) {
        char c = rest.s[i];
        if (c == '"') {
            break;
        }
        if (c == '\\' && i + 1 < rest.len) {
            i++;
            c = rest.s[i];
            if (c == 'n') {
                buf.AppendChar('\n');
            } else if (c == 't') {
                buf.AppendChar('\t');
            } else if (c == '\\') {
                buf.AppendChar('\\');
            } else if (c == '"') {
                buf.AppendChar('"');
            } else {
                buf.AppendChar(c);
            }
        } else {
            buf.AppendChar(c);
        }
    }
    return ToStrTemp(buf);
}

MainWindow* AIChatFindMainWindowByFrame(HWND hwndFrame) {
    for (MainWindow* w : gWindows) {
        if (w->hwndFrame == hwndFrame) {
            return w;
        }
    }
    return nullptr;
}

void AIChatFreeSessions(Vec<AIChatSessionInfo>& sessions) {
    for (int i = 0; i < len(sessions); i++) {
        str::Free(sessions[i].sessionId);
        str::Free(sessions[i].display);
        str::Free(sessions[i].project);
    }
    sessions.Reset();
}

void AIChatSortSessionsByTimestampDesc(Vec<AIChatSessionInfo>& sessions) {
    int n = len(sessions);
    for (int i = 0; i < n - 1; i++) {
        for (int j = 0; j < n - 1 - i; j++) {
            if (sessions[j].timestamp < sessions[j + 1].timestamp) {
                AIChatSessionInfo tmp = sessions[j];
                sessions[j] = sessions[j + 1];
                sessions[j + 1] = tmp;
            }
        }
    }
}

i64 AIChatFileTimeToMs(const FILETIME& ft) {
    ULARGE_INTEGER uli;
    uli.LowPart = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;
    return (i64)(uli.QuadPart / 10000);
}

// In-memory record of the most recent chat traffic (sent commands + received
// stream), for post-mortem debugging when a chat fails. Fed from AIChatLog, so
// every ">>>"/"<<<" line the app already logs is captured here too. Bounded: the
// whole buffer is dropped once it grows past the cap.
static Mutex gAIChatDbgMu;
static str::Builder gAIChatDbgLog;
constexpr int kAIChatDbgMaxBytes = 256 * 1024;

void AIChatDebugReset() {
    ScopedMutex lk(&gAIChatDbgMu);
    gAIChatDbgLog.Reset();
}

TempStr AIChatDebugGetTemp() {
    ScopedMutex lk(&gAIChatDbgMu);
    return str::DupTemp(ToStr(gAIChatDbgLog));
}

static void AIChatWriteLogEntry(AIChatLogger* logger, Str direction, Str safeText) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    str::Builder entry;
    entry.Append(fmt("[%04d-%02d-%02d %02d:%02d:%02d] %s: ", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
                     st.wSecond, direction ? direction : StrL("event")));
    if (safeText) {
        entry.AppendChar(' ');
        entry.Append(safeText);
    }
    if (entry.LastChar() != '\n') {
        entry.AppendChar('\n');
    }

    {
        ScopedMutex lk(&gAIChatDbgMu);
        if (len(gAIChatDbgLog) > kAIChatDbgMaxBytes) {
            gAIChatDbgLog.Reset();
        }
        gAIChatDbgLog.Append(ToStr(entry));
    }

    if (!logger) {
        return;
    }
    if (logger->logTag) {
        logf("%s %s: %s", logger->logTag, direction ? direction : StrL("event"), safeText ? safeText : StrL(""));
    }

    TempStr dir = GetSumatraDataDirTemp();
    if (!dir || !logger->logFileName) {
        return;
    }
    TempStr path = path::JoinTemp(dir, logger->logFileName);
    if (!path || !logger->mutex) {
        return;
    }

    logger->mutex->Lock();
    FILE* f = fopen(CStrTemp(path), "a");
    if (f) {
        fwrite(ToStr(entry).s, 1, len(entry), f);
        fflush(f);
        fclose(f);
    }
    logger->mutex->Unlock();
}

void AIChatLog(AIChatLogger* logger, Str direction, Str text) {
    AIChatWriteLogEntry(logger, direction, fmt("bytes=%d", text ? len(text) : 0));
}

void AIChatLogMeta(AIChatLogger* logger, Str direction, Str key, i64 value) {
    AIChatWriteLogEntry(logger, direction, fmt("%s=%lld", key ? key : StrL("value"), value));
}

constexpr int kBtnIdAIChatLearnMore = 100;

static HRESULT CALLBACK AIChatNotInstalledDialogCallback(HWND /*hwnd*/, UINT msg, WPARAM wParam, LPARAM /*lParam*/,
                                                         LONG_PTR lpRefData) {
    Str docUri = lpRefData ? *(Str*)lpRefData : Str{};
    switch (msg) {
        case TDN_HYPERLINK_CLICKED:
            LaunchDocumentation(docUri);
            break;
        case TDN_BUTTON_CLICKED:
            if ((int)wParam == kBtnIdAIChatLearnMore) {
                LaunchDocumentation(docUri);
                return S_FALSE;
            }
            break;
    }
    return S_OK;
}

void AIChatShowNotInstalledDialog(const AIChatNotInstalledDialogArgs& args) {
    Str linkLabel = _TRA("AI Chat documentation");
    TempStr link = fmt(R"(<a href="#">%s</a>)", linkLabel);
    TempStr content = fmt(_TRA("See %s for setup instructions.").s, link);

    TASKDIALOG_BUTTON buttons[2];
    buttons[0].nButtonID = IDOK;
    buttons[0].pszButtonText = CWStrTemp(_TRA("OK"));
    buttons[1].nButtonID = kBtnIdAIChatLearnMore;
    buttons[1].pszButtonText = CWStrTemp(_TRA("Learn more"));

    TASKDIALOGCONFIG dialogConfig{};
    DWORD flags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_SIZE_TO_CONTENT | TDF_ENABLE_HYPERLINKS;
    if (trans::IsCurrLangRtl()) {
        flags |= TDF_RTL_LAYOUT;
    }
    dialogConfig.cbSize = sizeof(TASKDIALOGCONFIG);
    dialogConfig.pszWindowTitle = CWStrTemp(args.windowTitle);
    dialogConfig.pszMainInstruction = CWStrTemp(args.mainInstruction);
    dialogConfig.pszContent = CWStrTemp(content);
    dialogConfig.nDefaultButton = IDOK;
    dialogConfig.dwFlags = (TASKDIALOG_FLAGS)flags;
    dialogConfig.pfCallback = AIChatNotInstalledDialogCallback;
    dialogConfig.lpCallbackData = (LONG_PTR)&args.docUri;
    dialogConfig.pButtons = buttons;
    dialogConfig.cButtons = dimof(buttons);
    dialogConfig.pszMainIcon = TD_INFORMATION_ICON;

    TaskDialogIndirect(&dialogConfig, nullptr, nullptr, nullptr);
}

TempStr AIChatFindExecutableTemp(const StrVec& fullPathCandidates, WStr searchExeName, WStr searchNameNoExt) {
#ifdef _MSC_VER
    for (int i = 0; i < len(fullPathCandidates); i++) {
        if (file::Exists(fullPathCandidates[i])) {
            // copy into the temp arena: callers pass a local StrVec that is
            // destroyed on return, so returning a view into it would dangle
            return str::DupTemp(fullPathCandidates[i]);
        }
    }
    WCHAR pathW[MAX_PATH];
    if (searchExeName && SearchPathW(nullptr, searchExeName.s, nullptr, MAX_PATH, pathW, nullptr) > 0) {
        return ToUtf8Temp(pathW);
    }
    if (searchNameNoExt && SearchPathW(nullptr, searchNameNoExt.s, L".exe", MAX_PATH, pathW, nullptr) > 0) {
        return ToUtf8Temp(pathW);
    }
#endif
    return nullptr;
}

void AIChatAppendModelUnique(StrVec& models, Str model) {
    if (len(model) == 0) {
        return;
    }
    TempStr norm = str::DupTemp(model);
    int start = 0;
    while (start < norm.len && str::IsWs(norm.s[start])) {
        start++;
    }
    if (start >= norm.len) {
        return;
    }
    norm = Str(norm.s + start, norm.len - start);
    str::ToLowerInPlace(norm);
    for (int i = 0; i < len(models); i++) {
        if (str::EqI(models[i], norm)) {
            return;
        }
    }
    models.Append(norm);
}

int AIChatFindModelInList(const StrVec& models, Str model) {
    if (len(model) == 0) {
        return -1;
    }
    TempStr norm = str::DupTemp(model);
    str::ToLowerInPlace(norm);
    for (int i = 0; i < len(models); i++) {
        if (str::EqI(models[i], norm)) {
            return i;
        }
    }
    return -1;
}

// the saved model if it's in the list, else defaultModel
Str AIChatResolveModel(const StrVec& models, Str model, Str defaultModel) {
    int idx = AIChatFindModelInList(models, model);
    if (idx >= 0) {
        return models[idx];
    }
    idx = AIChatFindModelInList(models, defaultModel);
    if (idx >= 0) {
        return models[idx];
    }
    return defaultModel;
}

TempStr AIChatModelDisplayNameTemp(Str model, Str defaultDisplay) {
    if (len(model) == 0) {
        return str::DupTemp(defaultDisplay ? defaultDisplay : StrL(""));
    }
    TempStr dup = str::DupTemp(model);
    if (len(dup) > 0) {
        dup.s[0] = (char)toupper((unsigned char)dup.s[0]);
    }
    return dup;
}

bool AIChatGetMarkedJsResource(void* ctx, Str path, WebViewResourceResult* res) {
    auto* data = (LoadedDataResource*)ctx;
    if (!data || !res || len(path) == 0) {
        return false;
    }
    if (!str::EqI(path, StrL("/marked.min.js")) && !str::EqI(path, StrL("marked.min.js"))) {
        return false;
    }
    res->data = data->data;
    res->dataLen = data->dataSize;
    res->contentType = str::Dup(StrL("text/javascript"));
    res->ownsData = false;
    return res->dataLen > 0;
}

static const char* kAIChatHtmlFmt = R"(<!DOCTYPE html><html><head><meta charset='utf-8'>
<script src='%smarked.min.js'></script>
<style>
:root { %s }
* { margin: 0; padding: 0; box-sizing: border-box; }
body { font-family: 'Segoe UI', sans-serif; font-size: 13px; margin: 0; padding: 6px;
  background: var(--bg); color: var(--fg); line-height: 1.4; }
p { margin: 2px 0; }
h1,h2,h3,h4 { margin: 6px 0 2px 0; }
ul,ol { margin: 2px 0 2px 18px; }
li { margin: 1px 0; }
.user { color: var(--user); font-weight: bold; margin: 8px 0 2px 0; padding: 4px 0;
  border-top: 1px solid var(--border); }
.tool { color: var(--muted); font-size: 11px; font-style: italic;
  border-left: 3px solid var(--muted); padding-left: 6px; margin: 2px 0; }
.assistant { margin: 2px 0; }
.assistant pre { background: var(--code-bg); padding: 6px; border-radius: 4px;
  overflow-x: auto; margin: 3px 0; font-size: 12px; }
.assistant code { background: var(--code-bg); padding: 1px 3px; border-radius: 2px; font-size: 12px; }
.assistant pre code { background: none; padding: 0; }
.error { color: var(--error); font-weight: bold; margin: 4px 0; }
</style></head><body><div id='chat'></div>
<script>
var chatDiv = document.getElementById('chat');
var currentBlock = null;
var currentRaw = '';
function addUser(text) {
  flushBlock();
  var d = document.createElement('div');
  d.className = 'user';
  d.textContent = 'You: ' + text;
  chatDiv.appendChild(d);
  scrollToBottom();
}
function addTool(text) {
  flushBlock();
  var d = document.createElement('div');
  d.className = 'tool';
  d.textContent = text;
  chatDiv.appendChild(d);
  scrollToBottom();
}
function addError(text) {
  flushBlock();
  var d = document.createElement('div');
  d.className = 'error';
  d.textContent = text;
  chatDiv.appendChild(d);
  scrollToBottom();
}
function sanitizedMarkdown(markdown) {
  var t = document.createElement('template');
  t.innerHTML = marked.parse(markdown);
  var allowed = new Set(['A', 'BLOCKQUOTE', 'BR', 'CODE', 'DEL', 'EM', 'H1', 'H2', 'H3', 'H4', 'H5', 'H6',
                         'HR', 'LI', 'OL', 'P', 'PRE', 'STRONG', 'TABLE', 'TBODY', 'TD', 'TH', 'THEAD', 'TR', 'UL']);
  var nodes = Array.from(t.content.querySelectorAll('*'));
  for (var el of nodes) {
    if (!allowed.has(el.tagName)) {
      el.replaceWith(document.createTextNode(el.textContent || ''));
      continue;
    }
    for (var attr of Array.from(el.attributes)) {
      var keep = el.tagName === 'A' && (attr.name === 'href' || attr.name === 'title');
      if (!keep) {
        el.removeAttribute(attr.name);
      }
    }
    if (el.tagName === 'A' && el.hasAttribute('href')) {
      try {
        var u = new URL(el.getAttribute('href'), location.href);
        if (!['http:', 'https:', 'mailto:'].includes(u.protocol)) el.removeAttribute('href');
      } catch (_) {
        el.removeAttribute('href');
      }
    }
  }
  return t.content;
}
function appendText(text) {
  if (!currentBlock) {
    currentBlock = document.createElement('div');
    currentBlock.className = 'assistant';
    chatDiv.appendChild(currentBlock);
    currentRaw = '';
  }
  currentRaw += text;
  if (typeof marked !== 'undefined') {
    currentBlock.replaceChildren(sanitizedMarkdown(currentRaw));
  } else {
    currentBlock.textContent = currentRaw;
  }
  scrollToBottom();
}
function flushBlock() {
  currentBlock = null; currentRaw = '';
}
function clearChat() {
  chatDiv.innerHTML = '';
  flushBlock();
}
function scrollToBottom() {
  window.scrollTo(0, document.body.scrollHeight);
}
</script></body></html>)";

static bool IsHexColor(Str value) {
    if (len(value) != 4 && len(value) != 7 && len(value) != 9) {
        return false;
    }
    if (value.s[0] != '#') {
        return false;
    }
    for (int i = 1; i < len(value); i++) {
        u8 c = (u8)value.s[i];
        bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (!hex) {
            return false;
        }
    }
    return true;
}

static TempStr ColorToCssTemp(Color c) {
    return fmt("#%02x%02x%02x", (int)GetRValue(c), (int)GetGValue(c), (int)GetBValue(c));
}

TempStr AIChatFormatChatHtmlTemp(Str virtualHost, Str bgColor) {
    Str host = virtualHost ? virtualHost : StrL("");
    bool validBg = IsHexColor(bgColor);
    bool followTheme = !validBg || str::EqI(bgColor, StrL("#ffffff"));
    Color themeBg = ThemeControlBackgroundColor();
    bool dark = followTheme && !IsLightColor(themeBg);
    TempStr bg = followTheme ? ColorToCssTemp(themeBg) : str::DupTemp(bgColor);
    TempStr fg = dark ? ColorToCssTemp(ThemeWindowTextColor()) : str::DupTemp("#222222");
    Str muted = dark ? StrL("#a0a0a0") : StrL("#555555");
    Str user = dark ? StrL("#7fb3d5") : StrL("#1a5276");
    Str border = dark ? StrL("#4a4a4a") : StrL("#cccccc");
    Str codeBg = dark ? StrL("#3a3a3a") : StrL("#f0f0f0");
    Str error = dark ? StrL("#e74c3c") : StrL("#c0392b");
    TempStr cssVars = fmt("--bg:%s; --fg:%s; --muted:%s; --user:%s; --border:%s; --code-bg:%s; --error:%s;", bg, fg,
                          muted, user, border, codeBg, error);
    return fmt(kAIChatHtmlFmt, host, cssVars);
}

static void AIChatTerminateProcessTree(DWORD rootPid) {
    if (!rootPid || rootPid == GetCurrentProcessId()) {
        return;
    }

    Vec<DWORD> pendingPids;
    pendingPids.Append(rootPid);
    for (int i = 0; i < len(pendingPids); i++) {
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE) {
            continue;
        }
        PROCESSENTRY32W entry = {};
        entry.dwSize = sizeof(entry);
        if (Process32FirstW(snapshot, &entry)) {
            do {
                if (entry.th32ParentProcessID != pendingPids[i] || entry.th32ProcessID == 0 ||
                    pendingPids.Contains(entry.th32ProcessID)) {
                    continue;
                }
                pendingPids.Append(entry.th32ProcessID);
                HANDLE child = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, entry.th32ProcessID);
                if (child) {
                    TerminateProcess(child, 0);
                    CloseHandle(child);
                }
            } while (Process32NextW(snapshot, &entry));
        }
        CloseHandle(snapshot);
    }
}

void AIChatCloseProcess(HANDLE* processHandle, bool terminateIfRunning) {
    if (!processHandle || !*processHandle) {
        return;
    }
    HANDLE h = *processHandle;
    *processHandle = nullptr;
    DWORD waitResult = WaitForSingleObject(h, 0);
    if (terminateIfRunning && waitResult != WAIT_OBJECT_0) {
        DWORD processId = GetProcessId(h);
        if (processId) {
            AIChatTerminateProcessTree(processId);
        }
        TerminateProcess(h, 0);
    }
    CloseHandle(h);
}

static bool IsSafeCmdScriptArg(Str value) {
    for (int i = 0; i < len(value); i++) {
        char c = value.s[i];
        if (c == '"' || c == '&' || c == '|' || c == '<' || c == '>' || c == '^' || c == '(' || c == ')' || c == '%' ||
            c == '!' || c == '\r' || c == '\n') {
            return false;
        }
    }
    return true;
}

static TempStr AIChatPrepareProcessCommandLine(Str cmdLine) {
    int argc = 0;
    WCHAR** argv = CommandLineToArgvW(CWStrTemp(cmdLine), &argc);
    if (!argv || argc < 1) {
        if (argv) {
            LocalFree(argv);
        }
        return {};
    }

    TempStr exePath = ToUtf8Temp(argv[0]);
    bool isScript = str::EndsWithI(exePath, StrL(".cmd")) || str::EndsWithI(exePath, StrL(".bat"));
    if (!isScript) {
        LocalFree(argv);
        return str::DupTemp(cmdLine);
    }
    if (!IsSafeCmdScriptArg(exePath)) {
        LocalFree(argv);
        return {};
    }
    for (int i = 1; i < argc; i++) {
        if (!IsSafeCmdScriptArg(ToUtf8Temp(argv[i]))) {
            LocalFree(argv);
            return {};
        }
    }

    WCHAR systemDir[MAX_PATH]{};
    UINT systemDirLen = GetSystemDirectoryW(systemDir, dimof(systemDir));
    if (systemDirLen == 0 || systemDirLen >= dimof(systemDir)) {
        LocalFree(argv);
        return {};
    }
    TempStr cmdExe = path::JoinTemp(ToUtf8Temp(systemDir), StrL("cmd.exe"));
    if (!file::Exists(cmdExe)) {
        LocalFree(argv);
        return {};
    }

    str::Builder payload;
    payload.Append(QuoteCmdLineArgTemp(exePath));
    for (int i = 1; i < argc; i++) {
        payload.AppendChar(' ');
        payload.Append(QuoteCmdLineArgTemp(ToUtf8Temp(argv[i])));
    }
    LocalFree(argv);
    return fmt("%s /d /s /c \"%s\"", QuoteCmdLineArgTemp(cmdExe), ToStr(payload));
}

static bool AIChatLaunchProcessWithPipes(Str cmdLine, Str cwd, bool withStdin, AIChatProcessLaunchResult* out) {
    if (!out || len(cmdLine) == 0) {
        return false;
    }
    *out = {};

    TempStr processCmdLine = AIChatPrepareProcessCommandLine(cmdLine);
    if (!processCmdLine) {
        return false;
    }

    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE hStdoutRead = nullptr;
    HANDLE hStdoutWrite = nullptr;
    HANDLE hStdinRead = nullptr;
    HANDLE hStdinWrite = nullptr;
    auto closeHandle = [](HANDLE& h) {
        if (h && h != INVALID_HANDLE_VALUE) {
            CloseHandle(h);
            h = nullptr;
        }
    };

    if (!CreatePipe(&hStdoutRead, &hStdoutWrite, &sa, 0) ||
        !SetHandleInformation(hStdoutRead, HANDLE_FLAG_INHERIT, 0)) {
        closeHandle(hStdoutRead);
        closeHandle(hStdoutWrite);
        return false;
    }
    if (withStdin && (!CreatePipe(&hStdinRead, &hStdinWrite, &sa, 0) ||
                      !SetHandleInformation(hStdinWrite, HANDLE_FLAG_INHERIT, 0))) {
        closeHandle(hStdoutRead);
        closeHandle(hStdoutWrite);
        closeHandle(hStdinRead);
        closeHandle(hStdinWrite);
        return false;
    }

    HANDLE inheritedHandles[2] = {hStdoutWrite, hStdinRead};
    SIZE_T inheritedCount = withStdin ? 2 : 1;
    SIZE_T attributeBytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
    if (attributeBytes == 0) {
        closeHandle(hStdoutRead);
        closeHandle(hStdoutWrite);
        closeHandle(hStdinRead);
        closeHandle(hStdinWrite);
        return false;
    }
    void* attributeMemory = malloc(attributeBytes);
    if (!attributeMemory) {
        closeHandle(hStdoutRead);
        closeHandle(hStdoutWrite);
        closeHandle(hStdinRead);
        closeHandle(hStdinWrite);
        return false;
    }
    PPROC_THREAD_ATTRIBUTE_LIST attributes = (PPROC_THREAD_ATTRIBUTE_LIST)attributeMemory;
    bool attributesInitialized = InitializeProcThreadAttributeList(attributes, 1, 0, &attributeBytes) != FALSE;
    bool attributesReady = attributesInitialized;
    if (attributesReady) {
        attributesReady = UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inheritedHandles,
                                                    inheritedCount * sizeof(HANDLE), nullptr, nullptr) != FALSE;
    }

    STARTUPINFOEXW startupInfo = {};
    startupInfo.StartupInfo.cb = sizeof(startupInfo);
    startupInfo.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startupInfo.StartupInfo.hStdInput = withStdin ? hStdinRead : nullptr;
    startupInfo.StartupInfo.hStdOutput = hStdoutWrite;
    startupInfo.StartupInfo.hStdError = hStdoutWrite;

    PROCESS_INFORMATION processInfo = {};
    WCHAR* commandLineW = CWStrTemp(processCmdLine);
    WCHAR* dirW = len(cwd) > 0 ? CWStrTemp(cwd) : nullptr;
    BOOL created = FALSE;
    if (attributesReady) {
        created = CreateProcessW(nullptr, commandLineW, nullptr, nullptr, TRUE,
                                 EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW, nullptr, dirW,
                                 &startupInfo.StartupInfo, &processInfo);
    }
    if (attributesInitialized) {
        DeleteProcThreadAttributeList(attributes);
    }
    free(attributeMemory);

    closeHandle(hStdoutWrite);
    closeHandle(hStdinRead);
    if (!created) {
        closeHandle(hStdoutRead);
        closeHandle(hStdinWrite);
        return false;
    }

    CloseHandle(processInfo.hThread);
    out->ok = true;
    out->hProcess = processInfo.hProcess;
    out->hReadPipe = hStdoutRead;
    out->hWritePipe = hStdinWrite;
    out->processId = processInfo.dwProcessId;
    return true;
}

bool AIChatLaunchProcessWithStdoutPipe(Str cmdLine, Str cwd, AIChatProcessLaunchResult* out) {
    return AIChatLaunchProcessWithPipes(cmdLine, cwd, false, out);
}

bool AIChatLaunchProcessWithStdinPipe(Str cmdLine, Str cwd, AIChatProcessLaunchResult* out) {
    return AIChatLaunchProcessWithPipes(cmdLine, cwd, true, out);
}

constexpr int kAIChatLabelCloseBtnDx = 16;
constexpr int kAIChatLabelCloseBtnSpaceDx = 8;
constexpr int kAIChatLabelPadX = 2;

int AIChatLabelMaxTextDx(int labelDx) {
    int padX = DpiScale(kAIChatLabelPadX);
    int btnDx = DpiScale(kAIChatLabelCloseBtnDx);
    int spaceDx = DpiScale(kAIChatLabelCloseBtnSpaceDx);
    int maxDx = labelDx - btnDx - spaceDx - (2 * padX);
    return maxDx > 0 ? maxDx : 0;
}

TempStr AIChatFitPanelTitleTemp(PlatformFont* font, Str prefix, Str docName, int maxDx) {
    TempStr full = str::JoinTemp(prefix, docName);
    if (maxDx <= 0) {
        return full;
    }
    Size sz = PlatformFontMeasureText(font, full);
    if (sz.dx <= maxDx) {
        return full;
    }

    int nRunes = utf8StrLen((u8*)docName.s);
    if (nRunes < 0) {
        return full;
    }

    TempStr best = str::JoinTemp(prefix, ShortenStringUtf8Temp(docName, 1));
    int lo = 1;
    int hi = nRunes;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        TempStr trial = str::JoinTemp(prefix, ShortenStringUtf8Temp(docName, mid));
        sz = PlatformFontMeasureText(font, trial);
        if (sz.dx <= maxDx) {
            best = trial;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return best;
}

TempStr AIChatGenerateSessionIdTemp() {
    GUID guid;
    if (FAILED(CoCreateGuid(&guid))) {
        return nullptr;
    }
    return fmt("%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x", guid.Data1, guid.Data2, guid.Data3, guid.Data4[0],
               guid.Data4[1], guid.Data4[2], guid.Data4[3], guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7]);
}

static AIChatBackend BackendFromTabStorage(int v) {
    if (v < 0 || v >= kAIChatProviderCount) {
        return AIChatBackend::None;
    }
    return (AIChatBackend)v;
}

static int BackendToTabStorage(AIChatBackend backend) {
    if (backend == AIChatBackend::None) {
        return -1;
    }
    return (int)backend;
}

AIChatBackend AIChatGetTabPanelOpen(WindowTab* tab) {
    if (!tab || tab->IsAboutTab()) {
        return AIChatBackend::None;
    }
    return BackendFromTabStorage(tab->aiChatPanelOpen);
}

void AIChatSetTabPanelOpen(WindowTab* tab, AIChatBackend backend) {
    if (!tab || tab->IsAboutTab()) {
        return;
    }
    tab->aiChatPanelOpen = BackendToTabStorage(backend);
}

// records the desired panel visibility; RelayoutFrame (via the scheduled UI
// update, which every caller triggers) shows/hides the panel windows
void AIChatSyncPanelsToCurrentTab(MainWindow* win) {
    if (!win) {
        return;
    }
    AIChatBackend open = AIChatGetTabPanelOpen(win->CurrentTab());
    win->uiState.aiChatVisible = open != AIChatBackend::None;
}

void AIChatApplySavedSidebarDx(MainWindow* win) {
    if (!win) {
        return;
    }
    if (gGlobalPrefs->aiChatSidebarDx > 0) {
        win->aiChatDx = gGlobalPrefs->aiChatSidebarDx;
    }
}

void AIChatUpdateSidebarDx(MainWindow* win, int dx, bool persist) {
    if (!win) {
        return;
    }
    win->aiChatDx = dx;
    if (dx > 0) {
        gGlobalPrefs->aiChatSidebarDx = dx;
    }
    if (persist) {
        ScheduleSaveSettings();
    }
}

void AIChatWaitForTabProcessesToFinish(MainWindow* win, bool (*tabHasRunningProcess)(WindowTab*)) {
    if (!win || !tabHasRunningProcess) {
        return;
    }
    u64 deadline = GetTickCount64() + 5000;
    for (;;) {
        uitask::DrainQueue();
        bool anyRunning = false;
        for (WindowTab* tab : win->Tabs()) {
            if (tab && tabHasRunningProcess(tab)) {
                anyRunning = true;
            }
        }
        if (!anyRunning || GetTickCount64() >= deadline) {
            break;
        }
        Sleep(10);
    }
    uitask::DrainQueue();
}
