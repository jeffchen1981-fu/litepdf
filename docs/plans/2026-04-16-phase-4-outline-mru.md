# Phase 4: Outline Pane + MRU — Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use `superpowers:subagent-driven-development` to execute this plan task-by-task (same pattern as Phases 2-3: implementer → spec reviewer → quality reviewer → fix).

**Goal:** Add a collapsible outline (bookmark) sidebar that navigates to pages on click, and a Most-Recently-Used file list in the File menu that persists across restarts via the Windows registry.

**Architecture:** `OutlinePane` is a child HWND wrapping a Win32 TreeView control, placed to the left of `PdfCanvas` in `MainWindow`. A simple static splitter (fixed width, no drag-resize — YAGNI for Phase 4) divides them. `MruList` is a headless, testable class that reads/writes up to 10 paths under `HKCU\Software\LitePDF\MRU`. `MainWindow` populates the File menu dynamically from `MruList` on each `WM_INITMENUPOPUP`.

**Tech Stack:** C++17, Win32 TreeView (comctl32), Windows Registry API (`RegCreateKeyExW` / `RegSetValueExW` / `RegQueryValueExW`), existing Catch2/CMake from Phases 0-3.

**Prerequisites (from Phase 3 + patch):**

- Tag `v0.0.4-phase3.1` on `main`; CI green on `windows-latest`.
- `Document::outline()` returns `vector<OutlineEntry>` with `depth`, `title`, `page_index` fields. Tested in `test_document_outline.cpp` against `bookmarks.pdf` fixture.
- `DocumentView` owns per-tab state; `MainWindow` owns one `DocumentView` via `unique_ptr`.
- 59/59 tests passing.

**Done when:**

1. Opening `bookmarks.pdf` shows an outline sidebar on the left with a TreeView whose items match the PDF's bookmark tree.
2. Clicking any outline entry navigates to its target page and renders it.
3. PDFs without outlines (e.g., `simple.pdf`) show no sidebar.
4. View → Toggle Outline (F5) shows/hides the sidebar.
5. Opening a file adds it to the MRU list stored at `HKCU\Software\LitePDF\MRU`.
6. File menu shows up to 10 recent files (numbered `&1`, `&2`, …) below the Open item.
7. Clicking an MRU entry opens that file (or shows "file not found" and removes from MRU).
8. MRU persists across `litepdf.exe` restarts.
9. `ctest --test-dir build -C Release` passes all Phase 3 tests (no regression) plus new MRU + outline unit tests.
10. Smoke test extended: launch with `bookmarks.pdf`, verify outline pane visible.
11. Tag `v0.0.5-phase4` pushed.

**Learnings carried from Phase 3:**

- UI thread must own its own `fz_context*` clone (D3 from Phase 3). Phase 4 adds no new MuPDF context usage — `Document::outline()` is called on the UI thread via `view_->doc()`, which already owns a ctx.
- `DragQueryFileW` now uses dynamic buffer (Phase 3.1 patch).
- `std::call_once` for class registration (Phase 3.1 patch).
- PIMPL discipline: headers stay MuPDF-free; only `.cpp` files pull fitz.h.

---

## Architectural Decisions

Pinned before tasks; revisit only if a task uncovers a blocker.

**D1. OutlinePane = left sidebar, fixed 250 DIP width.** No drag-resize splitter in Phase 4 — YAGNI. The sidebar is a child HWND containing a TreeView control. Hidden by default; auto-shown when a document with outline entries opens. `MainWindow` manages the layout: if outline visible, canvas starts at x=250; otherwise canvas fills the full client area.

**D2. Toggle mechanism: View → Toggle Outline (F5).** Adds `IDM_VIEW_OUTLINE` to menu + accelerator. Toggling hides/shows the `OutlinePane` HWND and repositions `PdfCanvas` via `on_layout()`.

**D3. MruList is a headless, unit-testable class.** Constructor takes a registry key path string (default `SOFTWARE\LitePDF\MRU`) so tests can use a disposable test key. API: `load()`, `save()`, `push(path)` (moves to front, caps at 10), `entries()` (returns `vector<wstring>`), `remove(index)`. All registry I/O goes through `load()`/`save()` — the in-memory list is the source of truth between calls.

**D4. MRU in File menu: dynamic insertion via `WM_INITMENUPOPUP`.** On `WM_INITMENUPOPUP` for the File menu, `MainWindow` removes old MRU items and re-inserts numbered entries (`&1 path`, `&2 path`, …) between the separator and Exit. Menu IDs: `IDM_MRU_1` through `IDM_MRU_10` (contiguous range 40020-40029).

**D5. MRU max 10 entries, HKCU only.** Per-user, no admin elevation. Registry values: `Entry0` through `Entry9` as `REG_SZ`. `Count` as `REG_DWORD`. Simple, no JSON, no file-based config.

**D6. Outline auto-show / auto-hide.** When `WM_USER_OPEN_OK` fires:
  - If new document has outline entries → show `OutlinePane`, populate tree.
  - If new document has no outline → hide `OutlinePane`, expand canvas.
  - User can override with F5 toggle at any time.

**D7. DocumentView exposes Document for outline queries.** Add `const Document& document() const` accessor to `DocumentView`. This lets `MainWindow` call `view_->document().outline()` on the UI thread without exposing MuPDF internals (outline returns plain `vector<OutlineEntry>`).

**D8. OutlinePane communicates page selection via callback.** `OutlinePane` takes a `std::function<void(int page)>` callback set by `MainWindow`. On `TVN_SELCHANGED`, the pane calls the callback with the target page index. `MainWindow::on_outline_navigate(int page)` calls `view_->set_current_page(page)` + `kick_render(page)`.

---

## Task List

### Task 0: Expose Document from DocumentView

**Files:**
- Modify: `src/core/DocumentView.hpp`
- Modify: `src/core/DocumentView.cpp`

**Step 1: Add accessor to header**

In `DocumentView.hpp`, add a public method after `source_path()`:

```cpp
    // Read-only access to the underlying Document (for outline queries,
    // page count, etc.). The Document reference is valid for the
    // lifetime of this DocumentView.
    const Document& document() const;
```

**Step 2: Implement in .cpp**

In `DocumentView.cpp`, add:

```cpp
const Document& DocumentView::document() const {
    return impl_->doc;
}
```

**Step 3: Build and verify tests pass**

Run: `cmake --build build --config Release && ctest --test-dir build -C Release`
Expected: 59/59 pass (no new tests — trivial accessor).

**Step 4: Commit**

```bash
git add src/core/DocumentView.hpp src/core/DocumentView.cpp
git commit -m "feat(core): expose Document accessor on DocumentView (Phase 4 D7)"
```

---

### Task 1: MruList — headless registry-backed MRU class (TDD)

**Files:**
- Create: `src/core/MruList.hpp`
- Create: `src/core/MruList.cpp`
- Create: `tests/unit/test_mru_list.cpp`
- Modify: `CMakeLists.txt` (add `MruList.cpp` to `litepdf_core`)
- Modify: `tests/CMakeLists.txt` (add test source)

**Step 1: Write tests first**

Create `tests/unit/test_mru_list.cpp`:

```cpp
#include "core/MruList.hpp"

#include <catch2/catch_test_macros.hpp>
#include <windows.h>

using namespace litepdf::core;

// Use a disposable registry key so tests don't pollute real MRU.
static constexpr wchar_t kTestKey[] = L"SOFTWARE\\LitePDF\\TestMRU";

// Helper: delete the test key to start clean.
static void delete_test_key() {
    RegDeleteKeyW(HKEY_CURRENT_USER, kTestKey);
}

TEST_CASE("MruList starts empty", "[mru]") {
    delete_test_key();
    MruList mru(kTestKey);
    mru.load();
    REQUIRE(mru.entries().empty());
}

TEST_CASE("MruList push adds entry and persists via save/load", "[mru]") {
    delete_test_key();
    {
        MruList mru(kTestKey);
        mru.load();
        mru.push(L"C:\\docs\\a.pdf");
        mru.push(L"C:\\docs\\b.pdf");
        mru.save();
    }
    {
        MruList mru2(kTestKey);
        mru2.load();
        auto e = mru2.entries();
        REQUIRE(e.size() == 2);
        // Most recent first
        REQUIRE(e[0] == L"C:\\docs\\b.pdf");
        REQUIRE(e[1] == L"C:\\docs\\a.pdf");
    }
    delete_test_key();
}

TEST_CASE("MruList push moves duplicate to front", "[mru]") {
    delete_test_key();
    MruList mru(kTestKey);
    mru.load();
    mru.push(L"C:\\a.pdf");
    mru.push(L"C:\\b.pdf");
    mru.push(L"C:\\a.pdf");  // should move to front, not duplicate
    REQUIRE(mru.entries().size() == 2);
    REQUIRE(mru.entries()[0] == L"C:\\a.pdf");
    REQUIRE(mru.entries()[1] == L"C:\\b.pdf");
    delete_test_key();
}

TEST_CASE("MruList caps at 10 entries", "[mru]") {
    delete_test_key();
    MruList mru(kTestKey);
    mru.load();
    for (int i = 0; i < 15; ++i) {
        mru.push(L"C:\\file" + std::to_wstring(i) + L".pdf");
    }
    REQUIRE(mru.entries().size() == 10);
    // Most recent (file14) is first
    REQUIRE(mru.entries()[0] == L"C:\\file14.pdf");
    delete_test_key();
}

TEST_CASE("MruList remove erases by index", "[mru]") {
    delete_test_key();
    MruList mru(kTestKey);
    mru.load();
    mru.push(L"C:\\a.pdf");
    mru.push(L"C:\\b.pdf");
    mru.push(L"C:\\c.pdf");
    mru.remove(1);  // remove b.pdf (index 1 in [c, b, a])
    auto e = mru.entries();
    REQUIRE(e.size() == 2);
    REQUIRE(e[0] == L"C:\\c.pdf");
    REQUIRE(e[1] == L"C:\\a.pdf");
    delete_test_key();
}
```

**Step 2: Create header**

Create `src/core/MruList.hpp`:

```cpp
#pragma once

// core::MruList — most-recently-used file list backed by the Windows
// registry. Stores up to kMaxEntries paths under HKCU. Headless and
// unit-testable: constructor accepts a registry sub-key path so tests
// can use a disposable key.
//
// Usage:
//   MruList mru;        // uses default key SOFTWARE\LitePDF\MRU
//   mru.load();         // read from registry
//   mru.push(path);     // add/move-to-front
//   mru.save();         // write back
//   auto v = mru.entries();  // most-recent-first

#include <cstddef>
#include <string>
#include <vector>

namespace litepdf::core {

class MruList {
public:
    static constexpr std::size_t kMaxEntries = 10;
    static constexpr wchar_t kDefaultKey[] = L"SOFTWARE\\LitePDF\\MRU";

    explicit MruList(const wchar_t* registry_subkey = kDefaultKey);

    // Read entries from registry into in-memory list.
    void load();

    // Write in-memory list to registry.
    void save() const;

    // Add or move `path` to the front. Caps at kMaxEntries.
    void push(const std::wstring& path);

    // Remove entry at `index`. No-op if out of range.
    void remove(std::size_t index);

    // Current entries, most-recent first.
    const std::vector<std::wstring>& entries() const noexcept { return entries_; }

private:
    std::wstring registry_subkey_;
    std::vector<std::wstring> entries_;
};

}  // namespace litepdf::core
```

**Step 3: Implement**

Create `src/core/MruList.cpp`:

```cpp
#include "core/MruList.hpp"

#include <windows.h>
#include <algorithm>

namespace litepdf::core {

MruList::MruList(const wchar_t* registry_subkey)
    : registry_subkey_(registry_subkey) {}

void MruList::load() {
    entries_.clear();
    HKEY hkey = nullptr;
    LONG rc = RegOpenKeyExW(HKEY_CURRENT_USER, registry_subkey_.c_str(),
                            0, KEY_READ, &hkey);
    if (rc != ERROR_SUCCESS) return;  // key doesn't exist yet → empty

    DWORD count = 0;
    DWORD cb = sizeof(count);
    rc = RegQueryValueExW(hkey, L"Count", nullptr, nullptr,
                          reinterpret_cast<BYTE*>(&count), &cb);
    if (rc != ERROR_SUCCESS || count == 0) {
        RegCloseKey(hkey);
        return;
    }
    if (count > kMaxEntries) count = static_cast<DWORD>(kMaxEntries);

    for (DWORD i = 0; i < count; ++i) {
        wchar_t name[16];
        wsprintfW(name, L"Entry%u", i);

        wchar_t buf[1024] = {};
        DWORD buf_cb = sizeof(buf);
        rc = RegQueryValueExW(hkey, name, nullptr, nullptr,
                              reinterpret_cast<BYTE*>(buf), &buf_cb);
        if (rc == ERROR_SUCCESS && buf[0] != L'\0') {
            entries_.emplace_back(buf);
        }
    }
    RegCloseKey(hkey);
}

void MruList::save() const {
    HKEY hkey = nullptr;
    DWORD disp = 0;
    LONG rc = RegCreateKeyExW(HKEY_CURRENT_USER, registry_subkey_.c_str(),
                              0, nullptr, REG_OPTION_NON_VOLATILE,
                              KEY_WRITE, nullptr, &hkey, &disp);
    if (rc != ERROR_SUCCESS) return;

    DWORD count = static_cast<DWORD>(entries_.size());
    RegSetValueExW(hkey, L"Count", 0, REG_DWORD,
                   reinterpret_cast<const BYTE*>(&count), sizeof(count));

    for (DWORD i = 0; i < count; ++i) {
        wchar_t name[16];
        wsprintfW(name, L"Entry%u", i);
        const auto& e = entries_[i];
        RegSetValueExW(hkey, name, 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(e.c_str()),
                       static_cast<DWORD>((e.size() + 1) * sizeof(wchar_t)));
    }
    // Clean up stale entries beyond current count (from a previous
    // session that may have had more items).
    for (DWORD i = count; i < static_cast<DWORD>(kMaxEntries); ++i) {
        wchar_t name[16];
        wsprintfW(name, L"Entry%u", i);
        RegDeleteValueW(hkey, name);
    }
    RegCloseKey(hkey);
}

void MruList::push(const std::wstring& path) {
    // Remove existing duplicate (case-insensitive on Windows).
    entries_.erase(
        std::remove_if(entries_.begin(), entries_.end(),
            [&](const std::wstring& e) {
                return _wcsicmp(e.c_str(), path.c_str()) == 0;
            }),
        entries_.end());

    entries_.insert(entries_.begin(), path);

    if (entries_.size() > kMaxEntries)
        entries_.resize(kMaxEntries);
}

void MruList::remove(std::size_t index) {
    if (index < entries_.size())
        entries_.erase(entries_.begin() + static_cast<ptrdiff_t>(index));
}

}  // namespace litepdf::core
```

**Step 4: Wire into CMake**

In `CMakeLists.txt`, add `src/core/MruList.cpp` to `litepdf_core`:

```cmake
add_library(litepdf_core STATIC
    src/core/Document.cpp
    src/core/DocumentView.cpp
    src/core/MruList.cpp
    src/core/PageCache.cpp
    src/core/RenderEngine.cpp
)
```

In `tests/CMakeLists.txt`, add the test source to the test executable's source list (after `test_document_view.cpp`):

```
    unit/test_mru_list.cpp
```

**Step 5: Build and run tests**

Run: `cmake --build build --config Release && ctest --test-dir build -C Release`
Expected: All 59 old tests pass + 5 new MRU tests pass = **64 total**.

**Step 6: Commit**

```bash
git add src/core/MruList.hpp src/core/MruList.cpp tests/unit/test_mru_list.cpp CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat(core): MruList — registry-backed MRU with TDD (Phase 4 D3/D5)"
```

---

### Task 2: Menu IDs + resource updates

**Files:**
- Modify: `resources/MainMenu.rc.h`
- Modify: `resources/litepdf.rc`

**Step 1: Add new menu IDs**

In `resources/MainMenu.rc.h`, append:

```cpp
#define IDM_VIEW_OUTLINE 40013

// MRU menu items: contiguous range 40020-40029
#define IDM_MRU_1        40020
#define IDM_MRU_2        40021
#define IDM_MRU_3        40022
#define IDM_MRU_4        40023
#define IDM_MRU_5        40024
#define IDM_MRU_6        40025
#define IDM_MRU_7        40026
#define IDM_MRU_8        40027
#define IDM_MRU_9        40028
#define IDM_MRU_10       40029
```

**Step 2: Update menu resource**

In `resources/litepdf.rc`, update the `POPUP "&File"` block to add MRU separator, and add outline toggle to View menu:

```rc
IDM_MAIN_MENU MENU
{
    POPUP "&File"
    {
        MENUITEM "&Open...\tCtrl+O", IDM_FILE_OPEN
        MENUITEM SEPARATOR
        // MRU entries are inserted dynamically at WM_INITMENUPOPUP.
        // The separator above doubles as the MRU divider.
        MENUITEM SEPARATOR
        MENUITEM "E&xit",            IDM_FILE_EXIT
    }
    POPUP "&View"
    {
        MENUITEM "Toggle &Outline\tF5", IDM_VIEW_OUTLINE
        MENUITEM SEPARATOR
        MENUITEM "Zoom &In\tCtrl+=",  IDM_ZOOM_IN
        MENUITEM "Zoom &Out\tCtrl+-", IDM_ZOOM_OUT
        MENUITEM "&Reset Zoom\tCtrl+0", IDM_ZOOM_RESET
    }
    POPUP "&Help"
    {
        MENUITEM "&About LitePDF",   IDM_HELP_ABOUT
    }
}
```

**Step 3: Build**

Run: `cmake --build build --config Release`
Expected: Compiles. No new tests (resource-only change).

**Step 4: Commit**

```bash
git add resources/MainMenu.rc.h resources/litepdf.rc
git commit -m "feat(ui): add menu IDs for outline toggle + MRU (Phase 4 D2/D4)"
```

---

### Task 3: OutlinePane — TreeView wrapper

**Files:**
- Create: `src/ui/OutlinePane.hpp`
- Create: `src/ui/OutlinePane.cpp`
- Modify: `CMakeLists.txt` (add to `litepdf` exe sources)

**Step 1: Create header**

Create `src/ui/OutlinePane.hpp`:

```cpp
#pragma once

#include "core/Document.hpp"

#include <functional>
#include <memory>
#include <windows.h>

namespace litepdf::ui {

// OutlinePane wraps a Win32 TreeView (SysTreeView32) that displays
// PDF bookmark/outline entries. Hidden by default; MainWindow shows it
// when a document with outline entries is opened.
//
// Communication: on TVN_SELCHANGED the pane calls the on_navigate
// callback with the target page index (0-based). If the entry has
// no target page (kNoPage), the callback is not fired.
class OutlinePane {
public:
    using NavigateCb = std::function<void(int page)>;

    OutlinePane(HINSTANCE hInstance, HWND parent);
    ~OutlinePane();

    OutlinePane(const OutlinePane&)            = delete;
    OutlinePane& operator=(const OutlinePane&) = delete;

    HWND hwnd() const;

    void set_on_navigate(NavigateCb cb);

    // Populate the tree from Document outline entries. Clears any
    // previous items. Handles nested items via OutlineEntry::depth.
    void populate(const std::vector<litepdf::core::Document::OutlineEntry>& entries);

    // Remove all tree items.
    void clear();

    void show();
    void hide();
    bool visible() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace litepdf::ui
```

**Step 2: Implement**

Create `src/ui/OutlinePane.cpp`:

```cpp
#include "ui/OutlinePane.hpp"

#include <commctrl.h>
#include <vector>

#pragma comment(lib, "comctl32.lib")

namespace litepdf::ui {

struct OutlinePane::Impl {
    HWND tree = nullptr;
    NavigateCb on_navigate;
    // Map from HTREEITEM → page_index for click-to-navigate.
    // We store page indices in the item's lParam.
    WNDPROC original_parent_proc = nullptr;
    HWND parent = nullptr;
};

// We subclass the parent to intercept WM_NOTIFY from the TreeView.
// This is simpler than creating a dedicated parent HWND.
// However, since MainWindow already handles WM_NOTIFY, we'll use
// a different approach: store a static map and forward from MainWindow.
//
// Actually, the simplest Win32 pattern: MainWindow forwards
// WM_NOTIFY to OutlinePane via a public method. No subclassing needed.

OutlinePane::OutlinePane(HINSTANCE hInstance, HWND parent)
    : impl_(std::make_unique<Impl>()) {
    impl_->parent = parent;

    // TreeView control — WS_CHILD, initially hidden.
    impl_->tree = CreateWindowExW(
        WS_EX_CLIENTEDGE,
        WC_TREEVIEWW,
        L"",
        WS_CHILD | TVS_HASLINES | TVS_HASBUTTONS | TVS_LINESATROOT
            | TVS_SHOWSELALWAYS | TVS_DISABLEDRAGDROP,
        0, 0, 250, 100,
        parent,
        nullptr,
        hInstance,
        nullptr);
}

OutlinePane::~OutlinePane() {
    if (impl_->tree) {
        DestroyWindow(impl_->tree);
        impl_->tree = nullptr;
    }
}

HWND OutlinePane::hwnd() const { return impl_->tree; }

void OutlinePane::set_on_navigate(NavigateCb cb) {
    impl_->on_navigate = std::move(cb);
}

void OutlinePane::populate(
        const std::vector<litepdf::core::Document::OutlineEntry>& entries) {
    if (!impl_->tree) return;
    TreeView_DeleteAllItems(impl_->tree);

    // Track parent HTREEITEM at each depth so we can build the tree
    // hierarchy from the flat list.
    std::vector<HTREEITEM> depth_stack;  // depth_stack[d] = parent at depth d

    for (const auto& e : entries) {
        TVINSERTSTRUCTW tvi = {};
        tvi.hInsertAfter = TVI_LAST;

        // Determine parent: depth 0 = root, depth N = child of depth N-1.
        if (e.depth == 0 || depth_stack.empty()) {
            tvi.hParent = TVI_ROOT;
        } else {
            int parent_depth = e.depth - 1;
            if (parent_depth < static_cast<int>(depth_stack.size())) {
                tvi.hParent = depth_stack[static_cast<std::size_t>(parent_depth)];
            } else {
                tvi.hParent = depth_stack.back();
            }
        }

        // Convert UTF-8 title to UTF-16 for the TreeView.
        int wlen = MultiByteToWideChar(CP_UTF8, 0,
                       e.title.c_str(), static_cast<int>(e.title.size()),
                       nullptr, 0);
        std::wstring wtitle(static_cast<std::size_t>(wlen), L'\0');
        MultiByteToWideChar(CP_UTF8, 0,
            e.title.c_str(), static_cast<int>(e.title.size()),
            wtitle.data(), wlen);

        tvi.item.mask    = TVIF_TEXT | TVIF_PARAM;
        tvi.item.pszText = const_cast<wchar_t*>(wtitle.c_str());
        // Store page_index in lParam. Use -1 for kNoPage.
        tvi.item.lParam  = (e.page_index == litepdf::core::Document::kNoPage)
                               ? static_cast<LPARAM>(-1)
                               : static_cast<LPARAM>(e.page_index);

        HTREEITEM item = TreeView_InsertItem(impl_->tree, &tvi);

        // Maintain depth_stack: ensure it has an entry for this depth.
        auto d = static_cast<std::size_t>(e.depth);
        if (d < depth_stack.size()) {
            depth_stack[d] = item;
            // Trim anything deeper — they're from a previous sibling branch.
            depth_stack.resize(d + 1);
        } else {
            // Fill gaps if the outline skips depths (shouldn't happen in
            // well-formed PDFs, but be defensive).
            while (depth_stack.size() < d) {
                depth_stack.push_back(item);
            }
            depth_stack.push_back(item);
        }
    }

    // Expand the first level so the outline is immediately useful.
    HTREEITEM root = TreeView_GetRoot(impl_->tree);
    while (root) {
        TreeView_Expand(impl_->tree, root, TVE_EXPAND);
        root = TreeView_GetNextSibling(impl_->tree, root);
    }
}

void OutlinePane::clear() {
    if (impl_->tree)
        TreeView_DeleteAllItems(impl_->tree);
}

void OutlinePane::show() {
    if (impl_->tree)
        ShowWindow(impl_->tree, SW_SHOW);
}

void OutlinePane::hide() {
    if (impl_->tree)
        ShowWindow(impl_->tree, SW_HIDE);
}

bool OutlinePane::visible() const {
    return impl_->tree && IsWindowVisible(impl_->tree);
}

}  // namespace litepdf::ui
```

**Step 3: Add to CMake**

In the `litepdf` executable target in `CMakeLists.txt`, add after `src/ui/PdfCanvas.cpp`:

```
    src/ui/OutlinePane.cpp
```

**Step 4: Build**

Run: `cmake --build build --config Release`
Expected: Compiles. OutlinePane is not yet wired to MainWindow.

**Step 5: Commit**

```bash
git add src/ui/OutlinePane.hpp src/ui/OutlinePane.cpp CMakeLists.txt
git commit -m "feat(ui): OutlinePane — TreeView wrapper for PDF bookmarks (Phase 4 D1/D8)"
```

---

### Task 4: Wire OutlinePane into MainWindow

**Files:**
- Modify: `src/ui/MainWindow.hpp`
- Modify: `src/ui/MainWindow.cpp`

This is the integration task: MainWindow creates an OutlinePane, handles WM_NOTIFY from the TreeView, lays out outline + canvas side-by-side, and toggles with F5.

**Step 1: Update MainWindow header**

Add to `MainWindow.hpp`:

```cpp
#include "ui/OutlinePane.hpp"
```

Add private members:

```cpp
    std::unique_ptr<OutlinePane> outline_;
    bool outline_visible_ = false;

    void on_layout();          // reposition canvas + outline for current state
    void toggle_outline();     // F5 handler
    void on_outline_navigate(int page);  // callback from OutlinePane
```

**Step 2: Implement layout + toggle + wiring**

Key changes in `MainWindow.cpp`:

- **WM_CREATE**: Create `OutlinePane` (hidden). Set `on_navigate` callback. Add F5 to accelerator table.
- **on_layout()**: If outline visible, place outline at (0,0,250,h) and canvas at (250,0,w-250,h). Otherwise canvas at (0,0,w,h). Called from `WM_SIZE` and `toggle_outline()`.
- **WM_SIZE**: Replace current `SetWindowPos(canvas_->hwnd()...)` with `on_layout()`.
- **WM_NOTIFY**: Check `NMHDR::hwndFrom == outline_->hwnd()`, code `TVN_SELCHANGEDW`. Extract `lParam` from `NMTREEVIEWW::itemNew`, call `on_outline_navigate(page)`.
- **WM_COMMAND IDM_VIEW_OUTLINE**: Call `toggle_outline()`.
- **WM_USER_OPEN_OK**: After setting up view, populate outline. If entries exist, show outline; else hide. Call `on_layout()`.
- **Accelerator**: Add `{ FVIRTKEY, VK_F5, IDM_VIEW_OUTLINE }`.

**Step 3: Build and manually test**

Run: `cmake --build build --config Release`
Manual: launch `litepdf.exe bookmarks.pdf` — outline should appear on the left. Click entries to navigate. Press F5 to toggle. Open `simple.pdf` — outline should hide.

**Step 4: Commit**

```bash
git add src/ui/MainWindow.hpp src/ui/MainWindow.cpp
git commit -m "feat(ui): wire OutlinePane into MainWindow — layout, F5 toggle, click-to-jump (Phase 4 D1/D2/D6/D8)"
```

---

### Task 5: Wire MRU into MainWindow

**Files:**
- Modify: `src/ui/MainWindow.hpp`
- Modify: `src/ui/MainWindow.cpp`

**Step 1: Add MRU member and menu logic**

Add to `MainWindow.hpp`:

```cpp
#include "core/MruList.hpp"
```

Add private member:

```cpp
    litepdf::core::MruList mru_;
```

**Step 2: Implement MRU wiring in MainWindow.cpp**

Key changes:

- **Constructor**: Call `mru_.load()`.
- **WM_INITMENUPOPUP**: If the popup is the File menu (position 0), rebuild MRU items:
  1. Remove existing `IDM_MRU_1`..`IDM_MRU_10` items.
  2. Find the position of `IDM_FILE_EXIT`.
  3. Insert MRU entries before Exit (after the separator), numbered `&1 filename`, `&2 filename`, etc.
- **WM_COMMAND for IDM_MRU_1..IDM_MRU_10**: Compute index = `id - IDM_MRU_1`. Get path from `mru_.entries()[index]`. Check if file exists:
  - Yes → `open_async(path)`.
  - No → `MessageBoxW` "file not found", `mru_.remove(index)`, `mru_.save()`.
- **open_async**: After kicking open, call `mru_.push(path.wstring())` + `mru_.save()`.
- **WM_DROPFILES**: After kicking open, same MRU push.

**Step 3: Build and manually test**

Run: `cmake --build build --config Release`
Manual: Open a PDF → check File menu for MRU entry. Close and relaunch → MRU should persist. Click MRU entry → opens file.

**Step 4: Commit**

```bash
git add src/ui/MainWindow.hpp src/ui/MainWindow.cpp
git commit -m "feat(ui): MRU in File menu — load/save/push on open, click to reopen (Phase 4 D4/D5)"
```

---

### Task 6: Integration polish + smoke test

**Files:**
- Modify: `scripts/smoke-test.ps1`

**Step 1: Extend smoke test**

Add to `scripts/smoke-test.ps1` after the existing fixture launch check:

- Launch with `bookmarks.pdf` fixture.
- Verify process exits cleanly (no crash).
- (Optional) Check window title contains "bookmarks".

**Step 2: Run full test suite**

Run: `cmake --build build --config Release && ctest --test-dir build -C Release`
Expected: All 64+ tests pass (59 old + 5 MRU).

**Step 3: Run smoke test**

Run: `powershell -File scripts/smoke-test.ps1`
Expected: PASS.

**Step 4: Commit**

```bash
git add scripts/smoke-test.ps1
git commit -m "test(smoke): extend smoke test for bookmarks.pdf outline fixture (Phase 4)"
```

---

### Task 7: Tag v0.0.5-phase4

**Step 1: Verify everything**

Run all tests + smoke test one more time. Verify clean `git status`.

**Step 2: Tag**

```bash
git tag v0.0.5-phase4
```

---

## Summary

| Task | What | New Tests | Commit |
|------|------|-----------|--------|
| 0 | DocumentView::document() accessor | 0 | `feat(core)` |
| 1 | MruList (TDD) | +5 | `feat(core)` |
| 2 | Menu IDs + resources | 0 | `feat(ui)` |
| 3 | OutlinePane (TreeView wrapper) | 0 | `feat(ui)` |
| 4 | Wire OutlinePane into MainWindow | 0 (manual) | `feat(ui)` |
| 5 | Wire MRU into MainWindow | 0 (manual) | `feat(ui)` |
| 6 | Integration + smoke test | 0 (smoke) | `test(smoke)` |
| 7 | Tag v0.0.5-phase4 | — | tag |

**Total: 7 tasks, ~400 LOC, 5 new tests → 64 total.**
