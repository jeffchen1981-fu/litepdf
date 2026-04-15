# Phase 3: Minimal Viewer (Tier 1) — Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use `superpowers:subagent-driven-development` to execute this plan task-by-task (same pattern that carried Phase 2 from 16→51 tests with clean review cycles). TDD with Catch2 where testable; manual smoke + scripted E2E where it's Win32/Direct2D.

**Goal:** First demoable LitePDF build. A real `MainWindow` with menu + `PdfCanvas` child control that renders a PDF via Direct2D off the UI thread. Open via File→Open or drag-drop, navigate with keyboard, zoom, and observe cold-start ≤ 1 s from launch-to-first-page on the HDD target.

**Architecture:** A single-tab viewer. `MainWindow` (top-level HWND, menu bar) owns one `DocumentView` — a per-tab container bundling `Document` + `RenderEngine` + `PageCache` + page/zoom state. `PdfCanvas` is a child HWND that owns a Direct2D `ID2D1HwndRenderTarget` and the currently-displayed `ID2D1Bitmap`. Render requests flow UI→Engine via `DocumentView::goto_page(n)` which calls `engine.submit(...)` with a callback that keeps the `fz_pixmap`, `PostMessage(WM_RENDER_COMPLETE, ...)` to the UI thread, where the handler uploads bytes into a new `ID2D1Bitmap`, swaps it in, and `InvalidateRect`s. No new UI-thread caching tier in Phase 3 — `PageCache` L2 (display list) already makes re-rasterize cheap, and D2D upload is ~5 ms for a 4 MB bitmap.

**Tech Stack:** C++17, MSVC v143, Win32 (comctl32, commdlg), Direct2D 1.1 (`d2d1.h`, `d2d1_1.h`), existing MuPDF/Catch2/CMake from Phases 0-2. No Direct2D wrapper libs (wrl, winrt) — raw COM with `ComPtr`-style `Microsoft::WRL::ComPtr` from `<wrl/client.h>` (header-only, ships with Windows SDK). No GDI fallback — Direct2D is required.

**Prerequisites (from Phase 2):**

- Tag `v0.0.3-phase2` on `main`; CI green on `windows-latest`.
- `core::RenderEngine` accepts `Document&` + optional `PageCache*`, renders to `fz_pixmap` via 2 workers, callback fires `on_complete(pix, ctx)` on worker thread per D2.
- `core::PageCache` L1 (pixmap by `(page, scale)`) + L2 (display list by `page`).
- `Document::clone_context()` + `Document::source_path()` available.
- 51/51 tests passing.

**Done when:**

1. `build/Release/litepdf.exe` launches to a visible window within **200 ms** of `wWinMain` entry (no document loaded — empty canvas).
2. File→Open or drag-dropping a PDF renders **page 1** within **1 second** from the user action (measured on the HDD fixture via the cold-start harness in Task 14).
3. Keyboard: PgDn/PgUp flips pages; Home/End jumps to first/last; arrow keys scroll when zoomed past fit; Ctrl+`=` / Ctrl+`-` / Ctrl+`0` zoom.
4. Ctrl+MouseWheel zooms.
5. Drag-dropping a `.pdf` file onto the window opens it (cancels any pending open first).
6. Opening a second PDF cleanly tears down the first `DocumentView` (joins workers, drops ctxs).
7. Resize / DPI-change redraws without flicker; `D2DERR_RECREATE_TARGET` recovery rebuilds the render target and re-submits.
8. `ctest --test-dir build -C Release` passes **all** Phase 2 tests (no regression) plus any new testable core pieces (Unicode path handling, zoom math).
9. Smoke test `scripts/smoke-test.ps1` extended to launch the exe with a fixture path on the command line and verify a page renders (screenshot-diff or window-valid check). CI runs it on `windows-latest`, green.
10. Cold-start benchmark log prints to stderr / `OutputDebugStringW` showing the 200 ms / 1 s checkpoints met on the dev machine fixture.
11. Tag `v0.0.4-phase3` pushed.

**Learnings carried from Phase 2 (important — constrain Phase 3 design):**

- MuPDF `fz_context` is not thread-safe; **UI thread needs its own ctx clone** for any MuPDF ops (refcount ops like `fz_keep_pixmap`/`fz_drop_pixmap` are atomic under locks, but the ctx you pass must be one you own — convention is one clone per long-lived thread).
- Refcount discipline: every `fz_keep_*` paired with `fz_drop_*`. Worker callbacks receive `(pix, worker_ctx)`; Phase 3 worker callback must `fz_keep_pixmap(worker_ctx, p)` to extend lifetime across the `PostMessage` boundary, then UI thread drops using its own ctx.
- `std::filesystem::path::string()` on Windows MSVC returns **ACP** (Windows-1252 / CP950 etc.), not UTF-8. Phase 3 user-selected paths will be Unicode; Task 0.1 fixes this retroactively in `Document.cpp` and `RenderEngine.cpp` before any File→Open code lands.
- `fz_device_bgr(ctx)` exists in MuPDF 1.24.11 (verified at `third_party/mupdf/include/mupdf/fitz/color.h:277`). Render with BGR + alpha=1 → BGRA output, matches `DXGI_FORMAT_B8G8R8A8_UNORM`. **Zero channel swap** at D2D upload time.
- Subagent-driven workflow (implementer → spec reviewer → quality reviewer → fix) caught 5+ deviations per phase. Keep using it. For small tasks bundle the two reviews into one combined pass.

---

## Architectural Decisions

Pinned before tasks; revisit only if a task uncovers a blocker.

**D1. Paged view, not continuous scroll.** One page displayed at a time. PgDn/PgUp flips. Continuous scrolling is an explicitly post-v1.0 feature per design doc §10. Rationale: roadmap Phase 3 exit criterion says "page up/down works" (Tier 1). Continuous requires rendering + laying out multiple pages simultaneously, ballooning Phase 3 scope.

**D2. No separate D2D bitmap cache in Phase 3.** `PdfCanvas` holds exactly one `ID2D1Bitmap` — the bitmap for the currently-displayed page at the currently-chosen zoom. When page or zoom changes, discard and upload the new one. `PageCache` L2 (display list) still makes re-rasterize fast (~30 ms for a typical A4 page), and D2D `CreateBitmap` from CPU bytes is ~5 ms. Total round-trip on page flip is well within 50 ms — eye-motion-threshold. If Phase 4+ UX reveals jitter on rapid paging, a `D2DBitmapCache` sibling to `PageCache` can be added then. Design doc §5.2's "L1 ID2D1Bitmap" goal becomes deferred — rationale documented here, not an omission.

**D3. UI thread owns one `fz_context*` clone for its MuPDF needs.** `MainWindow` (or `PdfCanvas`; whichever holds the active `DocumentView`) stores a `fz_context* ui_ctx_` created via `doc.clone_context()` after the document opens. Used for `fz_drop_pixmap` on pixmaps received from worker threads via `PostMessage`, and for any MuPDF queries on the UI thread (e.g., reading pixmap dimensions before upload). Dropped in the opposite order of creation when the DocumentView tears down.

**D4. Pixmap format = BGRA via `fz_device_bgr(ctx)` + alpha = 1.** Worker submits `fz_new_pixmap_from_display_list(ctx, dl, m, fz_device_bgr(ctx), nullptr, 1)`. Output matches D2D `DXGI_FORMAT_B8G8R8A8_UNORM` byte-for-byte. Update `RenderEngine.cpp` Task 10 rasterize call (Phase 2 hardcoded `fz_device_rgb` and `alpha=0` — verify and switch as part of Task 0.3 pre-phase cleanup).

**D5. Async document open.** On File→Open or WM_DROPFILES, UI thread:
  1. Cancels any currently-pending open (a `std::atomic<bool> cancel_pending_open_`).
  2. Spawns `std::thread([path, hwnd]{ ... })` that constructs `Document`, calls `open()`, constructs `RenderEngine` + `PageCache`, then `PostMessage(hwnd, WM_DOCUMENT_OPENED, success_flag, reinterpret_cast<LPARAM>(new DocumentView(...)))`.
  3. UI thread handler takes ownership, replaces current view, kicks off first render.
  4. If the user opened a second file while the first open was in flight, the first thread posts a result that the UI handler discards (by checking a token/epoch).

**D6. Zoom levels.** Finite set: 50/75/100/125/150/200/300/400% + **Fit-Width** (default) + **Fit-Page**. Ctrl+`=` steps up; Ctrl+`-` steps down; Ctrl+`0` resets to Fit-Width. Ctrl+MouseWheel zooms toward mouse cursor (positioning preserved). Actual render scale derived from zoom level + DPI + fit-mode at `DocumentView::set_zoom()` time.

**D7. Drag-drop via `WM_DROPFILES`.** No `IDropTarget` / OLE. `DragAcceptFiles(hwnd, TRUE)` in `WM_CREATE`; `WM_DROPFILES` handler calls `DragQueryFileW` for the first file, validates extension, kicks off D5.

**D8. `D2DERR_RECREATE_TARGET` recovery.** On any `Draw*` or `EndDraw` call that returns this HRESULT:
  1. Release current `ID2D1HwndRenderTarget` and `ID2D1Bitmap`.
  2. Rebuild render target from the HWND + current client size.
  3. Mark current page as needing re-render; re-submit.
  4. Next paint draws a placeholder until the re-render completes.

**D9. Cold-start targets (measured in Task 14).**
  - Window visible: **≤ 200 ms** from `wWinMain` entry.
  - First page painted after user-action Open: **≤ 1 s** on HDD fixture.
  - Measurement: `QueryPerformanceCounter` snapshots at (a) `wWinMain` entry, (b) just after `ShowWindow`, (c) WM_DOCUMENT_OPENED received, (d) WM_RENDER_COMPLETE applied to canvas, (e) `EndDraw` of first paint with the real bitmap. Log via `OutputDebugStringW` (Debug view / DebugView.exe).

**D10. Single tab in Phase 3.** `MainWindow` owns one `DocumentView` via `std::unique_ptr<DocumentView>`. `TabManager` + multi-tab state lands in Phase 5. Keep `DocumentView` self-contained so Phase 5 refactor is "hold N of these" not "rewrite this".

**D11. HIGH-DPI aware.** Manifest already declares PerMonitorV2 (Phase 0). Honor it: compute logical-pixel sizes from `GetDpiForWindow(hwnd)`, `GetSystemMetricsForDpi`. On `WM_DPICHANGED`, rebuild render target at the new DPI + resubmit current render.

---

## Pre-Phase Cleanup (Task 0 group)

Items that block clean Phase 3 work.

### Task 0.1: Unicode path handling in Document + RenderEngine

**Why:** File→Open will produce user paths that may contain Chinese/Japanese/Korean characters. Current code uses `path.string()` which on Windows MSVC returns the **active code page** (CP950 for Taiwan, CP936 mainland, CP932 Japan, CP949 Korea, CP1252 Western). MuPDF's `fz_open_document` expects **UTF-8**. For non-ASCII paths, the current code silently corrupts and fails-to-open.

**Files:**
- Modify: `src/core/Document.cpp` — line 156, replace `path.string()` with UTF-8 conversion.
- Modify: `src/core/RenderEngine.cpp` — line 249, same fix.
- Test: `tests/unit/test_document_unicode_path.cpp` (new).
- Test fixture: `tests/fixtures/測試.pdf` (new — a copy of `simple.pdf` renamed with Chinese characters).

**Step 1 — Add fixture.**

```bash
cp tests/fixtures/simple.pdf "tests/fixtures/測試.pdf"
git add "tests/fixtures/測試.pdf"
```

Verify the file's name survives git's filename handling on Windows (git's `core.quotepath` may mangle — if so, set `git config core.quotepath false` at the repo level).

**Step 2 — Write failing test:**

```cpp
// tests/unit/test_document_unicode_path.cpp
#include <catch2/catch_test_macros.hpp>
#include "core/Document.hpp"
#include <filesystem>

TEST_CASE("Document opens a PDF with a non-ASCII path", "[core][document][unicode]") {
    litepdf::core::Document doc;
    std::filesystem::path p = std::filesystem::u8path(u8"tests/fixtures/\u6e2c\u8a66.pdf");
    //                                                           ^^^^ 測 ^^^^ 試
    auto err = doc.open(p);
    REQUIRE(!err.has_value());
    REQUIRE(doc.page_count() > 0);
}
```

Register in `tests/CMakeLists.txt`.

**Step 3 — Run: expect fail** (current `path.string()` returns CP950-mangled path; `fz_open_document` returns null; `doc.open` returns `FileNotFound` or `Unsupported`).

**Step 4 — Fix in `Document.cpp`.** Replace:

```cpp
impl_->doc = fz_open_document(impl_->ctx, path.string().c_str());
```

with:

```cpp
// UTF-8 conversion — fz_open_document takes UTF-8 char*. On Windows,
// path.u8string() yields UTF-8 regardless of active code page.
#if __cpp_lib_char8_t >= 201907L
    // C++20: path::u8string() returns std::u8string; reinterpret to char*.
    const auto u8 = path.u8string();
    const char* utf8_bytes = reinterpret_cast<const char*>(u8.c_str());
#else
    // C++17: path::u8string() returns std::string already containing UTF-8.
    const std::string u8 = path.u8string();
    const char* utf8_bytes = u8.c_str();
#endif
    impl_->doc = fz_open_document(impl_->ctx, utf8_bytes);
```

(Project is C++17 — the `#if` keeps it future-proof if we upgrade.)

Also update `path.extension().string()` at Document.cpp:94 to `path.extension().u8string()` for consistency (extension comparison happens in `looks_like_supported_document` — check whether case-insensitive matching is affected, probably not for ASCII extensions, but use u8 for hygiene).

Also fix `src/core/RenderEngine.cpp:249` same way.

**Step 5 — Run test: expect pass.** Full suite `ctest -C Release` expect 52/52.

**Step 6 — Commit:**

```
fix(core): UTF-8 path handling for non-ASCII filenames (TDD)

Phase 3 Task 0.1. std::filesystem::path::string() on Windows MSVC
returns ACP (CP950/936/932/etc), but fz_open_document needs UTF-8.
Switched both Document::open and RenderEngine ctor to use
path.u8string() (C++17 returns std::string UTF-8; kept a C++20-
compatible reinterpret-cast branch for future-proofing).

New fixture tests/fixtures/測試.pdf (a copy of simple.pdf) exercises
the conversion path. 52/52 tests pass.

Deferred-from-Phase-2: ✅ resolved.
```

### Task 0.2: CI actions refresh (silence Node.js 20 deprecation)

**Why:** CI emits a warning about Node.js 20 deprecation starting June 2026. `actions/checkout@v4` and `actions/upload-artifact@v4` are on Node 20. Options per GitHub's changelog: upgrade to `@v5` (if published) OR add `FORCE_JAVASCRIPT_ACTIONS_TO_NODE24: true` env var.

**Files:** Modify `.github/workflows/ci.yml`.

**Step 1:** Check Actions marketplace via `gh api`. If `@v5` exists for checkout and upload-artifact, prefer explicit pin:

```bash
gh api repos/actions/checkout/releases/latest --jq .tag_name
gh api repos/actions/upload-artifact/releases/latest --jq .tag_name
```

**Step 2:** If v5 available, bump. Else add env:

```yaml
jobs:
  build-windows:
    runs-on: windows-latest
    env:
      FORCE_JAVASCRIPT_ACTIONS_TO_NODE24: true
```

**Step 3 — Commit:**

```
ci: silence Node.js 20 deprecation warning (v5 actions or FORCE_NODE24)
```

### Task 0.3: Switch render path to BGR + alpha=1

**Why:** D4 — upstream-consumable by Direct2D without channel swap. Currently Phase 2 renders RGB with alpha=0 (see `RenderEngine.cpp:174`).

**Files:** Modify `src/core/RenderEngine.cpp`. Test: no new test — existing render tests still pass because they only check non-null + dimensions, not pixel format. Add one assertion:

**Step 1 — Extend an existing test** (`tests/unit/test_render_engine_output.cpp`) to verify components == 4 (BGRA, not 3 for RGB):

```cpp
// After REQUIRE(h > 0);
int components = fz_pixmap_components(test_ctx, got);
REQUIRE(components == 3);  // BGR data planes; alpha is separate in MuPDF's model
int has_alpha = fz_pixmap_alpha(test_ctx, got);
REQUIRE(has_alpha == 1);
int stride = fz_pixmap_stride(test_ctx, got);
REQUIRE(stride >= w * 4);  // 4 bytes/pixel: B, G, R, A
```

Forward-declare `fz_pixmap_components` / `fz_pixmap_alpha` / `fz_pixmap_stride` in the test's `extern "C"` block.

**Step 2 — Update rasterize call in RenderEngine.cpp:**

```cpp
fz_try(ctx) {
    fz_matrix m = fz_scale(req.scale, req.scale);
    // BGR + alpha=1 → BGRA buffer matching D2D's B8G8R8A8_UNORM.
    pix = fz_new_pixmap_from_display_list(ctx, dlist, m,
                                          fz_device_bgr(ctx), /*alpha*/ 1);
}
```

(Remove the `nullptr` argument that snuck in during Phase 2 — `fz_new_pixmap_from_display_list` takes 5 args in 1.24.11.)

**Step 3 — Build, run tests: expect 52/52.**

**Step 4 — Update CLI PPM output path** (`src/cli/main.cpp`). Previously RGB, now BGR. Either:
  - (a) Swap channels to R-G-B for PPM output (PPM spec is RGB).
  - (b) Change CLI output to a format that natively accepts BGR — e.g., emit BMP.
  Choose **(a)**: simple per-pixel swap, fewer moving parts.

```cpp
for (int y = 0; y < h; ++y) {
    const unsigned char* row = samples + y * stride;
    for (int x = 0; x < w; ++x) {
        unsigned char rgb[3] = { row[x*4 + 2], row[x*4 + 1], row[x*4 + 0] };  // B,G,R → R,G,B
        std::fwrite(rgb, 1, 3, stdout);
    }
}
```

Manually smoke again: `litepdf-cli.exe simple.pdf --render 0 > out.ppm`; verify in viewer.

**Step 5 — Commit:**

```
refactor(core): render BGRA (fz_device_bgr + alpha=1) for D2D upload

Phase 3 Task 0.3 (D4). Switches rasterize from RGB+alpha=0 to
BGR+alpha=1 so pixmap bytes match Direct2D's B8G8R8A8_UNORM
format — zero channel swap at upload. CLI PPM path now swaps
channels at write time (PPM is RGB).

Tests: 52/52 (+1 assertion on pixmap components/alpha/stride).
```

---

## Phase 3 Tasks

### Task 1: Win32 app scaffold (MainWindow + menu + message pump)

**Why first:** every UI task needs a real window class + menu + message pump. Current `src/main.cpp` is a 66-line Phase 0 empty-window stub; replace with a real `MainWindow` class that Phase 3 can build on.

**Files:**
- Create: `src/ui/MainWindow.hpp`
- Create: `src/ui/MainWindow.cpp`
- Modify: `src/main.cpp` — thin wrapper that calls `MainWindow::run()`.
- Create: `resources/MainMenu.rc.h` (menu IDs) + include in `resources/litepdf.rc`.
- Modify: root `CMakeLists.txt` — add `src/ui/MainWindow.cpp` to `litepdf_app` sources (not core — UI lives outside the library).

**Public API (MainWindow.hpp):**

```cpp
#pragma once
#include <memory>
#include <windows.h>

namespace litepdf::ui {

class DocumentView;  // forward decl; Phase 3 Task 5

class MainWindow {
public:
    MainWindow();
    ~MainWindow();

    MainWindow(const MainWindow&)            = delete;
    MainWindow& operator=(const MainWindow&) = delete;

    // Top-level entry: registers classes, creates HWND, runs message pump.
    // Returns WM_QUIT's wParam (conventionally 0 on clean exit).
    int run(HINSTANCE hInstance, int nCmdShow);

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT handle_message(HWND, UINT, WPARAM, LPARAM);

    HWND hwnd_ = nullptr;
    std::unique_ptr<DocumentView> view_;  // Phase 3 Task 5+
};

}
```

**Menu IDs (MainMenu.rc.h):**

```cpp
#define IDM_FILE_OPEN    40001
#define IDM_FILE_EXIT    40002
#define IDM_HELP_ABOUT   40003
```

**Menu resource (append to litepdf.rc or create a sub-resource file):**

```rc
#include "MainMenu.rc.h"
IDM_MAIN_MENU MENU
{
    POPUP "&File"
    {
        MENUITEM "&Open...\tCtrl+O", IDM_FILE_OPEN
        MENUITEM SEPARATOR
        MENUITEM "E&xit",           IDM_FILE_EXIT
    }
    POPUP "&Help"
    {
        MENUITEM "&About LitePDF", IDM_HELP_ABOUT
    }
}
```

Assign `IDM_MAIN_MENU` to the `LoadMenuW(hInstance, MAKEINTRESOURCEW(IDM_MAIN_MENU))` call in `run()`.

**`run()` implementation sketch:**

1. `InitCommonControlsEx`.
2. Register window class with ptr-to-this in `cbWndExtra` (store via `SetWindowLongPtrW(hwnd, GWLP_USERDATA, this)` after `CreateWindowEx` returns).
3. `CreateWindowExW(...)` with menu loaded from resource.
4. `ShowWindow`, `UpdateWindow`.
5. Message pump: `GetMessageW` loop.

`WndProc` delegates to `handle_message` via `GetWindowLongPtrW(hwnd, GWLP_USERDATA)`; handle `WM_CREATE`, `WM_COMMAND` (menu), `WM_KEYDOWN` (Ctrl+O accelerator — actually wire via ACCEL table), `WM_DESTROY`.

Wire an accelerator table for Ctrl+O (and Ctrl+=, Ctrl+-, Ctrl+0 for zoom in Task 10 — declare the table now, add entries as tasks need them):

```cpp
ACCEL accels[] = {
    { FCONTROL | FVIRTKEY, 'O', IDM_FILE_OPEN },
};
HACCEL haccel = CreateAcceleratorTableW(accels, _countof(accels));
// In the message pump: if (TranslateAcceleratorW(hwnd_, haccel, &msg)) continue;
```

**Tests:** This task's testable surface is tiny (no core logic). Skip Catch2; manual smoke: launch exe, confirm window appears with menu, File→Open produces a message box (stub), File→Exit closes.

**Step 1 — Write files per sketch above.**

**Step 2 — Build:** `cmake --build build --config Release`. Expect clean.

**Step 3 — Manually launch:** `build/Release/litepdf.exe`. Confirm window appears < 500 ms (D9 prep), menu visible, File→Exit closes cleanly.

**Step 4 — Update `scripts/smoke-test.ps1`:**

The existing smoke test checks window shows up within a budget. Keep it — it'll still pass since MainWindow shows a window too.

**Step 5 — Commit:**

```
feat(ui): MainWindow scaffold with menu + message pump

Phase 3 Task 1. Replaces Phase 0 stub main.cpp with a real
ui::MainWindow class: registers window class, loads menu from
resources (File→Open, File→Exit, Help→About stubs), accelerator
table with Ctrl+O, message pump with TranslateAccelerator.

Menu handlers are message-box stubs for now; real Open logic lands
in Task 6 and About in a later polish task.

Manual smoke: window visible on launch; menu items navigable.
```

### Task 2: PdfCanvas child HWND + Direct2D init

**Files:**
- Create: `src/ui/PdfCanvas.hpp`
- Create: `src/ui/PdfCanvas.cpp`
- Modify: `src/ui/MainWindow.cpp` — create a `PdfCanvas` child in `WM_CREATE`, resize in `WM_SIZE`.
- Modify: root `CMakeLists.txt`.

**Public API (PdfCanvas.hpp):**

```cpp
#pragma once
#include <memory>
#include <windows.h>

// Forward-decl so header stays COM-free. Direct2D interfaces only in .cpp.
struct ID2D1Factory;
struct ID2D1HwndRenderTarget;
struct ID2D1Bitmap;

namespace litepdf::ui {

class PdfCanvas {
public:
    // Registers the window class on first call; creates a child HWND of `parent`.
    PdfCanvas(HINSTANCE hInstance, HWND parent);
    ~PdfCanvas();

    PdfCanvas(const PdfCanvas&) = delete;
    PdfCanvas& operator=(const PdfCanvas&) = delete;

    HWND hwnd() const { return hwnd_; }

    // Task 6+: called by MainWindow after a render completes.
    void set_bitmap(/* pixmap + ctx; details finalized in Task 6 */);

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT handle_message(HWND, UINT, WPARAM, LPARAM);

    void create_render_target();
    void discard_render_target();
    void on_paint();
    void on_size(int width, int height);

    HWND hwnd_ = nullptr;
    struct Impl;
    std::unique_ptr<Impl> impl_;  // holds ComPtr<ID2D1*>s
};

}
```

**Impl (in .cpp):**

```cpp
#include <d2d1.h>
#include <d2d1_1.h>
#include <wrl/client.h>
#pragma comment(lib, "d2d1.lib")

using Microsoft::WRL::ComPtr;

struct PdfCanvas::Impl {
    ComPtr<ID2D1Factory>          factory;
    ComPtr<ID2D1HwndRenderTarget> rt;
    ComPtr<ID2D1Bitmap>           current_bitmap;
    D2D1_SIZE_U last_size = { 0, 0 };
};
```

**Behavior for Task 2 scope only:**

- Register class `"LitePDFPdfCanvas"`, style `CS_HREDRAW|CS_VREDRAW`.
- On `WM_CREATE`: create `ID2D1Factory` (single-threaded, `D2D1_FACTORY_TYPE_SINGLE_THREADED`). Store in Impl. Do NOT create render target yet (creates on-demand in `on_paint()`).
- On `WM_SIZE`: store new size; if we have a render target, `rt->Resize(D2D1::SizeU(w, h))`. If the resize returns `D2DERR_RECREATE_TARGET`, discard and let next paint rebuild.
- On `WM_PAINT`:
  - If no render target, create one.
  - `BeginDraw`; `Clear` to a light gray (0xF0F0F0);
  - No bitmap yet (Phase 3 Task 6 wires set_bitmap). For Task 2, just paint the cleared background.
  - `EndDraw`. If result is `D2DERR_RECREATE_TARGET`, discard; next paint rebuilds.
  - `ValidateRect(hwnd_, nullptr)`.
- On `WM_ERASEBKGND`: return 1 (we paint ourselves; prevents flicker).

**Test:** No testable core logic; manual smoke — launching the exe now shows a light-gray client area instead of the default window bg.

**Step 1 — Write files.**

**Step 2 — Build, launch, observe light-gray canvas on resize. No flicker.**

**Step 3 — Commit:**

```
feat(ui): PdfCanvas child HWND with Direct2D render target

Phase 3 Task 2. Creates PdfCanvas as a child of MainWindow filling
the client area. Direct2D factory created on WM_CREATE; HwndRender
Target is lazy (first paint). On WM_SIZE we resize; on
D2DERR_RECREATE_TARGET we discard and let next paint rebuild.
WM_PAINT clears to light gray — page rendering lands in Task 6.

WM_ERASEBKGND returns 1 to prevent flicker.
```

### Task 3: DPI + WM_DPICHANGED handling

**Files:** Modify `src/ui/MainWindow.cpp` and `src/ui/PdfCanvas.cpp`.

Register PerMonitorV2 awareness in the manifest (already done in Phase 0). Read DPI via `GetDpiForWindow(hwnd)`. On `WM_DPICHANGED`:
- MainWindow: use the suggested rect in `LPARAM` to resize itself.
- PdfCanvas: discards render target (will rebuild with new DPI on next paint — `CreateHwndRenderTarget` picks up the current DPI automatically), resubmits current page at the new scale (Task 13 wiring — for now just rebuild and repaint empty).

**Smoke:** drag the window between monitors of different DPIs or toggle display scale in Windows Settings. Client area renders crisp (not bilinear-stretched).

**Step — Commit:**

```
feat(ui): WM_DPICHANGED handling for MainWindow + PdfCanvas

Phase 3 Task 3 (D11). PerMonitorV2 manifest already in place;
now honor it — window follows the suggested rect in DPICHANGED,
canvas discards its render target so next paint rebuilds at the
new DPI. Bitmap re-render (at the new pixel scale) lands in
Task 13 when render pipeline is wired.
```

### Task 4: `core::DocumentView` container

**Files:**
- Create: `src/core/DocumentView.hpp`
- Create: `src/core/DocumentView.cpp`
- Modify: root `CMakeLists.txt` — core library.
- Test: `tests/unit/test_document_view.cpp`

**API:**

```cpp
namespace litepdf::core {

class DocumentView {
public:
    enum class ZoomMode { FitWidth, FitPage, Custom };

    // Moves the Document in (caller usually opens it on a worker thread).
    // num_workers + cache capacities for the engine.
    DocumentView(Document doc,
                 std::size_t num_workers = 2,
                 std::size_t l1 = 5,
                 std::size_t l2 = 10);
    ~DocumentView();

    DocumentView(const DocumentView&) = delete;
    DocumentView& operator=(const DocumentView&) = delete;
    DocumentView(DocumentView&&) = delete;
    DocumentView& operator=(DocumentView&&) = delete;

    int page_count() const;
    int current_page() const;            // 0-based
    bool set_current_page(int idx);      // clamps; returns whether changed

    ZoomMode zoom_mode() const;
    float    zoom_scale() const;         // last-computed effective scale
    void     set_zoom_mode(ZoomMode, float viewport_w_px = 0, float viewport_h_px = 0);
    bool     zoom_in();                  // steps through preset levels
    bool     zoom_out();

    // Submit current page for render with a UI-thread-side completion hook.
    // on_complete fires on a worker thread per D2; caller is responsible for
    // fz_keep_pixmap / PostMessage pattern.
    using RenderCb = std::function<void(struct fz_pixmap*, struct fz_context*)>;
    void request_render(int page, RenderCb on_complete);

    // Bulk cancel for rapid paging.
    void cancel_stale_renders(int keep_priority_threshold);

    // UI thread uses this to drop pixmaps it received via PostMessage.
    fz_context* ui_ctx() const;  // D3

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
```

**Impl holds:** `Document doc`, `PageCache cache`, `RenderEngine engine`, `fz_context* ui_ctx`, `int current_page`, `ZoomMode zm`, `float scale`.

**Test:** basic construction + getters:

```cpp
TEST_CASE("DocumentView constructs from an opened Document", "[core][view]") {
    litepdf::core::Document doc;
    REQUIRE(!doc.open("tests/fixtures/simple.pdf").has_value());
    litepdf::core::DocumentView view(std::move(doc));
    REQUIRE(view.page_count() > 0);
    REQUIRE(view.current_page() == 0);
    REQUIRE(view.zoom_mode() == litepdf::core::DocumentView::ZoomMode::FitWidth);
}
```

Document's move ctor was added in Phase 1; confirm it's still there. If not, Task 4 also adds move semantics to Document (should be trivial — std::unique_ptr<Impl>).

**Step — Commit:**

```
feat(core): DocumentView per-tab container (Document + Engine + Cache) (TDD)

Phase 3 Task 4. Bundles a single Document + PageCache + RenderEngine
+ per-view state (current page, zoom mode, zoom scale, UI ctx clone).
Moves the Document in at construction; owns its lifetime. Phase 5's
TabManager will hold one of these per tab.

Render API request_render(page, cb) is a thin passthrough to
engine.submit for Task 6 wiring. ui_ctx() exposes a clone for the
UI thread's drop_pixmap needs (D3).
```

### Task 5: File→Open + async Document open

**Files:** Modify `src/ui/MainWindow.cpp`. Add `#include <commdlg.h>` + link `comdlg32.lib`.

On `WM_COMMAND`/`IDM_FILE_OPEN`:
1. `GetOpenFileNameW` with filter `"PDF files (*.pdf)\0*.pdf\0All files\0*.*\0"` and `OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR`.
2. On cancel → no-op. On OK → extract path (wchar_t buffer → `std::filesystem::path`).
3. Kick off async open (helper — see D5):

```cpp
void MainWindow::open_async(std::filesystem::path path) {
    // Cancel any prior pending open (just increment an epoch; old thread's
    // PostMessage is filtered on arrival).
    ++open_epoch_;
    int my_epoch = open_epoch_.load();

    std::thread([hwnd = hwnd_, path = std::move(path), my_epoch, this] {
        litepdf::core::Document doc;
        auto err = doc.open(path);
        if (err.has_value()) {
            PostMessageW(hwnd, WM_USER_OPEN_FAILED, static_cast<WPARAM>(*err), my_epoch);
            return;
        }
        auto* dv = new litepdf::core::DocumentView(std::move(doc));
        PostMessageW(hwnd, WM_USER_OPEN_OK, reinterpret_cast<WPARAM>(dv), my_epoch);
    }).detach();
}
```

Custom messages:
```cpp
constexpr UINT WM_USER_OPEN_OK     = WM_USER + 1;
constexpr UINT WM_USER_OPEN_FAILED = WM_USER + 2;
constexpr UINT WM_USER_RENDER_DONE = WM_USER + 3;  // Task 6
```

Handler for `WM_USER_OPEN_OK`:
```cpp
if (LPARAM(lParam) != open_epoch_.load()) {
    // Stale — user opened another file after this one.
    delete reinterpret_cast<litepdf::core::DocumentView*>(wParam);
    return 0;
}
view_.reset(reinterpret_cast<litepdf::core::DocumentView*>(wParam));
// Task 6 kicks off first render here.
return 0;
```

`WM_USER_OPEN_FAILED` pops a MessageBox describing the error (map `OpenError` enum to strings).

**Tear-down ordering:** `view_.reset(new ...)` destroys the old view, which destroys the old RenderEngine (joins workers) on the UI thread. For Phase 3 target HDD this is acceptable (< 200 ms). If Phase 4+ shows stutter, move old view to a detached deleter thread.

**Manual smoke:** File→Open on `simple.pdf` — no visible effect yet (Task 6 adds paint). On `corrupt.pdf`: expect MessageBox "Unsupported format" or similar.

**Commit:**

```
feat(ui): File→Open with async Document construction

Phase 3 Task 5 (D5). WM_COMMAND IDM_FILE_OPEN shows
GetOpenFileNameW, kicks off std::thread opening the Document and
constructing a DocumentView (Engine + Cache). Completion lands on
UI thread via PostMessage. Epoch-based staleness check discards
late results from a superseded open.

WM_USER_OPEN_FAILED pops a message box mapping OpenError values
to human strings. Task 6 will trigger a first-page render once
a view lands.
```

### Task 6: Render pipeline (worker → PostMessage → D2D upload)

**Files:** Modify `src/ui/MainWindow.cpp`, `src/ui/PdfCanvas.cpp`, `src/core/DocumentView.cpp`.

After receiving `WM_USER_OPEN_OK`, MainWindow calls `request_first_render()`:

```cpp
void MainWindow::request_first_render() {
    if (!view_) return;
    int page = view_->current_page();
    float scale = view_->zoom_scale();  // computed based on viewport size
    HWND target = canvas_->hwnd();
    view_->request_render(page, [target](fz_pixmap* p, fz_context* worker_ctx) {
        if (!p) {
            PostMessageW(target, WM_USER_RENDER_DONE, 0, 0);
            return;
        }
        // Keep the pixmap so it survives the PostMessage boundary.
        fz_keep_pixmap(worker_ctx, p);
        PostMessageW(target, WM_USER_RENDER_DONE,
                     reinterpret_cast<WPARAM>(p),
                     /*page*/ 0);
    });
}
```

`PdfCanvas::handle_message(WM_USER_RENDER_DONE, ...)`:
1. `fz_pixmap* pix = reinterpret_cast<fz_pixmap*>(wParam);`
2. If null → painter-error (Task 13 shows placeholder); fall through to clear paint.
3. Read dimensions via `fz_pixmap_width/_height/_stride/_samples`.
4. Create `ID2D1Bitmap` from samples:
```cpp
D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
    D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
ComPtr<ID2D1Bitmap> bmp;
HRESULT hr = rt->CreateBitmap(D2D1::SizeU(w, h), samples, stride, &props, &bmp);
```
5. If `CreateBitmap` fails with `D2DERR_RECREATE_TARGET`, discard rt and retry (MainWindow cycles paint).
6. Swap `impl_->current_bitmap = bmp;` and `InvalidateRect(hwnd_, nullptr, FALSE)`.
7. Drop the pixmap using the UI ctx: `fz_drop_pixmap(view_->ui_ctx(), pix);` — this is what D3 is for.

PdfCanvas needs a pointer back to DocumentView to get the ui_ctx. Add `set_view(DocumentView*)` to PdfCanvas called by MainWindow right after setting `view_`.

Update `PdfCanvas::on_paint()` to draw `current_bitmap` if non-null:

```cpp
if (impl_->current_bitmap) {
    D2D1_SIZE_F src_px = impl_->current_bitmap->GetSize();  // in DIPs
    D2D1_SIZE_F vp = impl_->rt->GetSize();
    // Center on canvas; scale to fit (width fit in Phase 3).
    float s = vp.width / src_px.width;
    if (src_px.height * s > vp.height) s = vp.height / src_px.height;
    float dx = (vp.width  - src_px.width  * s) / 2.0f;
    float dy = (vp.height - src_px.height * s) / 2.0f;
    D2D1_RECT_F dst = D2D1::RectF(dx, dy, dx + src_px.width * s, dy + src_px.height * s);
    impl_->rt->DrawBitmap(impl_->current_bitmap.Get(), dst, 1.0f,
                          D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
}
```

**Manual smoke:** File→Open simple.pdf → page 0 appears centered. Resizing re-fits.

**Commit:**

```
feat(ui): render page 0 on open — worker → PostMessage → D2D upload

Phase 3 Task 6. Completes the first end-to-end render path:
MainWindow submits a render on open, worker callback fz_keep_pixmaps
+ PostMessages the pointer, PdfCanvas uploads BGRA samples to an
ID2D1Bitmap via CreateBitmap, drops the pixmap on UI ctx, paints
fit-to-viewport centered. First demoable PDF viewer.

D2DERR_RECREATE_TARGET during CreateBitmap discards rt; next paint
rebuilds and the resubmit will follow in Task 13.
```

### Task 7: Fit-Width vs Fit-Page; derive scale from DPI + viewport

**Files:** Modify `src/core/DocumentView.cpp`, `src/ui/PdfCanvas.cpp` (to pass viewport size into DocumentView on each render request).

Scale derivation:
```
page_w_pt, page_h_pt = doc.page_size(page)  // 1 pt = 1/72 inch
viewport_w_dip, viewport_h_dip = canvas client size in DIPs (GetDpiForWindow adjusts)
dpi = GetDpiForWindow(hwnd)

fit_width_scale   = (viewport_w_dip / page_w_pt) * (dpi / 96)
fit_page_scale    = min(fit_width_scale, (viewport_h_dip / page_h_pt) * (dpi / 96))
custom_scale      = preset_percent * (dpi / 96)   // preset_percent ∈ {0.5, 0.75, 1.0, ...}
```

`DocumentView::set_zoom_mode(ZoomMode m, float vp_w, float vp_h)` stores the mode + viewport, updates the cached scale. Subsequent `request_render` uses the cached scale.

On `PdfCanvas::on_size`, recompute scale via `view_->set_zoom_mode(view_->zoom_mode(), new_w, new_h)` and re-submit current page.

**Test:** core-testable zoom math:
```cpp
TEST_CASE("DocumentView FitWidth zoom math", "[core][view][zoom]") {
    litepdf::core::Document doc;
    REQUIRE(!doc.open("tests/fixtures/simple.pdf").has_value());
    // simple.pdf is 596x842 pt (A4-ish).
    litepdf::core::DocumentView v(std::move(doc));
    v.set_zoom_mode(DocumentView::ZoomMode::FitWidth, /*vp*/ 1192, 800);
    // Expected: (1192 / 596) * (96/96) = 2.0
    REQUIRE(v.zoom_scale() == Catch::Approx(2.0f).epsilon(0.001));
}
```

Adjust the numbers based on the real `simple.pdf` size (query it first).

**Commit:**

```
feat(core): DocumentView zoom math — FitWidth/FitPage scale from DPI + viewport (TDD)

Phase 3 Task 7 (D6). Given page size (pt), viewport size (DIPs),
and DPI, compute render scale for FitWidth / FitPage / Custom
zoom modes. Custom uses preset percentages {50, 75, 100, 125,
150, 200, 300, 400}.

Tests verify FitWidth math on simple.pdf — deterministic, no
render round-trip.
```

### Task 8: Keyboard navigation (PgUp/PgDn/Home/End)

**Files:** Modify `src/ui/MainWindow.cpp` `handle_message` to forward `WM_KEYDOWN` of interest to `PdfCanvas` (or PdfCanvas subclasses the handler directly — simpler, since focus is on the canvas).

Actually, let the canvas take keyboard focus (`SetFocus(canvas_->hwnd())` after create) and handle `WM_KEYDOWN` itself. PdfCanvas needs a `DocumentView*` reference (already added in Task 6).

```cpp
case WM_KEYDOWN:
    switch (wParam) {
        case VK_NEXT:  view_->set_current_page(view_->current_page() + 1); request_render(); break;
        case VK_PRIOR: view_->set_current_page(view_->current_page() - 1); request_render(); break;
        case VK_HOME:  view_->set_current_page(0);                          request_render(); break;
        case VK_END:   view_->set_current_page(view_->page_count() - 1);    request_render(); break;
        case VK_LEFT:  if (scrolled) scroll_left();  break;  // Task 9
        // ... arrows for scroll in Task 9
    }
    break;
```

Call `cancel_stale_renders` before each new `request_render` to support aggressive scrubbing.

**Manual smoke:** open multi-page fixture, PgDn/PgUp flips within ~100 ms.

**Commit:**

```
feat(ui): keyboard page navigation (PgUp/PgDn/Home/End)

Phase 3 Task 8. PdfCanvas takes focus; WM_KEYDOWN dispatches to
DocumentView::set_current_page and triggers a new render. Each nav
cancel_stale_renders to avoid a flood of queued work on rapid
keypress.
```

### Task 9: Zoom (Ctrl+=, Ctrl+-, Ctrl+0, Ctrl+MouseWheel) + arrow scroll when zoomed

**Files:** Modify `src/ui/MainWindow.cpp` (accelerators), `src/ui/PdfCanvas.cpp` (WM_MOUSEWHEEL, arrow scroll state).

Accelerators (extend the table from Task 1):
```cpp
ACCEL accels[] = {
    { FCONTROL | FVIRTKEY, 'O', IDM_FILE_OPEN },
    { FCONTROL | FVIRTKEY, VK_OEM_PLUS,  IDM_ZOOM_IN },
    { FCONTROL | FVIRTKEY, VK_OEM_MINUS, IDM_ZOOM_OUT },
    { FCONTROL | FVIRTKEY, '0',          IDM_ZOOM_RESET },
};
```

Handlers: `view_->zoom_in()`, `zoom_out()`, `set_zoom_mode(FitWidth, ...)`. After each, resubmit render.

PdfCanvas `WM_MOUSEWHEEL`: if `wParam` has `MK_CONTROL`, zoom toward mouse cursor point (translate viewport offset to keep that point stationary — involves scroll state from the next bullet). Else regular scroll (pass through to Task 8 scroll logic).

Arrow scroll when zoomed: DocumentView tracks `(offset_x, offset_y)` (current pan in DIPs). `VK_UP/DOWN/LEFT/RIGHT` adjusts by 100 DIPs. PdfCanvas clamps to image bounds + repaints (no new render needed — just adjust the destination rect in `DrawBitmap`).

**Commit:**

```
feat(ui): zoom (Ctrl+=/-/0 + Ctrl+Wheel) and arrow-scroll when zoomed

Phase 3 Task 9. Zoom levels cycle through the preset list.
Ctrl+MouseWheel zooms toward cursor point (viewport-preserving
translation). Arrow keys scroll by 100 DIPs when the page is
larger than the viewport.

Zoom triggers a resubmit; pan adjusts the draw rect without
re-rasterizing.
```

### Task 10: Drag-drop (WM_DROPFILES)

**Files:** Modify `src/ui/MainWindow.cpp` and `src/ui/PdfCanvas.cpp`.

`DragAcceptFiles(hwnd_, TRUE)` on WM_CREATE (for whichever HWND should accept drops — typically MainWindow so the whole client region works).

`WM_DROPFILES` handler:
```cpp
auto hdrop = reinterpret_cast<HDROP>(wParam);
wchar_t buf[MAX_PATH];
if (DragQueryFileW(hdrop, 0, buf, MAX_PATH)) {
    std::filesystem::path p(buf);
    if (p.extension() == L".pdf" /* or other supported */) {
        open_async(std::move(p));
    }
}
DragFinish(hdrop);
```

**Commit:**

```
feat(ui): drag-drop PDFs onto the window (WM_DROPFILES)

Phase 3 Task 10 (D7). DragAcceptFiles on MainWindow; dropping a
.pdf reuses open_async from Task 5. Only the first dropped file
is honored; extension filter gates non-PDF drops silently.
```

### Task 11: Prefetch adjacent pages + aggressive cancel on rapid nav

**Files:** Modify `src/core/DocumentView.cpp` + `src/ui/MainWindow.cpp` or `PdfCanvas.cpp`.

After every `set_current_page(n)`:
1. `engine.cancel_all_below_priority(0)` — cancel any pending P1/P2 work.
2. Submit P0: page `n` at current scale.
3. Submit P1: page `n-1` and `n+1` at current scale.
4. Submit P2: page `n-5..n-2` and `n+2..n+5` for **display list only** (set a flag in RenderRequest; Task 11 may need a minor RenderRequest extension OR a separate `submit_display_list_only(page)` method on DocumentView that the engine interprets).

For simplicity, limit Phase 3 to P0 + P1 (prev/next pixmap prefetch). P2 display-list prefetch deferred to Phase 4 polish.

**Commit:**

```
feat(core): prefetch adjacent pages (P1) with aggressive cancel

Phase 3 Task 11. DocumentView::set_current_page now cancels all
pending below-P0 renders and submits P0 (current) + P1 (prev, next)
at the active zoom. Rapid PgDn scrubbing stays responsive — the
queue is drained before each new batch.

P2 display-list prefetch deferred to Phase 4.
```

### Task 12: D2DERR_RECREATE_TARGET recovery + resubmit current page

**Files:** Modify `src/ui/PdfCanvas.cpp`.

After `EndDraw` or `CreateBitmap` returns `D2DERR_RECREATE_TARGET`:
1. `impl_->current_bitmap.Reset()`.
2. `impl_->rt.Reset()`.
3. If there's a `DocumentView*`, resubmit current page.
4. Next `on_paint` rebuilds the render target.

Test: hard to automate without GPU device-loss simulation. Manual smoke: lock screen / unlock; change display settings; Ctrl+Alt+Del and back — viewer should recover without crash or blank.

**Commit:**

```
feat(ui): D2DERR_RECREATE_TARGET recovery + resubmit current page

Phase 3 Task 12 (D8). Any Draw/EndDraw/CreateBitmap that returns
this HRESULT causes us to drop the render target and the current
bitmap, resubmit the current page, and rebuild on next paint.
Exercised by screen-lock / display-reconfigure cycles.
```

### Task 13: Cold-start benchmark + debug logging

**Files:** Modify `src/ui/MainWindow.cpp` + `src/main.cpp`.

Add a `CSTimer` helper that snapshots `QueryPerformanceCounter` and logs elapsed ms to `OutputDebugStringW` at 5 named checkpoints (D9):
- `T0`: `wWinMain` entry.
- `T1`: after `ShowWindow`.
- `T2`: WM_USER_OPEN_OK received.
- `T3`: WM_USER_RENDER_DONE received (first page pixmap in).
- `T4`: `EndDraw` after first real bitmap paint.

Log format:
```
LitePDF cold-start: T0→T1=185ms T0→T2=520ms T0→T3=680ms T0→T4=690ms
```

Targets:
- T0→T1 ≤ 200 ms
- T0→T4 ≤ 1000 ms on HDD fixture

Bench via the manual smoke harness. If targets aren't met, investigate before tagging.

**Commit:**

```
feat(ui): cold-start timing log (5 named checkpoints)

Phase 3 Task 13 (D9). QueryPerformanceCounter snapshots at
wWinMain-entry, post-ShowWindow, document-opened, first-render-done,
first-paint-complete. Single OutputDebugStringW line at T4. Dev
machine target: T0→T1 ≤ 200ms, T0→T4 ≤ 1000ms on simple.pdf from
HDD. Smoke test in Task 14 greps this line.
```

### Task 14: Smoke test extension + scripts/smoke-test.ps1

**Files:** Modify `scripts/smoke-test.ps1`.

Extend the Phase 0 smoke (window-shows-up check) to:
1. Launch `litepdf.exe tests\fixtures\simple.pdf` (add a command-line path handler to MainWindow — treat argv[1] as a path to open_async immediately after show).
2. Wait up to 5 s for the window to show.
3. Verify process still alive after 5 s (no crash on first render).
4. Optionally: capture stdout/stderr from a debug `--log-timings` flag that mirrors the OutputDebugStringW to stderr, parse the cold-start line, and fail if T0→T4 > 1500 ms (loose budget; CI is variable).

No OCR / screenshot-diff in Phase 3; Phase 4+ adds that if needed.

**Step 1 — Add argv[1] path handler to MainWindow** (in `run()` before the message pump, if argv > 1 and the path exists, call `open_async(argv[1])`).

**Step 2 — Add `--log-timings` flag that redirects the cold-start line to stderr.**

**Step 3 — Update `scripts/smoke-test.ps1`:**

```powershell
$exe = "build/Release/litepdf.exe"
$fixture = "tests/fixtures/simple.pdf"
$proc = Start-Process -FilePath $exe -ArgumentList @($fixture, "--log-timings") `
                     -RedirectStandardError timings.log -PassThru
Start-Sleep -Seconds 5
if ($proc.HasExited) { throw "litepdf.exe exited prematurely (code $($proc.ExitCode))" }
Stop-Process -Id $proc.Id -Force

$line = Get-Content timings.log | Where-Object { $_ -match "cold-start:" }
if (-not $line) { throw "No cold-start line logged" }
if ($line -match "T0->T4=(\d+)ms") {
    $t4 = [int]$Matches[1]
    if ($t4 -gt 1500) { throw "Cold-start T0->T4 = $t4 ms > 1500 ms budget" }
    Write-Host "Cold-start T0->T4 = $t4 ms (budget 1500 ms)  OK"
}
```

**Commit:**

```
test: smoke-test verifies first page renders + cold-start budget

Phase 3 Task 14. scripts/smoke-test.ps1 now launches the exe with
a fixture path, confirms the process survives 5 s (first render
completes), and parses the cold-start timing line to enforce
T0->T4 < 1500 ms in CI.

MainWindow accepts an argv[1] path (opens on start) and a
--log-timings flag (mirrors OutputDebugStringW to stderr).
```

### Task 15: CI run + final review + tag v0.0.4-phase3

**Step 1** — push to origin; watch `gh run list`. Expect green on `windows-latest`.

**Step 2** — dispatch a Phase 3 final code review subagent over the diff `v0.0.3-phase2..HEAD`. Apply Important items.

**Step 3** — tag:

```bash
git tag -a v0.0.4-phase3 -m "Phase 3: MainWindow + PdfCanvas + Direct2D minimal viewer" -F docs/plans/phase-3-summary.md
git push origin v0.0.4-phase3
```

---

## Deferred to Phase 4+ (do NOT do in Phase 3)

- Outline / bookmarks tree (Phase 4).
- MRU recent files (Phase 4).
- Multi-tab (Phase 5).
- In-doc search Ctrl+F (Phase 6).
- Thumbnails pane F4 (Phase 7).
- Dark mode (Phase 8).
- Dual-page spread (Phase 8).
- Password dialog for encrypted PDFs (Phase 8).
- ePub/CBZ/XPS format-specific UX (Phase 8).
- P2 display-list prefetch (Phase 4 polish).
- Continuous-scroll mode (post-v1.0).
- `D2DBitmapCache` sibling cache (only if UX jitter observed on rapid paging — revisit after Phase 3 dogfood).

## Deferred Phase 2 items addressed here

- ✅ Unicode path handling (Task 0.1).
- ✅ BGRA pixmap format for D2D (Task 0.3).
- ✅ CI Node.js 20 deprecation (Task 0.2).

Still deferred:
- MuPDF 1.24.11 CVE audit (Phase 12 release gate).
- `flatten_outline` recursion bound — not exercised in Phase 3 (outline pane is Phase 4). Harden then.
- `vcxproj` copy-to-build-dir (pre-v1.0 cleanup).
- CI `actions/cache` for MuPDF (7m → 2-3m) — consider in Phase 4 since CI time climbs with more tests.
- C++20 upgrade decision — re-evaluate when `__cpp_lib_char8_t` branch in Task 0.1 matters (not Phase 3).

## Re-Planning Hooks (for Phase 4 kickoff)

Before starting Phase 4, re-invoke `superpowers:writing-plans` with:
- Phase 3 exit commit SHA + dev-machine cold-start numbers (T0→T1 / T0→T4).
- Whether rapid-paging UX stutters (drives `D2DBitmapCache` decision).
- Whether Unicode path fixture broke anything MuPDF-deep.
- Any D2D device-loss recovery quirks observed.
