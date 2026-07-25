/* Copyright 2024 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

// Thumbnail sidebar panel. Shows page thumbnails in a vertical list, lets the
// user click to navigate. Reads thumbnails from the render cache.

struct ThumbnailPanel {
    HWND hwnd = nullptr;
    HWND hwndOwner = nullptr; // the frame window
    int itemHeight = 120;     // height of each thumbnail row (DPI-scaled)
    int width = 180;          // panel width (DPI-scaled)
    int scrollPos = 0;
    int totalHeight = 0;

    // Selected / hovered page index (0-based)
    int selectedPage = -1;
    int hoveredPage = -1;

    bool visible = false;
    bool pinned = false; // if true, stays visible; otherwise auto-hides

    // Create the thumbnail panel window as a child of hwndOwner.
    void Create(HWND parent);

    // Show/hide the panel (with slide animation if animated=true).
    void Show(bool show, bool animated = true);

    // Toggle visibility (click on the sidebar toggle).
    void Toggle();

    // Update thumbnails when the document changes.
    void ReloadThumbnails();

    // Resize and reposition within the main window layout.
    void UpdateLayout(int parentY, int parentHeight);

    // Schedule a repaint.
    void Invalidate();
};

// Singleton per MainWindow: embed ThumbnailPanel in MainWindow.h fields.
