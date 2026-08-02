/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

struct MainWindow;
struct WindowTab;
struct TocItem;
struct FileState;
struct Favorite;

// Category group IDs for result grouping in the palette list.
// Stored in ItemDataCP.indent for non-TOC items (TOC uses indent for depth).
// We shift TOC indent by +kGroupTocOffset so grouping and indenting coexist.
enum PaletteGroup : int {
    PaletteGroup_Commands = 0,
    PaletteGroup_Tabs = 1,
    PaletteGroup_FileHistory = 2,
    PaletteGroup_TOC = 3,
    PaletteGroup_Favorites = 4,
    PaletteGroup_Count = 5,
};

// TOC items store real indent depth in indent; non-TOC items use indent for
// PaletteGroup. TOC indent is offset by this to distinguish from group IDs.
constexpr int kGroupTocOffset = 100;

struct ItemDataCP {
    i32 cmdId = 0;
    WindowTab* tab = nullptr;
    Str filePath;
    TocItem* tocItem = nullptr;
    int indent = 0; // TOC: indent depth (0-based).  Non-TOC: PaletteGroup.
    int pageNo = 0; // toc entry destination page (0 if none), shown in the list
    FileState* favFs = nullptr;
    Favorite* fav = nullptr;

    // Relevance score for search result ranking (higher = better match)
    int relevanceScore = 0;
};

using StrVecCP = StrVecWithData<ItemDataCP>;

struct ListBoxModelCP : ListBoxModel {
    StrVecCP strings;

    ListBoxModelCP() = default;
    ~ListBoxModelCP() override = default;
    int ItemsCount() override { return len(strings); }
    Str Item(int i) override { return strings.At(i); }
    ItemDataCP* Data(int i) { return strings.AtData(i); }
};

struct CommandPaletteWnd : Wnd {
    ~CommandPaletteWnd() override = default;
    HFONT font = nullptr;
    MainWindow* win = nullptr;

    Edit* editQuery = nullptr;
    Static* clearButton = nullptr; // \"×\" button that clears the query
    StrVecCP tabs;
    StrVecCP fileHistory;
    StrVecCP commands;
    StrVecCP toc;
    StrVecCP favorites;
    ListBox* listBox = nullptr;
    Static* staticInfo = nullptr;

    StrVec filterWords;
    Vec<u8> highlighted;

    int currTabIdx = 0;
    int currTocIdx = 0;
    bool tocMode = false;
    bool smartTabMode = false;
    bool stickyMode = false;

    // Esc-twice-to-close: first Esc when query is non-empty clears query;
    // second Esc (or first Esc when query is already empty) closes.
    bool queryWasEmptyOnLastEsc = true;

    bool PreTranslateMessage(MSG&) override;
    LRESULT WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) override;

    void CollectStrings(MainWindow*);
    void CollectTabsRegular(MainWindow*, WindowTab* currTab);
    void CollectTabsMru(MainWindow*, WindowTab* currTab);
    void CollectToc(MainWindow*);
    void CollectFavorites(MainWindow*);
    void FilterStringsForQuery(Str, StrVecCP&);
    void FilterStringsWithRelevance(StrVecCP& src, const StrVec& words, StrVecCP& dst);

    bool Create(MainWindow* win, Str prefix, int smartTabAdvance);
    void QueryChanged();
    void UpdateResultCount();
    void ClearQuery();

    void ExecuteCurrentSelection();
    bool AdvanceSelection(int dir);
    void SwitchToPrefix(Str prefix);
    void SwitchToCommands();
    void SwitchToTabs();
    void SwitchToEverything();
    void SwitchToFileHistory();
    void SwitchToTOC();
    void SwitchToFavorites();
    void OnSelectionChange();
    void OnListDoubleClick();
    void DrawListBoxItem(ListBox::DrawItemEvent* ev);
};

extern CommandPaletteWnd* gCommandPaletteWnd;
extern HWND gCommandPaletteHwnd;

Str CommandPaletteSkipWS(Str s);
void CommandPaletteSetCurrentSelection(CommandPaletteWnd* wnd, int idx);
void ScheduleDeleteAndExecCommand(i32 cmdId = 0);
void SafeDeleteCommandPaletteWnd();
void PositionCommandPalette(HWND hwnd, HWND hwndRelative);