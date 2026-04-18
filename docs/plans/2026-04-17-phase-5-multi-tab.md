# Phase 5: Multi-Tab + Single-Instance IPC — Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use `superpowers:subagent-driven-development` to execute this plan task-by-task (same pattern as Phases 2–4: implementer → spec reviewer → quality reviewer → fix).

**Goal:** Open more than one document at a time in a single `litepdf.exe` instance. Each tab owns an independent `DocumentView` (and therefore its own `Document`, `RenderEngine`, `PageCache`, zoom/scroll/current-page state). Launching a second `litepdf.exe <path>` while a first is running routes the file to the running instance as a new tab instead of opening a second process.

**Architecture:** A new `TabList` class holds the pure-logic tab collection (`vector<unique_ptr<Tab>>` + active index) and is fully unit-testable without Win32. `TabManager` wraps a Win32 `SysTabControl32` HWND, owns the `TabList`, and emits a callback on `TCN_SELCHANGE`. `MainWindow` stacks TabManager above the existing outline/canvas row; on tab switch it snapshots the outgoing tab's canvas pan + outline visibility, restores the incoming tab's snapshot, re-points `PdfCanvas::set_view()` to the new `DocumentView`, and kicks a render. A named `Local\\` mutex gates single-instance behavior; if a first instance already owns the mutex, the second sends its command-line path via `WM_COPYDATA` and exits.

**Tech Stack:** C++17, Win32 `SysTabControl32` (comctl32 — already linked), `CreateMutexW` + `WM_COPYDATA` for single-instance IPC, existing Catch2/CMake from Phases 0–4.

**Prerequisites (from Phase 4 + patch):**

- Tag `v0.0.5-phase4.1` on `main`; CI green on `windows-latest`.
- `DocumentView` owns per-tab `Document` + `RenderEngine` + `PageCache` + UI-thread `fz_context` clone (D3 from Phase 3). Multiple concurrent `DocumentView` instances are already safe by construction.
- `OutlinePane` is a single HWND whose content is populated per-document; it is reused across tabs (repopulated on switch).
- `MainWindow` owns a single `std::unique_ptr<DocumentView> view_`; replacing this with an active-tab accessor is the core refactor.
- `PdfCanvas` keeps non-owning `DocumentView* view` + a clone-of-UI-ctx `orphan_ctx` for late-arriving pixmap drops. `set_view()` already handles the swap correctly — we just need to call it more often.
- 64/64 ctest pass.

**Done when:**

1. File → Open or drag-drop a second file creates a second tab; the first tab and its state (page, zoom, scroll, outline) are preserved.
2. Clicking a tab header switches the canvas + outline to that tab's document. Each tab remembers its own `current_page`, zoom mode + scale, canvas pan, and outline visibility across switches.
3. `Ctrl+Tab` cycles forward, `Ctrl+Shift+Tab` cycles backward, `Ctrl+1`…`Ctrl+9` jump to tab N (1-indexed).
4. `Ctrl+W` closes the active tab; middle-click on a tab header closes that tab.
5. Closing a non-last tab activates the right neighbor (the tab now at the same index). Closing the active last tab activates the new last tab (left neighbor). Closing the only tab leaves the window blank and the title reverts to `"LitePDF"`.
6. No cross-tab render bleed: rapidly switching while renders are in flight never paints tab A's bitmap on tab B's canvas.
7. Launching a second `litepdf.exe <path>` while a first is running results in a single process with two tabs; the second process exits with code 0 and no window is created.
8. Launching a second `litepdf.exe` with no path while a first is running brings the first instance's window to the foreground and exits.
9. `ctest --test-dir build -C Release` passes all Phase 4 tests (no regression) plus new `TabList` unit tests (≥ 6 cases, 70+ total).
10. `scripts/smoke-test.ps1` extended: launch with 2 args, verify both tabs visible; a follow-up forward via `WM_COPYDATA` brings a third tab.
11. `scripts/ux-probe.ps1` extended to enumerate the tab control and report active index + titles.
12. Tag `v0.0.6-phase5` pushed.

**Learnings carried from Phase 4:**

- **Menu dynamic regions use `MF_BYCOMMAND` anchors with SEP bracketing.** We won't add tab-related items to the main menu, so this doesn't recur, but when keyboard-cycling shortcuts are advertised (e.g., "Next Tab\tCtrl+Tab") the literal in `.rc` and the accelerator table must match.
- **Non-throwing `std::filesystem` on UI thread.** MRU-click used `std::filesystem::exists(p, ec)` to collapse removable-drive errors. We keep the same discipline for any path checks added in IPC forwarding.
- **Null-terminate registry + IPC buffers.** `MruList::load()` had a null-term bug; `WM_COPYDATA` receivers must also validate that the byte buffer ends in `L'\0'` before constructing a `wstring`.
- **PIMPL + `std::call_once` for window class registration.** `TabManager` follows `PdfCanvas` / `OutlinePane` style — PIMPL in `.cpp`, no exposure of tab-control internals in the header.
- **Test keys isolated for registry-touching tests.** If any new test touches registry (none expected in Phase 5), isolate under `HKCU\\SOFTWARE\\LitePDF\\TestXXX`.

---

## Architectural Decisions

Pinned before tasks; revisit only if a task uncovers a blocker.

**D1. Tab strip = horizontal `SysTabControl32` across the top of the client area, below the menu.** Height follows the control's own `TCM_ADJUSTRECT` calculation (typically ~24 DIP at 96 DPI, scales with DPI). `on_layout()` subtracts the tab strip's height from the outline/canvas row. Tab strip is hidden when `tab_count() == 0` to preserve the empty-window look.

**D2. Per-tab state holder = `Tab` struct owned by `TabList`.** Fields:

```cpp
struct Tab {
    std::unique_ptr<core::DocumentView> view;
    std::wstring                        label;            // filename, for TabCtrl text
    std::filesystem::path               path;             // for tooltip + Win title
    // UI state snapshot saved on deactivation, restored on activation:
    float  pan_x          = 0.0f;
    float  pan_y          = 0.0f;
    bool   outline_visible = false;
    // current_page, zoom_mode, zoom_scale already live inside DocumentView.
};
```

Scroll/pan lives in `PdfCanvas::Impl` (floats in DIPs). We don't move it into `DocumentView` (canvas-specific, not core-model state) — instead `PdfCanvas` gains `pan()` and `set_pan()` accessors and `MainWindow` shuttles the pair into/out of the active `Tab` on switch.

**D3. TabList is a pure-logic, unit-testable class in `core/`.** No Win32 types in its header. API:

```cpp
class TabList {
public:
    std::size_t size() const noexcept;
    bool        empty() const noexcept;
    int         active_index() const noexcept;   // -1 when empty
    Tab*        active() noexcept;               // nullptr when empty
    Tab*        at(std::size_t i) noexcept;      // nullptr if out of range

    // Appends at the end; returns new tab's index. Does NOT auto-activate.
    std::size_t add(std::unique_ptr<Tab> t);

    // Removes tab at i; returns the new active index (or -1 if now empty).
    // If i == active, the new active is min(i, size()-1) after removal
    // (i.e., activate the right neighbor, or the last tab if i was last).
    int         remove(std::size_t i);

    // Sets active; returns false if i out of range.
    bool        set_active(std::size_t i) noexcept;
};
```

This is the single most-testable surface in Phase 5 and the canonical place for TDD.

**D4. Activation policy on close: right-neighbor by default, left-neighbor only when removing the last tab.** Matches Chrome, Edge, and SumatraPDF. Example with three tabs A/B/C active=B: closing B → activate C. Closing C (last) → activate B.

**D5. TabManager owns the TabList; MainWindow drives it via a thin API.** `TabManager` wraps the HWND and exposes:

```cpp
class TabManager {
public:
    using SwitchCb = std::function<void(int new_index, int old_index)>;
    using CloseCb  = std::function<void(int index)>;

    TabManager(HINSTANCE, HWND parent);
    HWND hwnd() const;

    int  add_tab(std::unique_ptr<core::Tab> t);     // returns new index, activates
    void close_tab(int index);                       // fires CloseCb if the host wants confirmation; see D9
    int  active_index() const;
    core::Tab* active_tab();
    core::Tab* tab_at(int index);
    std::size_t count() const;
    bool set_active(int index);                      // updates TCM_SETCURSEL + fires SwitchCb

    void set_on_switch(SwitchCb cb);
    void set_on_close_request(CloseCb cb);

    // Per-tab label updates (e.g., if filename is truncated).
    void set_tab_label(int index, const std::wstring& label);

    // WM_NOTIFY (TCN_SELCHANGING/TCN_SELCHANGE) and mouse routing are
    // handled by TabManager's own subclass or by parent forwarding —
    // see D6.
};
```

**D6. Tab strip receives mouse input via parent WM_NOTIFY + explicit middle-click subclassing.** `SysTabControl32` fires `TCN_SELCHANGE` (via `WM_NOTIFY` to parent). Middle-click close requires `WM_MBUTTONUP` handling, which `TabManager` implements by `SetWindowSubclass`-ing the tab HWND (standard Win32 pattern). The subclass converts `WM_MBUTTONUP` + `TabCtrl_HitTest` → `CloseCb(hit_index)`.

**D7. Tab switching pipeline (on `TCN_SELCHANGE`):**

```
1. outgoing = list.active();
2. if (outgoing) {
       auto [px, py] = canvas_->pan();
       outgoing->pan_x = px;
       outgoing->pan_y = py;
       outgoing->outline_visible = outline_->visible();
   }
3. list.set_active(new_index);
4. incoming = list.active();
5. canvas_->set_view(incoming ? incoming->view.get() : nullptr);
6. canvas_->set_pan(incoming->pan_x, incoming->pan_y);
7. outline_->clear();
   if (incoming) {
       const auto& entries = incoming->view->document().outline();
       if (!entries.empty() && incoming->outline_visible) {
           outline_->populate(entries);
           outline_->show();
       } else {
           outline_->hide();
       }
   }
8. on_layout();
9. update_window_title();
10. if (incoming) kick_render(incoming->view->current_page());
    else invalidate canvas (paints blank);
```

Rendering the incoming tab from scratch is acceptable — the old L1 bitmap was owned by the outgoing view. A brief blank frame is fine; prefetched L2 display lists in that view's PageCache will make the next kick fast.

**D8. Canvas state on blank-view (no active tab):** `PdfCanvas::set_view(nullptr)` already clears `current_bitmap` and `InvalidateRect`s. Pan is reset on next `set_view` via `set_pan(0,0)` from the switch pipeline, which is what we want for "first activation of a fresh tab". For blank-window state we call `set_view(nullptr)` and also `set_pan(0,0)` for hygiene. The tab strip hides itself in `on_layout()` when `count() == 0`.

**D9. Close semantics: no confirmation dialog in Phase 5.** Closing a tab is immediate. SumatraPDF / Chrome / Edge all behave this way. Session restore for the "oops I closed" case lands in Phase 12 via `session.json`.

**D10. New tab activation policy = activate on open.** Every new open (File→Open, drag-drop, MRU-click, IPC-forward) calls `open_async(path)` which, on `WM_USER_OPEN_OK`, appends a new tab and switches to it. The user-visible behavior matches Phase 4 ("the file I just opened is what I'm looking at"); difference is the previously open doc stays alive as a sibling tab instead of being destroyed.

**D11. `open_async` epoch bookkeeping moves to per-request.** Current `open_epoch_` discards a stale open when a newer one supersedes. In multi-tab, every accepted open becomes its own tab — there's no "supersede", only "append". Remove the stale-epoch check at `WM_USER_OPEN_OK` and let every successful open append unconditionally. `WM_USER_OPEN_FAILED` still needs the request's payload (error code) but no stale-discard logic.

  - Edge case: user clicks File→Open, cancels via ESC — no harm, no open_async fired.
  - Edge case: user clicks File→Open twice in quick succession — two async opens both run; both eventually append tabs. This is fine and matches user intent.

**D12. Single-instance mutex = `Local\\LitePDF_SingleInstance_v1`.** Per-user session scope (not `Global\\`); this matches per-user install expectations from design §1.1 and §8.1. The `_v1` suffix gives us a clean way to break the contract if we ever need to (e.g., if the IPC protocol changes incompatibly).

**D13. IPC protocol = `WM_COPYDATA` with `dwData = 'LPDF'` (0x4C504446) and payload = UTF-16 null-terminated path.** Receiver validates: `dwData == 0x4C504446`, `cbData >= sizeof(wchar_t)`, `cbData <= 64 * 1024`, last `wchar_t` is `L'\0'`. On success: `open_async(path)`. On any validation failure: silently ignore (never MessageBox — a malicious/broken sender must not be able to pop dialogs).

**D14. IPC forwarding sender uses `FindWindowW(kWindowClassName, nullptr)` + `SendMessageTimeoutW` with `SMTO_ABORTIFHUNG` and a 3-second timeout.** If `SendMessageTimeoutW` fails (server hung, or window destroyed mid-forward), the second instance exits with code 1 — we do NOT fall through to "become a normal instance" because two competing mutex holders is worse than a dropped file.

**D15. No path is brought to the foreground unless the user ran a second instance.** If the first instance is currently minimized and a user double-clicks a .pdf from Explorer, the forward should `SetForegroundWindow` + `ShowWindow(SW_RESTORE)` as part of the WM_COPYDATA handler. If the second instance had no path (user re-double-clicked the .exe itself), send a different IPC message: `dwData = 'LPDB'` (bring-to-front), empty payload. Receiver handles both.

**D16. Known Phase 5 limitation: per-tab render-pool cost.** Each `DocumentView` constructs its own `RenderEngine` with `num_workers = 2`. Five tabs = 10 worker threads. Design §5.3 implies a single shared pool, but shared-pool requires a per-request `fz_context*` strategy that would touch the Phase 2 engine core. **Defer shared pool to Phase 11** (benchmark regression gate will surface the actual cost). Add a brief note in `docs/plans/2026-04-15-litepdf-roadmap.md` §"What Each Phase Costs" or a new §"Known Limitations" captured after Phase 5.

---

## Task List

### Task 0: Reserve resource IDs + accelerators

**Files:**
- Modify: `resources/MainMenu.rc.h`

**Step 1: Add tab command IDs**

Append to `MainMenu.rc.h`, keeping the numbering convention from Phase 4 (MRU sits at 40020-40029, so tabs land at 40030+):

```cpp
// Phase 5: tab management. Added as accelerators only — not in the File menu
// popup, to keep the empty-MRU layout from Phase 4 unchanged.
#define IDM_TAB_CLOSE    40030   // Ctrl+W
#define IDM_TAB_NEXT     40031   // Ctrl+Tab
#define IDM_TAB_PREV     40032   // Ctrl+Shift+Tab
// Ctrl+1..9 jump to tab 1..9 (range 40033-40041, 1-indexed in user UI).
#define IDM_TAB_GOTO_1   40033
#define IDM_TAB_GOTO_2   40034
#define IDM_TAB_GOTO_3   40035
#define IDM_TAB_GOTO_4   40036
#define IDM_TAB_GOTO_5   40037
#define IDM_TAB_GOTO_6   40038
#define IDM_TAB_GOTO_7   40039
#define IDM_TAB_GOTO_8   40040
#define IDM_TAB_GOTO_9   40041
```

**Step 2: No menu changes**

We deliberately do NOT add these to the File popup — per D1 keyboard-only to avoid menu bloat. The MRU separator bracketing (Phase 4 polish) stays correct.

**Step 3: Build sanity-check**

Run: `cmake --build build --config Release`
Expected: Green. (No new code yet; just resource header additions.)

**Step 4: Commit**

```bash
git add resources/MainMenu.rc.h
git commit -m "feat(ui): reserve menu IDs for tab close/next/prev/goto (Phase 5 Task 0)"
```

---

### Task 1: TabList — pure-logic tab collection (TDD)

**Files:**
- Create: `src/core/TabList.hpp`
- Create: `src/core/TabList.cpp`
- Create: `tests/unit/test_tab_list.cpp`
- Modify: `CMakeLists.txt` (add `TabList.cpp` to `litepdf_core`)
- Modify: `tests/CMakeLists.txt` (add `test_tab_list.cpp`)

**Step 1: Write tests first**

Create `tests/unit/test_tab_list.cpp`:

```cpp
#include "core/TabList.hpp"

#include <catch2/catch_test_macros.hpp>

#include <memory>

using namespace litepdf::core;

namespace {
// Test helper: build a Tab with just a label; DocumentView is left null
// because TabList is pure-logic and does not dereference view.
std::unique_ptr<Tab> make_tab(std::wstring label) {
    auto t = std::make_unique<Tab>();
    t->label = std::move(label);
    return t;
}
}  // namespace

TEST_CASE("TabList starts empty", "[tablist]") {
    TabList list;
    REQUIRE(list.size() == 0);
    REQUIRE(list.empty());
    REQUIRE(list.active_index() == -1);
    REQUIRE(list.active() == nullptr);
}

TEST_CASE("TabList add appends and returns index", "[tablist]") {
    TabList list;
    REQUIRE(list.add(make_tab(L"a")) == 0);
    REQUIRE(list.add(make_tab(L"b")) == 1);
    REQUIRE(list.add(make_tab(L"c")) == 2);
    REQUIRE(list.size() == 3);
    // add() does NOT auto-activate (caller decides).
    REQUIRE(list.active_index() == -1);
}

TEST_CASE("TabList set_active updates active pointer", "[tablist]") {
    TabList list;
    list.add(make_tab(L"a"));
    list.add(make_tab(L"b"));
    REQUIRE(list.set_active(1));
    REQUIRE(list.active_index() == 1);
    REQUIRE(list.active()->label == L"b");
    REQUIRE_FALSE(list.set_active(5));  // out of range
    REQUIRE(list.active_index() == 1);  // unchanged
}

TEST_CASE("TabList remove: right-neighbor activation", "[tablist]") {
    // A, B, C — active=1 (B). Remove B → active should be C (now at idx 1).
    TabList list;
    list.add(make_tab(L"a"));
    list.add(make_tab(L"b"));
    list.add(make_tab(L"c"));
    list.set_active(1);
    int new_active = list.remove(1);
    REQUIRE(new_active == 1);
    REQUIRE(list.size() == 2);
    REQUIRE(list.active()->label == L"c");
}

TEST_CASE("TabList remove last: left-neighbor activation", "[tablist]") {
    // A, B — active=1 (B, the last). Remove B → active should be A.
    TabList list;
    list.add(make_tab(L"a"));
    list.add(make_tab(L"b"));
    list.set_active(1);
    int new_active = list.remove(1);
    REQUIRE(new_active == 0);
    REQUIRE(list.active()->label == L"a");
}

TEST_CASE("TabList remove non-active shifts indices", "[tablist]") {
    // A, B, C — active=2 (C). Remove A → C is now at idx 1, active should
    // stay on C.
    TabList list;
    list.add(make_tab(L"a"));
    list.add(make_tab(L"b"));
    list.add(make_tab(L"c"));
    list.set_active(2);
    int new_active = list.remove(0);
    REQUIRE(new_active == 1);
    REQUIRE(list.active()->label == L"c");
}

TEST_CASE("TabList remove last remaining tab resets active", "[tablist]") {
    TabList list;
    list.add(make_tab(L"solo"));
    list.set_active(0);
    int new_active = list.remove(0);
    REQUIRE(new_active == -1);
    REQUIRE(list.empty());
    REQUIRE(list.active() == nullptr);
}

TEST_CASE("TabList at() bounds-checks", "[tablist]") {
    TabList list;
    list.add(make_tab(L"a"));
    REQUIRE(list.at(0) != nullptr);
    REQUIRE(list.at(1) == nullptr);  // one past end
    REQUIRE(list.at(std::size_t(-1)) == nullptr);  // wrap-around
}
```

**Step 2: Create the TabList header**

Create `src/core/TabList.hpp`:

```cpp
#pragma once

// core::TabList — pure-logic tab collection used by ui::TabManager.
// No Win32 types in this header so the class can be unit-tested without
// HWND creation. Manages ownership of Tab structs and the active index.
//
// Activation policy on remove (D4 from Phase 5 plan):
//   - Removing a non-active tab shifts indices; active stays on the same
//     Tab instance (its index may decrement).
//   - Removing the active non-last tab activates the right neighbor.
//   - Removing the active last tab activates the new last tab (left).
//   - Removing the final remaining tab leaves the list empty (active_index=-1).

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace litepdf::core {

class DocumentView;

struct Tab {
    std::unique_ptr<DocumentView> view;
    std::wstring                  label;
    std::filesystem::path         path;

    // UI-state snapshot saved on deactivation, restored on activation.
    // pan_x/pan_y are in DIPs, matching PdfCanvas::Impl pan state.
    float pan_x            = 0.0f;
    float pan_y            = 0.0f;
    bool  outline_visible  = false;

    Tab()  = default;
    ~Tab();  // out-of-line so DocumentView can stay forward-declared here.

    Tab(const Tab&)            = delete;
    Tab& operator=(const Tab&) = delete;
};

class TabList {
public:
    TabList()  = default;
    ~TabList() = default;

    TabList(const TabList&)            = delete;
    TabList& operator=(const TabList&) = delete;

    std::size_t size()  const noexcept { return tabs_.size(); }
    bool        empty() const noexcept { return tabs_.empty(); }
    int         active_index() const noexcept { return active_; }

    Tab* active() noexcept;
    Tab* at(std::size_t i) noexcept;

    // Appends at the end; returns the new tab's index. Does NOT activate.
    std::size_t add(std::unique_ptr<Tab> t);

    // Removes tab at i. Returns the new active_index() (may be -1 if
    // the list is now empty). Out-of-range i is a no-op returning the
    // current active_index().
    int remove(std::size_t i);

    // Sets active; returns false (and does nothing) if i is out of range.
    bool set_active(std::size_t i) noexcept;

private:
    std::vector<std::unique_ptr<Tab>> tabs_;
    int                               active_ = -1;
};

}  // namespace litepdf::core
```

**Step 3: Implement the TabList**

Create `src/core/TabList.cpp`:

```cpp
#include "core/TabList.hpp"

#include "core/DocumentView.hpp"

namespace litepdf::core {

// Defined out-of-line so DocumentView can be a forward-decl in the header.
// The unique_ptr<DocumentView> member needs the full type here to destroy.
Tab::~Tab() = default;

Tab* TabList::active() noexcept {
    if (active_ < 0 || static_cast<std::size_t>(active_) >= tabs_.size()) {
        return nullptr;
    }
    return tabs_[static_cast<std::size_t>(active_)].get();
}

Tab* TabList::at(std::size_t i) noexcept {
    return (i < tabs_.size()) ? tabs_[i].get() : nullptr;
}

std::size_t TabList::add(std::unique_ptr<Tab> t) {
    tabs_.push_back(std::move(t));
    return tabs_.size() - 1;
}

int TabList::remove(std::size_t i) {
    if (i >= tabs_.size()) return active_;

    const bool removing_active = (static_cast<int>(i) == active_);
    tabs_.erase(tabs_.begin() + static_cast<std::ptrdiff_t>(i));

    if (tabs_.empty()) {
        active_ = -1;
    } else if (removing_active) {
        // Right-neighbor default; clamp to last tab if we removed the end.
        active_ = static_cast<int>(std::min(i, tabs_.size() - 1));
    } else if (static_cast<std::size_t>(active_) > i) {
        // Active tab was to the right of the removed one; shift down.
        --active_;
    }
    return active_;
}

bool TabList::set_active(std::size_t i) noexcept {
    if (i >= tabs_.size()) return false;
    active_ = static_cast<int>(i);
    return true;
}

}  // namespace litepdf::core
```

**Step 4: Wire into build**

In `CMakeLists.txt`, add `src/core/TabList.cpp` to the `litepdf_core` sources list (alphabetical order, after `PageCache.cpp`):

```cmake
add_library(litepdf_core STATIC
    src/core/Document.cpp
    src/core/DocumentView.cpp
    src/core/MruList.cpp
    src/core/PageCache.cpp
    src/core/RenderEngine.cpp
    src/core/TabList.cpp
)
```

In `tests/CMakeLists.txt`, add `test_tab_list.cpp` to `litepdf_unit_tests` (alphabetical):

```cmake
add_executable(litepdf_unit_tests
    # ...
    unit/test_tab_list.cpp
    # ...
)
```

**Step 5: Run the new tests**

Run:
```
cmake --build build --config Release
ctest --test-dir build -C Release -R tablist --output-on-failure
```
Expected: 8 cases pass.

**Step 6: Run the full suite to confirm no regression**

Run: `ctest --test-dir build -C Release`
Expected: 72/72 pass (64 Phase 4 + 8 new).

**Step 7: Commit**

```bash
git add src/core/TabList.hpp src/core/TabList.cpp tests/unit/test_tab_list.cpp \
        CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat(core): TabList — pure-logic tab collection with TDD (Phase 5 D3/D4)"
```

---

### Task 2: PdfCanvas pan accessors

**Files:**
- Modify: `src/ui/PdfCanvas.hpp`
- Modify: `src/ui/PdfCanvas.cpp`

**Rationale.** `MainWindow` needs to snapshot the outgoing tab's canvas pan and restore the incoming tab's pan on tab switch (D7). `pan_x / pan_y` currently live inside `PdfCanvas::Impl` with no accessors.

**Step 1: Add accessors to the header**

In `src/ui/PdfCanvas.hpp`, below `set_view`:

```cpp
    // Get/set the canvas pan offset (DIPs from the centered/fit origin).
    // Used by MainWindow to snapshot/restore per-tab scroll on tab switch.
    // Both are no-ops if called before the impl is ready.
    struct Pan { float x; float y; };
    Pan  pan() const;
    void set_pan(float x, float y);
```

**Step 2: Implement in .cpp**

In `src/ui/PdfCanvas.cpp`, near the existing `set_view()`:

```cpp
PdfCanvas::Pan PdfCanvas::pan() const {
    if (!impl_) return { 0.0f, 0.0f };
    return { impl_->pan_x, impl_->pan_y };
}

void PdfCanvas::set_pan(float x, float y) {
    if (!impl_) return;
    impl_->pan_x = x;
    impl_->pan_y = y;
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}
```

**Step 3: Build and test**

Run: `cmake --build build --config Release && ctest --test-dir build -C Release`
Expected: 72/72 pass. (No new tests — the accessors are exercised by Phase 5 tab-switch integration, not unit-testable without Direct2D.)

**Step 4: Commit**

```bash
git add src/ui/PdfCanvas.hpp src/ui/PdfCanvas.cpp
git commit -m "feat(ui): PdfCanvas pan() / set_pan() accessors for per-tab scroll snapshots (Phase 5 Task 2)"
```

---

### Task 3: TabManager — Win32 tab-control wrapper

**Files:**
- Create: `src/ui/TabManager.hpp`
- Create: `src/ui/TabManager.cpp`
- Modify: `CMakeLists.txt` (add `TabManager.cpp` to `litepdf` executable)

**Step 1: Create the header**

Create `src/ui/TabManager.hpp`:

```cpp
#pragma once

// ui::TabManager — Win32 SysTabControl32 wrapper owning a core::TabList.
// Header stays PIMPL-clean; the only Win32 types exposed are HWND/HINSTANCE
// which appear throughout the UI layer already.

#include "core/TabList.hpp"

#include <functional>
#include <memory>
#include <windows.h>

namespace litepdf::ui {

class TabManager {
public:
    using SwitchCb       = std::function<void(int new_index, int old_index)>;
    using CloseRequestCb = std::function<void(int index)>;

    TabManager(HINSTANCE hInstance, HWND parent);
    ~TabManager();

    TabManager(const TabManager&)            = delete;
    TabManager& operator=(const TabManager&) = delete;

    HWND hwnd() const;

    // Append a tab and activate it. Returns the new index.
    int add_tab(std::unique_ptr<litepdf::core::Tab> t);

    // Remove the tab at index. The active-index policy from TabList
    // decides the next active tab; SwitchCb fires if the active tab
    // changed as a result.
    void close_tab(int index);

    int                     count() const;
    int                     active_index() const;
    litepdf::core::Tab*     active_tab();
    litepdf::core::Tab*     tab_at(int index);

    // Programmatic activate (e.g. Ctrl+1..9). Fires SwitchCb.
    bool set_active(int index);

    // Update the label shown in the tab header.
    void set_tab_label(int index, const std::wstring& label);

    void set_on_switch(SwitchCb cb);
    void set_on_close_request(CloseRequestCb cb);

    // Parent's WM_NOTIFY for TCN_SELCHANGE routes here so TabManager
    // can emit SwitchCb. Returns true if the notification was handled.
    bool handle_notify(const NMHDR* hdr);

    // Reserve vertical space: writes tab-strip height into *h_out in
    // pixels for the given DPI. Used by MainWindow::on_layout().
    int  strip_height(UINT dpi) const;

    // Show / hide the tab strip (hidden when count()==0 per D1).
    void set_visible(bool v);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace litepdf::ui
```

**Step 2: Implement TabManager**

Create `src/ui/TabManager.cpp`. Important parts:

```cpp
#include "ui/TabManager.hpp"

#include <commctrl.h>
#include <algorithm>
#include <utility>

namespace litepdf::ui {

namespace {
constexpr UINT_PTR kTabSubclassId = 0xAB01;

LRESULT CALLBACK tab_subclass_proc(HWND hwnd, UINT msg, WPARAM w, LPARAM l,
                                   UINT_PTR id, DWORD_PTR ref_data);
}  // namespace

struct TabManager::Impl {
    HWND                        hwnd = nullptr;
    litepdf::core::TabList      list;
    TabManager::SwitchCb        on_switch;
    TabManager::CloseRequestCb  on_close_request;
};

TabManager::TabManager(HINSTANCE hInstance, HWND parent)
    : impl_(std::make_unique<Impl>())
{
    impl_->hwnd = CreateWindowExW(
        0, WC_TABCONTROLW, L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | TCS_FOCUSNEVER,
        0, 0, 0, 0,
        parent, nullptr, hInstance, nullptr);
    // Subclass so we can catch WM_MBUTTONUP (middle-click close). Pass
    // `this` via ref_data so the subclass proc can route back.
    SetWindowSubclass(impl_->hwnd, tab_subclass_proc,
                      kTabSubclassId, reinterpret_cast<DWORD_PTR>(this));
}

TabManager::~TabManager() {
    if (impl_ && impl_->hwnd) {
        RemoveWindowSubclass(impl_->hwnd, tab_subclass_proc, kTabSubclassId);
        DestroyWindow(impl_->hwnd);
    }
}

HWND TabManager::hwnd() const { return impl_ ? impl_->hwnd : nullptr; }
int  TabManager::count()        const { return impl_ ? static_cast<int>(impl_->list.size()) : 0; }
int  TabManager::active_index() const { return impl_ ? impl_->list.active_index() : -1; }

litepdf::core::Tab* TabManager::active_tab() {
    return impl_ ? impl_->list.active() : nullptr;
}
litepdf::core::Tab* TabManager::tab_at(int index) {
    if (!impl_ || index < 0) return nullptr;
    return impl_->list.at(static_cast<std::size_t>(index));
}

int TabManager::add_tab(std::unique_ptr<litepdf::core::Tab> t) {
    // Insert into the Win32 control first so TCM_SETCURSEL can land.
    TCITEMW tci = {};
    tci.mask    = TCIF_TEXT;
    // label is const during the SendMessage lifetime.
    tci.pszText = const_cast<LPWSTR>(t->label.c_str());

    const int new_index = static_cast<int>(impl_->list.add(std::move(t)));
    SendMessageW(impl_->hwnd, TCM_INSERTITEMW, new_index,
                 reinterpret_cast<LPARAM>(&tci));

    const int old_active = impl_->list.active_index();
    impl_->list.set_active(static_cast<std::size_t>(new_index));
    SendMessageW(impl_->hwnd, TCM_SETCURSEL, new_index, 0);
    if (impl_->on_switch) impl_->on_switch(new_index, old_active);
    return new_index;
}

void TabManager::close_tab(int index) {
    if (index < 0 || index >= count()) return;
    const int old_active = impl_->list.active_index();
    SendMessageW(impl_->hwnd, TCM_DELETEITEM, index, 0);
    const int new_active = impl_->list.remove(static_cast<std::size_t>(index));
    if (new_active >= 0) {
        SendMessageW(impl_->hwnd, TCM_SETCURSEL, new_active, 0);
    }
    if (new_active != old_active && impl_->on_switch) {
        impl_->on_switch(new_active, old_active);
    }
}

bool TabManager::set_active(int index) {
    if (index < 0) return false;
    if (!impl_->list.set_active(static_cast<std::size_t>(index))) return false;
    const int old_active = impl_->list.active_index();
    SendMessageW(impl_->hwnd, TCM_SETCURSEL, index, 0);
    if (impl_->on_switch) impl_->on_switch(index, old_active);
    return true;
}

void TabManager::set_tab_label(int index, const std::wstring& label) {
    if (index < 0 || index >= count()) return;
    if (auto* t = impl_->list.at(static_cast<std::size_t>(index))) {
        t->label = label;
    }
    TCITEMW tci = {};
    tci.mask    = TCIF_TEXT;
    tci.pszText = const_cast<LPWSTR>(label.c_str());
    SendMessageW(impl_->hwnd, TCM_SETITEMW, index, reinterpret_cast<LPARAM>(&tci));
}

void TabManager::set_on_switch(SwitchCb cb) {
    impl_->on_switch = std::move(cb);
}
void TabManager::set_on_close_request(CloseRequestCb cb) {
    impl_->on_close_request = std::move(cb);
}

bool TabManager::handle_notify(const NMHDR* hdr) {
    if (!hdr || hdr->hwndFrom != impl_->hwnd) return false;
    if (hdr->code == TCN_SELCHANGE) {
        const int new_index = static_cast<int>(
            SendMessageW(impl_->hwnd, TCM_GETCURSEL, 0, 0));
        const int old_active = impl_->list.active_index();
        if (new_index >= 0 && new_index != old_active) {
            impl_->list.set_active(static_cast<std::size_t>(new_index));
            if (impl_->on_switch) impl_->on_switch(new_index, old_active);
        }
        return true;
    }
    return false;
}

int TabManager::strip_height(UINT dpi) const {
    // TabCtrl doesn't expose a clean "natural height"; we probe with
    // TCM_ADJUSTRECT on a dummy rect scaled for the requested DPI.
    // 96 DPI baseline ≈ 24 px tab strip.
    const int baseline = 24;
    return MulDiv(baseline, static_cast<int>(dpi), 96);
}

void TabManager::set_visible(bool v) {
    if (impl_ && impl_->hwnd) {
        ShowWindow(impl_->hwnd, v ? SW_SHOW : SW_HIDE);
    }
}

namespace {
LRESULT CALLBACK tab_subclass_proc(HWND hwnd, UINT msg, WPARAM w, LPARAM l,
                                   UINT_PTR id, DWORD_PTR ref_data) {
    auto* self = reinterpret_cast<TabManager*>(ref_data);
    if (msg == WM_MBUTTONUP && self) {
        TCHITTESTINFO hti = {};
        hti.pt.x = GET_X_LPARAM(l);
        hti.pt.y = GET_Y_LPARAM(l);
        const int hit = static_cast<int>(
            SendMessageW(hwnd, TCM_HITTEST, 0, reinterpret_cast<LPARAM>(&hti)));
        if (hit >= 0) {
            // Fire the close-request callback via the public API if set.
            // TabManager doesn't expose the Impl here, so we reach the
            // callback by round-trip through count()/close_tab().
            // (Simpler: friend the subclass proc to Impl.)
            // For clarity we just post a message the parent handles; see
            // D6 variant: MainWindow subscribes to on_close_request instead.
            // If the host registered the callback, invoke directly:
            // (This block is filled in during implementation — the final
            //  code calls impl_->on_close_request(hit) through a friend
            //  declaration.)
        }
    }
    return DefSubclassProc(hwnd, msg, w, l);
}
}  // namespace

}  // namespace litepdf::ui
```

> **Implementer note (delete before committing):** the subclass proc's comment about "filled in during implementation" is a placeholder. The cleanest finalization: add `friend LRESULT CALLBACK tab_subclass_proc(...)` to `TabManager::Impl` via a friend class wrapper, or expose an internal member `void on_middle_click(int hit)` on `TabManager::Impl` and call `self->impl_->on_middle_click(hit)` which dispatches to `impl_->on_close_request`. Pick whichever produces less forwarding noise.

**Step 2a: CMake wiring**

In `CMakeLists.txt`, add `src/ui/TabManager.cpp` to the `litepdf` executable's source list (alphabetical):

```cmake
add_executable(litepdf WIN32
    src/main.cpp
    src/ui/ColdStartTimer.cpp
    src/ui/MainWindow.cpp
    src/ui/OutlinePane.cpp
    src/ui/PdfCanvas.cpp
    src/ui/TabManager.cpp
    resources/litepdf.rc
)
```

**Step 3: Build**

Run: `cmake --build build --config Release`
Expected: Green. (TabManager not yet wired into MainWindow — that's Task 4.)

**Step 4: Commit**

```bash
git add src/ui/TabManager.hpp src/ui/TabManager.cpp CMakeLists.txt
git commit -m "feat(ui): TabManager — Win32 tab-control wrapper owning a TabList (Phase 5 D5/D6)"
```

---

### Task 4: MainWindow — host TabManager, refactor view_ → active tab

**Files:**
- Modify: `src/ui/MainWindow.hpp`
- Modify: `src/ui/MainWindow.cpp`

**Step 1: Replace single view_ with TabManager**

In `src/ui/MainWindow.hpp`:

- Remove `#include "core/DocumentView.hpp"` (now transitively via TabList/TabManager), and add `#include "ui/TabManager.hpp"`.
- Replace `std::unique_ptr<litepdf::core::DocumentView> view_;` with `std::unique_ptr<TabManager> tabs_;`.
- Add helper methods:

```cpp
    // Active-tab view accessor — replaces the old `view_` raw reference.
    // Returns nullptr when no tab is active.
    litepdf::core::DocumentView* active_view();

    // Tab-switch handler registered with TabManager.
    void on_tab_switch(int new_index, int old_index);

    // Tab-close handler registered with TabManager (from middle-click).
    void on_tab_close_request(int index);

    // Open a new tab for `path` asynchronously (one request → one tab).
    void open_tab_async(std::filesystem::path path);

    // Rewrite window title based on the active tab (or reset to "LitePDF").
    void update_window_title();
```

- Remove the old `open_async` — replaced by `open_tab_async`. The single-tab epoch counter `open_epoch_` goes away (D11); each open appends unconditionally.

**Step 2: Update WM_CREATE to build TabManager**

In `WM_CREATE`, create the tab manager before outline/canvas children so z-order is natural (tabs at top, outline+canvas below):

```cpp
case WM_CREATE: {
    auto* cs = reinterpret_cast<CREATESTRUCTW*>(l);
    tabs_ = std::make_unique<TabManager>(cs->hInstance, hwnd);
    tabs_->set_on_switch(
        [this](int n, int o) { on_tab_switch(n, o); });
    tabs_->set_on_close_request(
        [this](int i) { on_tab_close_request(i); });
    tabs_->set_visible(false);  // no tabs yet
    canvas_ = std::make_unique<PdfCanvas>(cs->hInstance, hwnd);
    canvas_->set_log_timings(log_timings_);
    outline_ = std::make_unique<OutlinePane>(cs->hInstance, hwnd);
    outline_->set_on_navigate(
        [this](int page) { on_outline_navigate(page); });
    DragAcceptFiles(hwnd, TRUE);
    return 0;
}
```

**Step 3: Update on_layout for tab strip**

Replace `on_layout()` with:

```cpp
void MainWindow::on_layout() {
    if (!hwnd_) return;
    RECT rc; GetClientRect(hwnd_, &rc);
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    const UINT dpi = GetDpiForWindow(hwnd_);

    // Tab strip: only when there's at least one tab.
    const int tab_h = (tabs_ && tabs_->count() > 0)
        ? tabs_->strip_height(dpi) : 0;
    if (tabs_) {
        tabs_->set_visible(tab_h > 0);
        if (tab_h > 0) {
            SetWindowPos(tabs_->hwnd(), nullptr,
                         0, 0, w, tab_h,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }

    const int row_y = tab_h;
    const int row_h = std::max(0, h - tab_h);

    const int outline_w = MulDiv(250, static_cast<int>(dpi), 96);

    if (outline_ && outline_->visible()) {
        SetWindowPos(outline_->hwnd(), nullptr,
                     0, row_y, outline_w, row_h,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        if (canvas_) {
            const int canvas_w = std::max(0, w - outline_w);
            SetWindowPos(canvas_->hwnd(), nullptr,
                         outline_w, row_y, canvas_w, row_h,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
    } else {
        if (canvas_) {
            SetWindowPos(canvas_->hwnd(), nullptr,
                         0, row_y, w, row_h,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }
}
```

**Step 4: Rewrite open_async → open_tab_async**

```cpp
void MainWindow::open_tab_async(std::filesystem::path path) {
    HWND hwnd = hwnd_;
    std::thread([hwnd, path = std::move(path)]() {
        litepdf::core::Document doc;
        auto err = doc.open(path);
        if (err.has_value()) {
            PostMessageW(hwnd, WM_USER_OPEN_FAILED,
                         static_cast<WPARAM>(*err), 0);
            return;
        }
        try {
            auto tab = std::make_unique<litepdf::core::Tab>();
            tab->path  = path;
            tab->label = path.filename().wstring();
            tab->view  = std::make_unique<litepdf::core::DocumentView>(
                std::move(doc));
            // Transfer ownership across thread boundary via raw ptr.
            PostMessageW(hwnd, WM_USER_OPEN_OK,
                         reinterpret_cast<WPARAM>(tab.release()), 0);
        } catch (...) {
            PostMessageW(hwnd, WM_USER_OPEN_FAILED,
                         static_cast<WPARAM>(
                             litepdf::core::Document::OpenError::Other), 0);
        }
    }).detach();
}
```

**Step 5: Rewrite WM_USER_OPEN_OK**

```cpp
case WM_USER_OPEN_OK: {
    auto* raw = reinterpret_cast<litepdf::core::Tab*>(w);
    std::unique_ptr<litepdf::core::Tab> t(raw);
    ColdStartTimer::mark(2);

    const int new_index = tabs_->add_tab(std::move(t));
    // add_tab() fires on_switch synchronously with new_index — that
    // callback is where canvas/outline/layout/kick_render happens.
    // So here we have nothing further to do.
    (void)new_index;
    return 0;
}
```

**Step 6: Implement on_tab_switch**

```cpp
void MainWindow::on_tab_switch(int new_index, int old_index) {
    // Snapshot outgoing state.
    if (old_index >= 0) {
        if (auto* outgoing = tabs_->tab_at(old_index)) {
            auto p = canvas_ ? canvas_->pan() : PdfCanvas::Pan{0,0};
            outgoing->pan_x = p.x;
            outgoing->pan_y = p.y;
            outgoing->outline_visible = outline_ && outline_->visible();
        }
    }

    auto* incoming = tabs_->active_tab();
    if (canvas_) {
        canvas_->set_view(incoming ? incoming->view.get() : nullptr);
        if (incoming) canvas_->set_pan(incoming->pan_x, incoming->pan_y);
        else          canvas_->set_pan(0.0f, 0.0f);
    }

    if (outline_) {
        outline_->clear();
        if (incoming) {
            const auto& entries = incoming->view->document().outline();
            if (!entries.empty() && incoming->outline_visible) {
                outline_->populate(entries);
                outline_->show();
            } else {
                outline_->hide();
            }
        } else {
            outline_->hide();
        }
    }

    on_layout();
    update_window_title();

    if (incoming && canvas_) {
        // Re-seed zoom for the current viewport then kick render.
        RECT rc; GetClientRect(canvas_->hwnd(), &rc);
        UINT dpi = GetDpiForWindow(hwnd_);
        incoming->view->set_zoom_mode(
            incoming->view->zoom_mode(),
            static_cast<float>(rc.right - rc.left),
            static_cast<float>(rc.bottom - rc.top),
            static_cast<float>(dpi));
        kick_render(incoming->view->current_page());
    } else if (canvas_) {
        InvalidateRect(canvas_->hwnd(), nullptr, FALSE);
    }
}
```

**Step 7: Implement on_tab_close_request and close handlers**

```cpp
void MainWindow::on_tab_close_request(int index) {
    if (!tabs_) return;
    tabs_->close_tab(index);
    // close_tab() fires on_switch if the active tab changed — so layout,
    // canvas, outline, title all get refreshed via the existing path.
    if (tabs_->count() == 0) {
        // No active tab; ensure everything is blank.
        if (canvas_) {
            canvas_->set_view(nullptr);
            canvas_->set_pan(0.0f, 0.0f);
        }
        if (outline_) { outline_->clear(); outline_->hide(); }
        on_layout();
        update_window_title();
    }
}
```

**Step 8: update_window_title + active_view helpers**

```cpp
litepdf::core::DocumentView* MainWindow::active_view() {
    if (!tabs_) return nullptr;
    auto* t = tabs_->active_tab();
    return t ? t->view.get() : nullptr;
}

void MainWindow::update_window_title() {
    if (!hwnd_) return;
    auto* t = tabs_ ? tabs_->active_tab() : nullptr;
    if (!t) {
        SetWindowTextW(hwnd_, kWindowTitle);
        return;
    }
    std::wstring title = L"LitePDF — ";
    title += t->label;
    SetWindowTextW(hwnd_, title.c_str());
}
```

**Step 9: Replace every use of `view_` with `active_view()`**

- `kick_render()` → null-check `active_view()` instead of `view_`.
- `on_outline_navigate()` → route through `active_view()`.
- `toggle_outline()` → same; after toggle, store visibility on the active tab and kick_render.
- `WM_SIZE` / `WM_DPICHANGED` → use `active_view()`; iterate across tabs only if invariant state (like current_page) matters — it doesn't (zoom re-seeds on each switch), so a single active-tab kick is enough.
- `IDM_FILE_OPEN` / `WM_DROPFILES` / `IDM_MRU_*` → call `open_tab_async` instead of `open_async`.
- `IDM_ZOOM_*`: route through `active_view()`.

**Step 10: WM_NOTIFY forwarding for tab control**

Extend the existing `WM_NOTIFY` case to route tab-control notifications through `TabManager::handle_notify` first:

```cpp
case WM_NOTIFY: {
    auto* hdr = reinterpret_cast<NMHDR*>(l);
    if (tabs_ && tabs_->handle_notify(hdr)) return 0;
    if (outline_ && hdr && hdr->hwndFrom == outline_->hwnd() &&
        hdr->code == TVN_SELCHANGEDW) {
        // ... existing outline branch unchanged
    }
    break;
}
```

**Step 11: Remove stale-epoch discard at WM_USER_OPEN_OK (D11)**

`WM_USER_OPEN_FAILED` stops using the LPARAM epoch too — the value is now always 0 and the error code is shown regardless of how many opens are in flight.

**Step 12: Build + run**

Run: `cmake --build build --config Release`
Expected: Green.

Manual: launch with `bookmarks.pdf`. First tab appears with the filename. Window title reflects it. Everything looks the same as Phase 4 for a single tab. Open a second file via File→Open — second tab appears, first tab preserved, clicking tab headers switches documents.

**Step 13: Commit**

```bash
git add src/ui/MainWindow.hpp src/ui/MainWindow.cpp
git commit -m "feat(ui): MainWindow hosts TabManager; per-tab DocumentView + switch pipeline (Phase 5 D1/D7/D8/D10/D11)"
```

---

### Task 5: Tab close + keyboard shortcuts

**Files:**
- Modify: `src/ui/MainWindow.cpp`
- Modify: `src/ui/TabManager.cpp` (finalize middle-click wiring)

**Step 1: Finalize middle-click → close**

Remove the placeholder in the subclass proc from Task 3 and route middle-click properly. One clean shape: add `void Impl::on_middle_click(int hit)` that calls `on_close_request(hit)` if set, then `friend`-ed access from the subclass proc. Implementer's choice — the key invariant is that `WM_MBUTTONUP` on a tab header with `TCM_HITTEST >= 0` fires `on_close_request(hit)`.

**Step 2: Accelerators**

Extend the accelerator table in `MainWindow::run()`:

```cpp
ACCEL accels[] = {
    { FCONTROL | FVIRTKEY, 'O',          IDM_FILE_OPEN     },
    { FCONTROL | FVIRTKEY, VK_OEM_PLUS,  IDM_ZOOM_IN       },
    { FCONTROL | FVIRTKEY, VK_OEM_MINUS, IDM_ZOOM_OUT      },
    { FCONTROL | FVIRTKEY, '0',          IDM_ZOOM_RESET    },
    { FVIRTKEY,            VK_F5,        IDM_VIEW_OUTLINE  },
    // Phase 5: tab management.
    { FCONTROL | FVIRTKEY, 'W',          IDM_TAB_CLOSE     },
    { FCONTROL | FVIRTKEY, VK_TAB,       IDM_TAB_NEXT      },
    { FCONTROL | FSHIFT | FVIRTKEY, VK_TAB, IDM_TAB_PREV   },
    { FCONTROL | FVIRTKEY, '1', IDM_TAB_GOTO_1 },
    { FCONTROL | FVIRTKEY, '2', IDM_TAB_GOTO_2 },
    { FCONTROL | FVIRTKEY, '3', IDM_TAB_GOTO_3 },
    { FCONTROL | FVIRTKEY, '4', IDM_TAB_GOTO_4 },
    { FCONTROL | FVIRTKEY, '5', IDM_TAB_GOTO_5 },
    { FCONTROL | FVIRTKEY, '6', IDM_TAB_GOTO_6 },
    { FCONTROL | FVIRTKEY, '7', IDM_TAB_GOTO_7 },
    { FCONTROL | FVIRTKEY, '8', IDM_TAB_GOTO_8 },
    { FCONTROL | FVIRTKEY, '9', IDM_TAB_GOTO_9 },
};
```

**Step 3: WM_COMMAND handlers**

Extend the `WM_COMMAND` switch:

```cpp
case IDM_TAB_CLOSE:
    if (tabs_ && tabs_->count() > 0) {
        on_tab_close_request(tabs_->active_index());
    }
    return 0;
case IDM_TAB_NEXT:
    if (tabs_ && tabs_->count() > 1) {
        int n = (tabs_->active_index() + 1) % tabs_->count();
        tabs_->set_active(n);
    }
    return 0;
case IDM_TAB_PREV:
    if (tabs_ && tabs_->count() > 1) {
        int n = (tabs_->active_index() - 1 + tabs_->count()) % tabs_->count();
        tabs_->set_active(n);
    }
    return 0;
```

And for `IDM_TAB_GOTO_1..9`:

```cpp
if (id >= IDM_TAB_GOTO_1 && id <= IDM_TAB_GOTO_9) {
    int target = id - IDM_TAB_GOTO_1;  // 0-indexed
    if (tabs_ && target < tabs_->count()) {
        tabs_->set_active(target);
    }
    return 0;
}
```

**Step 4: Build + manual test**

Run: `cmake --build build --config Release`
Manual: open 3 tabs. Ctrl+Tab cycles forward, Ctrl+Shift+Tab cycles backward. Ctrl+2 jumps to tab 2. Ctrl+W closes active. Middle-click closes clicked tab. Close all → blank window, title resets to "LitePDF".

**Step 5: Commit**

```bash
git add src/ui/MainWindow.cpp src/ui/TabManager.cpp
git commit -m "feat(ui): tab close + Ctrl+W / Ctrl+Tab / Ctrl+1..9 keyboard nav (Phase 5 Task 5)"
```

---

### Task 6: Single-instance IPC — sender + server

**Files:**
- Create: `src/app/SingleInstance.hpp`
- Create: `src/app/SingleInstance.cpp`
- Modify: `src/main.cpp`
- Modify: `src/ui/MainWindow.hpp`
- Modify: `src/ui/MainWindow.cpp`
- Modify: `CMakeLists.txt` (add `SingleInstance.cpp` to `litepdf` executable; `src/app/` becomes the new app layer per design §4.1)

**Step 1: Create SingleInstance helper**

Create `src/app/SingleInstance.hpp`:

```cpp
#pragma once

// Per-user, single-instance gate for litepdf.exe.
// Uses Local\ namespace so the gate is scoped to the user's login session.
//
// IPC payloads (WM_COPYDATA):
//   dwData = kIpcOpenPath   ('LPDF'): lpData is a null-terminated UTF-16
//                                     absolute path; receiver opens it as a
//                                     new tab.
//   dwData = kIpcBringToFront ('LPDB'): empty payload; receiver restores
//                                       + foregrounds its main window.

#include <filesystem>
#include <windows.h>

namespace litepdf::app {

inline constexpr ULONG_PTR kIpcOpenPath     = 0x4C504446;  // 'LPDF'
inline constexpr ULONG_PTR kIpcBringToFront = 0x4C504442;  // 'LPDB'

// Attempts to acquire the single-instance mutex. Returns:
//   - owned HANDLE (caller must keep it alive for the app's lifetime) when
//     this process is the first instance.
//   - nullptr when another instance already holds the mutex.
// Out-param `already_running` is set true in the second case.
HANDLE try_acquire_single_instance(bool& already_running);

// Locates the running instance's main window by class name and forwards
// the command-line argument. If `path` is empty, sends kIpcBringToFront.
// Returns true on successful delivery.
bool forward_to_running_instance(const std::filesystem::path& path);

}  // namespace litepdf::app
```

**Step 2: Implement SingleInstance**

Create `src/app/SingleInstance.cpp`:

```cpp
#include "app/SingleInstance.hpp"

namespace {
constexpr wchar_t kMutexName[]  = L"Local\\LitePDF_SingleInstance_v1";
// Must match ui::MainWindow's kWindowClassName.
constexpr wchar_t kMainClass[]  = L"LitePDFMainWindow";
constexpr DWORD   kSendTimeoutMs = 3000;
}  // namespace

namespace litepdf::app {

HANDLE try_acquire_single_instance(bool& already_running) {
    already_running = false;
    HANDLE h = CreateMutexW(nullptr, FALSE, kMutexName);
    if (!h) return nullptr;  // CreateMutex failed — treat as "fail open"
                             // and let the caller continue as normal instance.
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(h);
        already_running = true;
        return nullptr;
    }
    return h;  // we own it
}

bool forward_to_running_instance(const std::filesystem::path& path) {
    HWND target = FindWindowW(kMainClass, nullptr);
    if (!target) return false;

    COPYDATASTRUCT cds = {};
    std::wstring wpath;
    if (path.empty()) {
        cds.dwData = kIpcBringToFront;
        cds.cbData = 0;
        cds.lpData = nullptr;
    } else {
        wpath = path.wstring();
        cds.dwData = kIpcOpenPath;
        cds.cbData = static_cast<DWORD>(
            (wpath.size() + 1) * sizeof(wchar_t));
        cds.lpData = wpath.data();
    }

    DWORD_PTR result = 0;
    LRESULT ok = SendMessageTimeoutW(
        target, WM_COPYDATA,
        reinterpret_cast<WPARAM>(nullptr),
        reinterpret_cast<LPARAM>(&cds),
        SMTO_ABORTIFHUNG, kSendTimeoutMs, &result);
    return ok != 0;
}

}  // namespace litepdf::app
```

**Step 3: Handle WM_COPYDATA in MainWindow**

In `MainWindow.hpp`, add:

```cpp
    LRESULT on_copydata(HWND hwnd, WPARAM w, LPARAM l);
```

In `MainWindow.cpp`, include the app header and handle the message:

```cpp
#include "app/SingleInstance.hpp"

// ...
case WM_COPYDATA:
    return on_copydata(hwnd, w, l);
```

```cpp
LRESULT MainWindow::on_copydata(HWND hwnd, WPARAM, LPARAM l) {
    auto* cds = reinterpret_cast<const COPYDATASTRUCT*>(l);
    if (!cds) return 0;

    if (cds->dwData == litepdf::app::kIpcBringToFront) {
        if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
        SetForegroundWindow(hwnd);
        return 1;
    }

    if (cds->dwData == litepdf::app::kIpcOpenPath) {
        // Validate: non-empty, sane length, null-terminated UTF-16.
        if (cds->cbData < sizeof(wchar_t)) return 0;
        if (cds->cbData > 64u * 1024u) return 0;
        if (cds->cbData % sizeof(wchar_t) != 0) return 0;
        const wchar_t* data = static_cast<const wchar_t*>(cds->lpData);
        const std::size_t count = cds->cbData / sizeof(wchar_t);
        if (count == 0 || data[count - 1] != L'\0') return 0;

        std::filesystem::path p(std::wstring(data, count - 1));
        if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
        SetForegroundWindow(hwnd);
        mru_.push(p.wstring());
        mru_.save();
        open_tab_async(std::move(p));
        return 1;
    }
    return 0;
}
```

**Step 4: Update main.cpp with the single-instance gate**

Rewrite `src/main.cpp`:

```cpp
#include "app/SingleInstance.hpp"
#include "ui/ColdStartTimer.hpp"
#include "ui/MainWindow.hpp"

#include <windows.h>
#include <shellapi.h>
#include <filesystem>
#include <string>

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    litepdf::ui::ColdStartTimer::set_t0();

    bool log_timings = false;
    std::filesystem::path initial_path;

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv && i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"--log-timings") log_timings = true;
        else if (initial_path.empty()) initial_path = a;
    }
    if (argv) LocalFree(argv);

    bool already = false;
    HANDLE mutex = litepdf::app::try_acquire_single_instance(already);
    if (already) {
        // Another instance is already running — forward our payload and exit.
        litepdf::app::forward_to_running_instance(initial_path);
        return 0;
    }
    // If CreateMutex failed entirely (mutex == nullptr && !already),
    // fall through as a normal instance — a single stale process is less
    // bad than refusing to run.

    litepdf::ui::MainWindow app;
    app.set_log_timings(log_timings);
    int rc = app.run(hInstance, nCmdShow, initial_path);

    if (mutex) CloseHandle(mutex);
    return rc;
}
```

**Step 5: CMake wiring**

In `CMakeLists.txt`, add `src/app/SingleInstance.cpp` to the `litepdf` executable:

```cmake
add_executable(litepdf WIN32
    src/main.cpp
    src/app/SingleInstance.cpp
    src/ui/ColdStartTimer.cpp
    src/ui/MainWindow.cpp
    src/ui/OutlinePane.cpp
    src/ui/PdfCanvas.cpp
    src/ui/TabManager.cpp
    resources/litepdf.rc
)
```

Also make sure `target_include_directories(litepdf_core PUBLIC src)` covers `src/app/*` — it already does since the include root is `src/`.

**Step 6: Manual test**

Run: `cmake --build build --config Release`

Terminal 1: `build\Release\litepdf.exe tests\fixtures\simple.pdf`
Terminal 2 (while 1 is still running): `build\Release\litepdf.exe tests\fixtures\bookmarks.pdf`
Expected: Terminal 2 exits immediately, code 0. Terminal 1's window now has two tabs (simple.pdf, bookmarks.pdf) and is foregrounded.

Terminal 2 again: `build\Release\litepdf.exe` (no path).
Expected: Terminal 1 foregrounded (or restored from minimized); no new tab.

**Step 7: Commit**

```bash
git add src/app/SingleInstance.hpp src/app/SingleInstance.cpp \
        src/main.cpp src/ui/MainWindow.hpp src/ui/MainWindow.cpp \
        CMakeLists.txt
git commit -m "feat(app): single-instance IPC via Local\\ mutex + WM_COPYDATA forwarding (Phase 5 D12/D13/D14/D15)"
```

---

### Task 7: Smoke-test + ux-probe extensions

**Files:**
- Modify: `scripts/smoke-test.ps1`
- Modify: `scripts/ux-probe.ps1`

**Step 1: Extend smoke test**

Add a multi-tab case to `scripts/smoke-test.ps1`:

- Start `litepdf.exe tests\fixtures\simple.pdf`.
- After window appears, spawn `litepdf.exe tests\fixtures\bookmarks.pdf` (second call should return ~instantly with exit 0).
- Poll until the main process reports 2 tabs via `ux-probe.ps1` (Step 2 below provides the probe).
- Verify no crash on exit (`Ctrl+W` twice, or kill gracefully).

**Step 2: Add tab-enumeration to ux-probe**

In `scripts/ux-probe.ps1`, add a `-TabEnum` switch that:

- Finds the `LitePDFMainWindow` HWND via `FindWindow`.
- Finds the `SysTabControl32` child via `EnumChildWindows`.
- Sends `TCM_GETITEMCOUNT` to get the count, and `TCM_GETCURSEL` to get active index.
- For each tab, sends `TCM_GETITEMW` with a `TCITEMW` to read the label text.
- Prints a JSON blob like `{"count":2,"active":1,"labels":["simple.pdf","bookmarks.pdf"]}`.

**Step 3: Run both**

Run:
```
cmake --build build --config Release
ctest --test-dir build -C Release
powershell -File scripts/smoke-test.ps1
```
Expected: all green.

**Step 4: Commit**

```bash
git add scripts/smoke-test.ps1 scripts/ux-probe.ps1
git commit -m "test(smoke): multi-tab smoke case + ux-probe -TabEnum (Phase 5 Task 7)"
```

---

### Task 8: Hardening sweep + version bump

**Files:**
- Modify: `src/ui/MainWindow.cpp`
- Modify: `src/ui/TabManager.cpp` (if the middle-click wiring needs polish)
- Modify: `resources/litepdf.rc` (VERSIONINFO 0.0.6.0)
- Modify: `VERSION` (`0.1.0-dev` → `0.1.0-dev` stays; tag is the version marker per roadmap)
- Modify: About dialog string in `MainWindow.cpp` (`v0.0.5` → `v0.0.6`)

**Step 1: Code-review the diff**

Invoke the spec-review + quality-review cycle from `superpowers:subagent-driven-development`. Likely findings and pre-emptive fixes:

- **WM_COPYDATA validator**: the 64 KB cap must include the terminator; double-check `cbData > 64 * 1024` comparison semantics.
- **Stale-tab pointer across reentry**: if `on_tab_switch` calls `kick_render()` and that synchronously posts a render callback that references `active_tab()`, a second switch could outpace it. This is already handled by PdfCanvas's orphan_ctx pattern (D8 from Phase 3), but double-check that `set_view(nullptr)` between switches doesn't drop pixmaps mid-post.
- **Thread-safety of `tabs_` during `open_tab_async`**: the worker thread owns the `Tab` until `WM_USER_OPEN_OK` is processed — no concurrent access. Verify with a comment above `open_tab_async`.
- **Middle-click on a tab while WM_USER_OPEN_OK for that slot is in flight**: the posted Tab* will arrive and add a new tab — indices shift. Add a comment noting this is benign (user closed a different tab; pending opens append at end).

**Step 2: Version strings**

Update `resources/litepdf.rc`:

```rc
FILEVERSION     0,0,6,0
PRODUCTVERSION  0,0,6,0
// ...
VALUE "FileVersion",      "0.0.6.0"
VALUE "ProductVersion",   "0.0.6.0"
```

Update About dialog in `MainWindow.cpp`:

```cpp
MessageBoxW(hwnd,
    L"LitePDF v0.0.6\n\n"
    L"A lightweight PDF / ePub / CBZ / XPS viewer for Windows.\n\n"
    L"License: AGPL-3.0\n"
    L"Engine: MuPDF 1.24.11\n"
    L"Rendering: Direct2D",
    kWindowTitle, MB_ICONINFORMATION);
```

**Step 3: Full CI**

Run: `cmake --build build --config Release && ctest --test-dir build -C Release`
Expected: all 72+ pass.

Run: `powershell -File scripts/smoke-test.ps1`
Expected: PASS.

**Step 4: Commit**

```bash
git add src/ui/MainWindow.cpp src/ui/TabManager.cpp resources/litepdf.rc
git commit -m "chore(ui): hardening pass + bump to v0.0.6 (Phase 5 finalize)"
```

---

### Task 9: Tag v0.0.6-phase5

**Step 1: Verify everything**

Run all tests + smoke test one more time. Verify clean `git status`.

**Step 2: Tag**

```bash
git tag v0.0.6-phase5
```

---

## Summary

| Task | What | New Tests | Commit verb |
|------|------|-----------|-------------|
| 0 | Reserve tab menu IDs | 0 | `feat(ui)` |
| 1 | TabList + TDD | +8 | `feat(core)` |
| 2 | PdfCanvas pan accessors | 0 | `feat(ui)` |
| 3 | TabManager (Win32 wrapper) | 0 | `feat(ui)` |
| 4 | MainWindow hosts TabManager; view_→active tab | 0 (manual) | `feat(ui)` |
| 5 | Tab close + keyboard shortcuts | 0 (manual) | `feat(ui)` |
| 6 | Single-instance IPC (mutex + WM_COPYDATA) | 0 (manual) | `feat(app)` |
| 7 | Smoke + ux-probe extensions | 0 (smoke) | `test(smoke)` |
| 8 | Hardening + version bump to 0.0.6 | 0 | `chore(ui)` |
| 9 | Tag v0.0.6-phase5 | — | tag |

**Total: 10 tasks, ~650 LOC, 8 new tests → 72 total.**

LOC note: the roadmap estimated ~400 for Phase 5. Realistic count lands closer to ~650 because TabManager's Win32 subclass plumbing and the `WM_COPYDATA` validator eat more than the table accounted for. The overage is tracked in §"Known Limitations" below so Phase 11's benchmark gate knows what it's measuring.

---

> **Fast-follow log:** `v0.0.6-phase5.1` added `cancel_stale_renders(INT_MAX)` on tab switch as a mitigation; `v0.0.6-phase5.2` delivered the root-cause fix via per-render context escrow (see `docs/plans/2026-04-18-per-render-ctx-escrow-design.md`).

## Known Limitations (to revisit)

- **Per-tab render pool.** Each `DocumentView` constructs its own `RenderEngine` with `num_workers = 2`. Ten tabs ⇒ twenty worker threads. Design §5.3 implies a shared pool; the refactor is deferred to Phase 11 where the benchmark gate will quantify the cost. (D16.)
- **No tab context menu.** Phase 5 has no right-click "close others" / "close to the right" affordance. Middle-click closes; `Ctrl+W` closes active. Context menu is YAGNI for v1.0; revisit if user feedback demands it.
- **No session restore.** Closing the last tab blanks the window. Phase 12 adds `session.json` per design §6.3.
- **No drag-to-reorder tabs.** Defer to post-v1.0.
- **IPC is one-way.** The forwarder never receives an ack; `SendMessageTimeoutW` returns 0 on timeout but we've already exited the sender by then. Acceptable because the only possible "failure" is a hung server, and failing silently is better than spawning a duplicate process.
