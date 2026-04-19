# Tab Strip Polish Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Replace the default Win11-flat `SysTabControl32` rendering with a clearly-bounded owner-drawn strip: per-tab separators, distinct active-tab highlight, hover-revealed close button, label ellipsis, and hot-switching dark-mode awareness.

**Architecture:** Enable `TCS_OWNERDRAWFIXED` on the existing tab control and take over paint inside `TabManager.cpp`. `MainWindow` gains two small delegate hops — `WM_DRAWITEM` → `TabManager::handle_draw_item` and `WM_SETTINGCHANGE` → `TabManager::handle_theme_change`. `TabList` and its 9 unit tests are untouched. Design doc: `docs/plans/2026-04-19-tab-strip-polish-design.md`.

**Tech Stack:** C++17, Win32 (USER + GDI), `SysTabControl32` owner-draw, DWM (dark-mode query). No new dependencies.

---

## Pre-flight

Before starting, confirm:

- Working tree is clean: `git status` should show no uncommitted changes.
- Release build + tests currently green:
  ```bash
  cmake --build build --config Release
  ctest --test-dir build -C Release --output-on-failure
  ```
- Design doc is committed (commit `27f944e`).

**Reference open at all times:** `docs/plans/2026-04-19-tab-strip-polish-design.md` (visual spec + geometry table).

**Smoke-test app invocation** (used as the "manual check" in most tasks):

```bash
cd C:/Users/User/projects/litepdf
build/Release/litepdf.exe tests/fixtures/simple.pdf
# Then File → Open a second and third fixture to get 3 tabs.
```

---

## Task 1: Plumbing — header methods + MainWindow delegates (no visual change)

**Goal:** Land the delegate wiring so `MainWindow` can forward `WM_DRAWITEM` and `WM_SETTINGCHANGE` to `TabManager`, but the methods are stubs that return `false` → behavior is identical to today.

**Files:**
- Modify: `src/ui/TabManager.hpp` — add two public method declarations.
- Modify: `src/ui/TabManager.cpp` — add two empty method definitions.
- Modify: `src/ui/MainWindow.cpp` — add two `case` branches in `handle_message`.

**Step 1.1: Edit `src/ui/TabManager.hpp`**

After the `handle_notify` declaration (around line 52), add:

```cpp
// Parent's WM_DRAWITEM routes here when the tab control is the sender.
// Returns true if the DRAWITEMSTRUCT was ours and was painted.
bool handle_draw_item(const DRAWITEMSTRUCT* dis);

// Parent's WM_SETTINGCHANGE routes here on "ImmersiveColorSet" so the
// strip can re-detect dark mode and repaint without a restart.
void handle_theme_change();
```

**Step 1.2: Edit `src/ui/TabManager.cpp`**

Add stub definitions just above `LRESULT CALLBACK tab_subclass_proc`:

```cpp
bool TabManager::handle_draw_item(const DRAWITEMSTRUCT* /*dis*/) {
    return false;  // stub — Task 2 will implement
}

void TabManager::handle_theme_change() {
    // stub — Task 9 will implement
}
```

**Step 1.3: Edit `src/ui/MainWindow.cpp`**

Inside `handle_message`, add a `WM_DRAWITEM` case before the `WM_NOTIFY` case (around line 366):

```cpp
case WM_DRAWITEM: {
    auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(l);
    if (tabs_ && dis && dis->hwndItem == tabs_->hwnd()) {
        if (tabs_->handle_draw_item(dis)) return TRUE;
    }
    break;
}
case WM_SETTINGCHANGE: {
    if (w == 0 && l != 0) {
        auto* name = reinterpret_cast<const wchar_t*>(l);
        if (wcscmp(name, L"ImmersiveColorSet") == 0) {
            if (tabs_) tabs_->handle_theme_change();
        }
    }
    break;
}
```

**Step 1.4: Build + test**

Run:
```bash
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```
Expected: build succeeds, all tests green (TabList 9 tests still pass).

**Step 1.5: Smoke-test**

Launch `build/Release/litepdf.exe tests/fixtures/simple.pdf`. Visually identical to before — this task adds wiring only.

**Step 1.6: Commit**

```bash
git add src/ui/TabManager.hpp src/ui/TabManager.cpp src/ui/MainWindow.cpp
git commit -m "refactor(ui): add TabManager WM_DRAWITEM + WM_SETTINGCHANGE delegates

Wires MainWindow to forward WM_DRAWITEM (from the owner-drawn tab
control) and WM_SETTINGCHANGE (broadcast-only, child controls cannot
observe it directly) into new TabManager public hooks. Hooks are
currently stubs returning false / no-op; subsequent commits implement
the owner-draw paint and dark-mode hot-switch.

Part of tab-strip polish (see docs/plans/2026-04-19-tab-strip-polish-design.md)."
```

---

## Task 2: Enable `TCS_OWNERDRAWFIXED` + minimal paint (confirm we own the paint)

**Goal:** Prove the owner-draw pipeline works end-to-end with a trivial paint (solid background + label with `DrawTextW`). Output will be ugly but consistent — we will refine palette and geometry in later tasks.

**Files:**
- Modify: `src/ui/TabManager.cpp` — add `TCS_OWNERDRAWFIXED` to style, implement `paint_tab` as a free helper and call from `handle_draw_item`.

**Step 2.1: Add `TCS_OWNERDRAWFIXED` to the create-window style**

In `TabManager::TabManager` ctor, change:

```cpp
WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | TCS_FOCUSNEVER,
```
to:
```cpp
WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | TCS_FOCUSNEVER | TCS_OWNERDRAWFIXED,
```

**Step 2.2: Implement minimal `paint_tab`**

In the anonymous namespace at the top of `TabManager.cpp`, add:

```cpp
void paint_tab_minimal(const DRAWITEMSTRUCT* dis, const std::wstring& label) {
    HDC hdc = dis->hDC;
    RECT rc = dis->rcItem;

    // Background: pale grey for normal, near-white for active.
    const bool is_active = (dis->itemState & ODS_SELECTED) != 0;
    HBRUSH bg = CreateSolidBrush(is_active ? RGB(0xFF, 0xFF, 0xFF)
                                           : RGB(0xE8, 0xE8, 0xE8));
    FillRect(hdc, &rc, bg);
    DeleteObject(bg);

    // Label.
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(0x20, 0x20, 0x20));
    RECT text_rc = rc;
    text_rc.left += 8;
    text_rc.right -= 8;
    DrawTextW(hdc, label.c_str(), -1, &text_rc,
              DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
}
```

**Step 2.3: Wire `handle_draw_item` to call it**

Replace the Task 1 stub:

```cpp
bool TabManager::handle_draw_item(const DRAWITEMSTRUCT* dis) {
    if (!impl_ || !dis || dis->hwndItem != impl_->hwnd) return false;
    const int idx = static_cast<int>(dis->itemID);
    if (idx < 0 || idx >= count()) return false;
    auto* tab = impl_->list.at(static_cast<std::size_t>(idx));
    if (!tab) return false;
    paint_tab_minimal(dis, tab->label);
    return true;
}
```

**Step 2.4: Build**

```bash
cmake --build build --config Release
```
Expected: clean build.

**Step 2.5: Smoke-test**

Launch litepdf with 2-3 PDFs. Each tab now paints with our own background and label. Active tab is distinctly lighter than inactive. Looks rough (no separator, no height bump yet) but proves pipeline.

**Step 2.6: Commit**

```bash
git add src/ui/TabManager.cpp
git commit -m "feat(ui): enable TCS_OWNERDRAWFIXED with minimal paint

Proves the owner-draw pipeline: style mask gains TCS_OWNERDRAWFIXED,
handle_draw_item dispatches to a minimal paint routine that fills a
flat background and draws the label with DT_END_ELLIPSIS. Active tab
is distinguished by near-white background vs pale-grey inactive.
Visual polish (colors, separator, active accent, close button, hover,
DPI-aware height) follows in subsequent commits."
```

---

## Task 3: DPI-aware height + RAII GDI wrappers + font creation

**Goal:** Make the strip taller (32 px @ 96 DPI, scaling with DPI) and introduce RAII-wrapped HFONTs (normal + bold) cached in `Impl`, rebuilt only when DPI changes.

**Files:**
- Modify: `src/ui/TabManager.cpp` — add `unique_hfont` alias, `Impl` members, font-ensure helper, `strip_height` override, `TCM_SETITEMSIZE` call.

**Step 3.1: Add RAII alias + Impl members**

At the top of the anonymous namespace in `TabManager.cpp`:

```cpp
constexpr int kTabHeightDip  = 32;
constexpr int kTabMinWidthDip = 120;
constexpr int kTabMaxWidthDip = 240;
constexpr int kTabPaddingDip  = 12;       // horizontal inner padding
constexpr int kCloseSizeDip   = 14;
constexpr int kCloseRightPadDip = 8;

using unique_hfont = std::unique_ptr<std::remove_pointer_t<HFONT>,
                                     decltype(&DeleteObject)>;
unique_hfont make_unique_hfont(HFONT h) {
    return unique_hfont(h, &DeleteObject);
}

HFONT create_tab_font(UINT dpi, bool bold) {
    LOGFONTW lf = {};
    lf.lfHeight = -MulDiv(9, static_cast<int>(dpi), 72);  // 9 pt
    lf.lfWeight = bold ? FW_SEMIBOLD : FW_NORMAL;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    wcscpy_s(lf.lfFaceName, L"Segoe UI");
    return CreateFontIndirectW(&lf);
}
```

Extend `struct TabManager::Impl` (keep existing members, add these):

```cpp
    unique_hfont font_normal { nullptr, &DeleteObject };
    unique_hfont font_bold   { nullptr, &DeleteObject };
    UINT cached_dpi = 0;

    void ensure_fonts(UINT dpi) {
        if (cached_dpi == dpi && font_normal && font_bold) return;
        font_normal = make_unique_hfont(create_tab_font(dpi, /*bold=*/false));
        font_bold   = make_unique_hfont(create_tab_font(dpi, /*bold=*/true));
        cached_dpi  = dpi;
    }
```

**Step 3.2: Override `strip_height` to use our constant**

Replace the body of `TabManager::strip_height`:

```cpp
int TabManager::strip_height(UINT dpi) const {
    if (!impl_ || !impl_->hwnd) return 0;
    if (dpi == 0) dpi = GetDpiForWindow(impl_->hwnd);
    return MulDiv(kTabHeightDip, static_cast<int>(dpi), 96);
}
```

**Step 3.3: Apply TCM_SETITEMSIZE after create and on DPI change**

In `TabManager::TabManager`, after the `SetWindowSubclass` call, append:

```cpp
    // Baseline item size; paint code still computes per-tab width.
    const UINT dpi = GetDpiForWindow(impl_->hwnd);
    SendMessageW(impl_->hwnd, TCM_SETITEMSIZE, 0,
                 MAKELPARAM(MulDiv(kTabMaxWidthDip, dpi, 96),
                            MulDiv(kTabHeightDip, dpi, 96)));
```

Also set the control's window font so non-owner-draw measurements (e.g. hit-test) use the right size:

```cpp
    impl_->ensure_fonts(dpi);
    SendMessageW(impl_->hwnd, WM_SETFONT,
                 reinterpret_cast<WPARAM>(impl_->font_normal.get()),
                 MAKELPARAM(TRUE, 0));
```

**Step 3.4: Build + smoke-test**

```bash
cmake --build build --config Release
```
Launch with 2–3 PDFs. Strip is now noticeably taller (~32 px), tab text is Segoe UI 9 pt. No other visual change yet.

**Step 3.5: Commit**

```bash
git add src/ui/TabManager.cpp
git commit -m "feat(ui): DPI-aware tab height and cached Segoe UI fonts

Bumps the tab strip to 32 dip (scaled via MulDiv) and introduces
unique_hfont RAII wrappers around cached normal + semibold Segoe UI
fonts. Fonts rebuild lazily when DPI changes (multi-monitor drag).
TCM_SETITEMSIZE + WM_SETFONT keep the control's internal metrics
consistent with owner-draw paint."
```

---

## Task 4: Dark-mode detection + palette struct

**Goal:** Centralize colors in a `Palette` struct that is produced from a `bool dark_mode` flag detected via DWM / registry with a sane fallback.

**Files:**
- Modify: `src/ui/TabManager.cpp` — add Palette struct, `is_dark_mode_detected()` helper, Impl member + getter.

**Step 4.1: Add palette + detection in anonymous namespace**

```cpp
struct Palette {
    COLORREF bg_normal;
    COLORREF bg_hover;
    COLORREF bg_active;
    COLORREF text_inactive;
    COLORREF text_active;
    COLORREF separator;
    COLORREF accent;          // active top bar
    COLORREF close_hover_bg;  // red
    COLORREF close_hover_fg;  // white × on red
    COLORREF close_fg;        // grey × at rest
};

Palette make_palette(bool dark) {
    if (dark) {
        return {
            /*bg_normal*/      RGB(0x26, 0x26, 0x26),
            /*bg_hover*/       RGB(0x3A, 0x3A, 0x3A),
            /*bg_active*/      RGB(0x2D, 0x2D, 0x2D),
            /*text_inactive*/  RGB(0xA0, 0xA0, 0xA0),
            /*text_active*/    RGB(0xF2, 0xF2, 0xF2),
            /*separator*/      RGB(0x3A, 0x3A, 0x3A),
            /*accent*/         GetSysColor(COLOR_HOTLIGHT),
            /*close_hover_bg*/ RGB(0xC4, 0x2B, 0x1C),
            /*close_hover_fg*/ RGB(0xFF, 0xFF, 0xFF),
            /*close_fg*/       RGB(0xC8, 0xC8, 0xC8),
        };
    }
    return {
        /*bg_normal*/      RGB(0xF3, 0xF3, 0xF3),
        /*bg_hover*/       RGB(0xEA, 0xEA, 0xEA),
        /*bg_active*/      RGB(0xFF, 0xFF, 0xFF),
        /*text_inactive*/  RGB(0x60, 0x60, 0x60),
        /*text_active*/    RGB(0x1C, 0x1C, 0x1C),
        /*separator*/      RGB(0xD8, 0xD8, 0xD8),
        /*accent*/         GetSysColor(COLOR_HOTLIGHT),
        /*close_hover_bg*/ RGB(0xE8, 0x11, 0x23),
        /*close_hover_fg*/ RGB(0xFF, 0xFF, 0xFF),
        /*close_fg*/       RGB(0x50, 0x50, 0x50),
    };
}

bool detect_dark_mode(HWND hwnd) {
    // 1. DWM immersive-dark-mode attr (returns BOOL for this window).
    BOOL dark = FALSE;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd,
            DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark)))) {
        if (dark) return true;
    }
    // 2. AppsUseLightTheme registry (system-wide app mode).
    HKEY hk = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            0, KEY_READ, &hk) == ERROR_SUCCESS) {
        DWORD val = 1, cb = sizeof(val);
        LONG r = RegQueryValueExW(hk, L"AppsUseLightTheme", nullptr, nullptr,
                                  reinterpret_cast<LPBYTE>(&val), &cb);
        RegCloseKey(hk);
        if (r == ERROR_SUCCESS) return val == 0;
    }
    // 3. Fallback: assume light.
    return false;
}
```

Add these includes near the top of the .cpp:

```cpp
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")
```

Extend `Impl`:

```cpp
    bool dark_mode = false;
    Palette palette = make_palette(false);
```

And in the ctor, after `ensure_fonts`:

```cpp
    impl_->dark_mode = detect_dark_mode(parent);
    impl_->palette   = make_palette(impl_->dark_mode);
```

**Step 4.2: Build**

```bash
cmake --build build --config Release
```

**Step 4.3: Commit**

```bash
git add src/ui/TabManager.cpp
git commit -m "feat(ui): dark-mode detection + Palette struct

Queries DWMWA_USE_IMMERSIVE_DARK_MODE, falls back to the
AppsUseLightTheme registry value, and finally assumes light. The
resulting Palette struct holds every color the paint code will need
(background / text / separator / accent / close-button states) for
both light and dark themes. Not yet consumed by paint; Task 5 wires
it in."
```

---

## Task 5: Real paint — palette-driven backgrounds, active accent bar, bold active text

**Goal:** Replace the Task 2 placeholder paint with the final normal/active visual: palette-driven background, 2 px top accent bar on active, bold font on active text, inactive text in muted color.

**Files:**
- Modify: `src/ui/TabManager.cpp` — replace `paint_tab_minimal` with `paint_tab` that takes palette + fonts.

**Step 5.1: Replace the minimal paint helper**

Delete `paint_tab_minimal`. Add (still in the anonymous namespace):

```cpp
enum class TabVisualState { Normal, Hover, Active };

struct PaintCtx {
    const Palette& palette;
    HFONT font_normal;
    HFONT font_bold;
    UINT  dpi;
};

void paint_tab(const DRAWITEMSTRUCT* dis, const std::wstring& label,
               TabVisualState state, const PaintCtx& pc) {
    HDC hdc = dis->hDC;
    RECT rc = dis->rcItem;

    COLORREF bg = pc.palette.bg_normal;
    if (state == TabVisualState::Hover)  bg = pc.palette.bg_hover;
    if (state == TabVisualState::Active) bg = pc.palette.bg_active;

    HBRUSH brush = CreateSolidBrush(bg);
    FillRect(hdc, &rc, brush);
    DeleteObject(brush);

    // Top accent bar on active tab.
    if (state == TabVisualState::Active) {
        const int bar_h = MulDiv(2, static_cast<int>(pc.dpi), 96);
        RECT bar = rc;
        bar.bottom = bar.top + bar_h;
        HBRUSH accent = CreateSolidBrush(pc.palette.accent);
        FillRect(hdc, &bar, accent);
        DeleteObject(accent);
    }

    // Label.
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, (state == TabVisualState::Active)
        ? pc.palette.text_active : pc.palette.text_inactive);
    HFONT chosen = (state == TabVisualState::Active) ? pc.font_bold
                                                     : pc.font_normal;
    HGDIOBJ old_font = SelectObject(hdc, chosen);

    const int pad = MulDiv(kTabPaddingDip, static_cast<int>(pc.dpi), 96);
    RECT text_rc = rc;
    text_rc.left  += pad;
    text_rc.right -= pad;  // close button reservation added in Task 8
    DrawTextW(hdc, label.c_str(), -1, &text_rc,
              DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS
                  | DT_NOPREFIX);

    SelectObject(hdc, old_font);
}
```

**Step 5.2: Rewrite `handle_draw_item` to use it**

```cpp
bool TabManager::handle_draw_item(const DRAWITEMSTRUCT* dis) {
    if (!impl_ || !dis || dis->hwndItem != impl_->hwnd) return false;
    const int idx = static_cast<int>(dis->itemID);
    if (idx < 0 || idx >= count()) return false;
    auto* tab = impl_->list.at(static_cast<std::size_t>(idx));
    if (!tab) return false;

    const UINT dpi = GetDpiForWindow(impl_->hwnd);
    impl_->ensure_fonts(dpi);

    const bool is_active = (dis->itemState & ODS_SELECTED) != 0;
    TabVisualState state = is_active ? TabVisualState::Active
                                     : TabVisualState::Normal;
    // Hover handling lands in Task 7.

    PaintCtx pc { impl_->palette, impl_->font_normal.get(),
                  impl_->font_bold.get(), dpi };
    paint_tab(dis, tab->label, state, pc);
    return true;
}
```

**Step 5.3: Build + smoke-test**

```bash
cmake --build build --config Release
```
Launch with 2–3 PDFs. Active tab should now show a blue accent bar on top, bolder + darker text, and lighter background. Inactive tabs show muted grey text on pale background.

**Step 5.4: Commit**

```bash
git add src/ui/TabManager.cpp
git commit -m "feat(ui): palette-driven tab paint with active accent bar

Replaces the placeholder paint with the final active/inactive visual:
2 dip top accent bar using COLOR_HOTLIGHT, semibold text on active
vs muted grey on inactive, flat background in three palette tones.
Hover state is defined but not yet driven (Task 7 wires hover
tracking)."
```

---

## Task 6: Separators between adjacent inactive tabs

**Goal:** Draw a 1 px vertical separator at the right edge of each inactive tab, using the palette's `separator` color. Skip separator on the active tab and on the last tab.

**Files:**
- Modify: `src/ui/TabManager.cpp` — extend `paint_tab` with a separator clause.

**Step 6.1: Extend `paint_tab` signature**

Add a `bool draw_right_separator` parameter:

```cpp
void paint_tab(const DRAWITEMSTRUCT* dis, const std::wstring& label,
               TabVisualState state, bool draw_right_separator,
               const PaintCtx& pc);
```

Inside, at the end of the function:

```cpp
    if (draw_right_separator && state != TabVisualState::Active) {
        const int sep_w = 1;  // always 1 px, independent of DPI (per Win11 UX)
        RECT sep = rc;
        sep.left = rc.right - sep_w;
        const int inset = MulDiv(6, static_cast<int>(pc.dpi), 96);
        sep.top    += inset;
        sep.bottom -= inset;
        HBRUSH b = CreateSolidBrush(pc.palette.separator);
        FillRect(hdc, &sep, b);
        DeleteObject(b);
    }
```

**Step 6.2: Update caller**

In `handle_draw_item`:

```cpp
    const bool has_next = (idx + 1) < count();
    const bool next_is_active = has_next && (idx + 1 == active_index());
    const bool draw_sep = has_next && !is_active && !next_is_active;
    paint_tab(dis, tab->label, state, draw_sep, pc);
```

**Step 6.3: Build + smoke-test**

Launch with 3 PDFs; verify a subtle vertical line appears between each pair of adjacent inactive tabs and disappears on either side of the active tab.

**Step 6.4: Commit**

```bash
git add src/ui/TabManager.cpp
git commit -m "feat(ui): 1px vertical separator between adjacent inactive tabs

Separator is suppressed on the active tab and on the tab immediately
preceding the active one, matching Chrome/Edge behavior where the
active tab 'breaks' the divider flow. Inset 6 dip top+bottom so the
line is visually shorter than the tab to match Fluent tab strips."
```

---

## Task 7: Hover tracking (WM_MOUSEMOVE + TrackMouseEvent + WM_MOUSELEAVE)

**Goal:** When the mouse moves over an inactive tab, that tab repaints with the hover background. Leaving the control repaints the previously-hovered tab back to normal.

**Files:**
- Modify: `src/ui/TabManager.cpp` — add `hovered_tab` + `mouse_tracking` to Impl, handle `WM_MOUSEMOVE` and `WM_MOUSELEAVE` in `tab_subclass_proc`, thread hover state into `handle_draw_item`.

**Step 7.1: Extend Impl**

```cpp
    int  hovered_tab   = -1;
    bool mouse_tracking = false;

    RECT tab_rect(int i) const {
        RECT r = {};
        if (hwnd && i >= 0) {
            SendMessageW(hwnd, TCM_GETITEMRECT, static_cast<WPARAM>(i),
                         reinterpret_cast<LPARAM>(&r));
        }
        return r;
    }
    void invalidate_tab(int i) {
        if (!hwnd || i < 0) return;
        RECT r = tab_rect(i);
        InvalidateRect(hwnd, &r, FALSE);
    }
```

**Step 7.2: Handle mouse events in `tab_subclass_proc`**

Add cases alongside the existing `WM_MBUTTONDOWN`:

```cpp
        case WM_MOUSEMOVE: {
            TCHITTESTINFO hti = {};
            hti.pt.x = GET_X_LPARAM(l);
            hti.pt.y = GET_Y_LPARAM(l);
            const int i = static_cast<int>(
                SendMessageW(hwnd, TCM_HITTEST, 0,
                             reinterpret_cast<LPARAM>(&hti)));
            if (i != impl.hovered_tab) {
                const int old_hover = impl.hovered_tab;
                impl.hovered_tab = i;
                if (old_hover >= 0) impl.invalidate_tab(old_hover);
                if (i >= 0) impl.invalidate_tab(i);
            }
            if (!impl.mouse_tracking) {
                TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
                TrackMouseEvent(&tme);
                impl.mouse_tracking = true;
            }
            break;
        }
        case WM_MOUSELEAVE: {
            impl.mouse_tracking = false;
            const int old_hover = impl.hovered_tab;
            impl.hovered_tab = -1;
            if (old_hover >= 0) impl.invalidate_tab(old_hover);
            break;
        }
```

**Step 7.3: Use hover state in `handle_draw_item`**

Replace the `state` computation:

```cpp
    TabVisualState state = TabVisualState::Normal;
    if (is_active) {
        state = TabVisualState::Active;
    } else if (idx == impl_->hovered_tab) {
        state = TabVisualState::Hover;
    }
```

**Step 7.4: Build + smoke-test**

Launch with 3 PDFs. Move cursor over inactive tabs — each should tint to `bg_hover` while hovered, revert on leave. Moving onto active tab should not change it.

**Step 7.5: Commit**

```bash
git add src/ui/TabManager.cpp
git commit -m "feat(ui): hover tracking for tab strip

WM_MOUSEMOVE hit-tests with TCM_HITTEST, invalidates the two affected
tab rects on transition, and arms TME_LEAVE. WM_MOUSELEAVE clears the
hover index. Hover state is consumed by paint_tab via the existing
TabVisualState enum. Active tab is immune to hover tinting (matches
Chrome/Edge)."
```

---

## Task 8: Close button paint + hit-test + click handling

**Goal:** Draw a 14×14 `×` at the right edge of the active tab and the hovered tab, with a red-background hover state on the button itself; clicking the button fires `on_close_request` and does not change active tab.

**Files:**
- Modify: `src/ui/TabManager.cpp` — geometry helpers, paint extension, WM_LBUTTONDOWN/UP handling, Impl state.

**Step 8.1: Extend Impl**

```cpp
    bool hovered_close  = false;
    int  pressed_close  = -1;
```

**Step 8.2: Add geometry helper to anonymous namespace**

```cpp
RECT close_rect_in(const RECT& tab, UINT dpi) {
    const int sz  = MulDiv(kCloseSizeDip, static_cast<int>(dpi), 96);
    const int pad = MulDiv(kCloseRightPadDip, static_cast<int>(dpi), 96);
    const int cy  = (tab.top + tab.bottom) / 2;
    RECT r;
    r.right  = tab.right - pad;
    r.left   = r.right - sz;
    r.top    = cy - sz / 2;
    r.bottom = r.top + sz;
    return r;
}
```

**Step 8.3: Extend `paint_tab` to draw the close button**

Add a new parameter `bool show_close, bool close_is_hot`. Reserve space for the button in the text rect (update the `text_rc.right -= pad;` line):

```cpp
    const int pad_close = show_close
        ? MulDiv(kCloseSizeDip + kCloseRightPadDip + 6, /*gap*/
                 static_cast<int>(pc.dpi), 96)
        : pad;
    text_rc.right = rc.right - pad_close;
```

After drawing the label, before the separator branch:

```cpp
    if (show_close) {
        RECT cb = close_rect_in(rc, pc.dpi);
        if (close_is_hot) {
            HBRUSH hb = CreateSolidBrush(pc.palette.close_hover_bg);
            FillRect(hdc, &cb, hb);
            DeleteObject(hb);
        }
        COLORREF x_color = close_is_hot ? pc.palette.close_hover_fg
                                        : pc.palette.close_fg;
        HPEN pen = CreatePen(PS_SOLID,
                             MulDiv(1, static_cast<int>(pc.dpi), 96),
                             x_color);
        HGDIOBJ old_pen = SelectObject(hdc, pen);
        const int inset = MulDiv(3, static_cast<int>(pc.dpi), 96);
        MoveToEx(hdc, cb.left + inset, cb.top + inset, nullptr);
        LineTo  (hdc, cb.right - inset, cb.bottom - inset);
        MoveToEx(hdc, cb.right - inset, cb.top + inset, nullptr);
        LineTo  (hdc, cb.left + inset, cb.bottom - inset);
        SelectObject(hdc, old_pen);
        DeleteObject(pen);
    }
```

**Step 8.4: Pass show_close + close_is_hot from `handle_draw_item`**

```cpp
    const bool show_close = is_active ||
                            (idx == impl_->hovered_tab);
    const bool close_is_hot = show_close &&
                              (idx == impl_->hovered_tab) &&
                              impl_->hovered_close;
    paint_tab(dis, tab->label, state, draw_sep, show_close, close_is_hot, pc);
```

**Step 8.5: Track close-button hover inside WM_MOUSEMOVE**

In the `WM_MOUSEMOVE` branch, after updating `impl.hovered_tab`:

```cpp
            bool new_close_hover = false;
            if (i >= 0) {
                RECT tr = impl.tab_rect(i);
                RECT cb = close_rect_in(tr, GetDpiForWindow(hwnd));
                POINT pt = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
                new_close_hover = PtInRect(&cb, pt);
            }
            if (new_close_hover != impl.hovered_close) {
                impl.hovered_close = new_close_hover;
                if (i >= 0) impl.invalidate_tab(i);
            }
```

Also in `WM_MOUSELEAVE`: `impl.hovered_close = false;`.

**Step 8.6: Handle WM_LBUTTONDOWN / WM_LBUTTONUP**

Add cases in `tab_subclass_proc`:

```cpp
        case WM_LBUTTONDOWN: {
            TCHITTESTINFO hti = {};
            hti.pt.x = GET_X_LPARAM(l);
            hti.pt.y = GET_Y_LPARAM(l);
            const int i = static_cast<int>(
                SendMessageW(hwnd, TCM_HITTEST, 0,
                             reinterpret_cast<LPARAM>(&hti)));
            if (i >= 0) {
                RECT tr = impl.tab_rect(i);
                RECT cb = close_rect_in(tr, GetDpiForWindow(hwnd));
                POINT pt = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
                if (PtInRect(&cb, pt)) {
                    impl.pressed_close = i;
                    SetCapture(hwnd);
                    impl.invalidate_tab(i);
                    return 0;  // block tab-select
                }
            }
            break;  // fall through to DefSubclassProc for tab-select
        }
        case WM_LBUTTONUP: {
            if (impl.pressed_close >= 0) {
                const int pressed = impl.pressed_close;
                impl.pressed_close = -1;
                if (GetCapture() == hwnd) ReleaseCapture();
                TCHITTESTINFO hti = {};
                hti.pt.x = GET_X_LPARAM(l);
                hti.pt.y = GET_Y_LPARAM(l);
                const int released_on = static_cast<int>(
                    SendMessageW(hwnd, TCM_HITTEST, 0,
                                 reinterpret_cast<LPARAM>(&hti)));
                if (released_on == pressed) {
                    RECT tr = impl.tab_rect(released_on);
                    RECT cb = close_rect_in(tr, GetDpiForWindow(hwnd));
                    POINT pt = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
                    if (PtInRect(&cb, pt)) {
                        impl.fire_close_request(released_on);
                    }
                }
                return 0;
            }
            break;
        }
```

Add a `WM_CAPTURECHANGED` reset for `pressed_close` too:

```cpp
        case WM_CAPTURECHANGED:
            impl.pressed_tab   = -1;
            impl.pressed_close = -1;
            break;
```

**Step 8.7: Build + smoke-test**

Launch with 3 PDFs. Verify:
- Active tab shows × permanently; hovering it turns it red.
- Hovering an inactive tab reveals its × and tints tab bg.
- Clicking an inactive tab's × closes it without changing active tab.
- Clicking the tab body (not ×) still switches active.
- Middle-click still closes (regression).

**Step 8.8: Commit**

```bash
git add src/ui/TabManager.cpp
git commit -m "feat(ui): per-tab close button with hover state

Adds a 14x14 × at the right edge of the active and hovered tabs, with
a red hover background on the button itself. Close-button hit-test
takes precedence over tab-select: a DOWN/UP gesture on the button
fires on_close_request and does not change the active tab. The
pressed_close state mirrors the existing middle-click pressed_tab
discipline (WM_CAPTURECHANGED cancels the gesture if capture is
stolen). Label right-padding is expanded to reserve space for the
button so the ellipsis lands before it."
```

---

## Task 9: Hot-switch dark mode on WM_SETTINGCHANGE

**Goal:** Flip palette live when the user toggles Windows dark mode while litepdf is running.

**Files:**
- Modify: `src/ui/TabManager.cpp` — implement `handle_theme_change` body.

**Step 9.1: Implement the method**

```cpp
void TabManager::handle_theme_change() {
    if (!impl_ || !impl_->hwnd) return;
    HWND parent = GetParent(impl_->hwnd);
    const bool new_dark = detect_dark_mode(parent ? parent : impl_->hwnd);
    if (new_dark == impl_->dark_mode) return;
    impl_->dark_mode = new_dark;
    impl_->palette   = make_palette(new_dark);
    InvalidateRect(impl_->hwnd, nullptr, TRUE);
}
```

**Step 9.2: Build + smoke-test**

Launch litepdf with 2 PDFs. Keep it open. In Windows Settings → Personalization → Colors, toggle "Choose your mode" between Light and Dark. The tab strip should immediately repaint into the other palette without restarting the app.

**Step 9.3: Commit**

```bash
git add src/ui/TabManager.cpp
git commit -m "feat(ui): hot-switch tab strip palette on WM_SETTINGCHANGE

MainWindow already forwards ImmersiveColorSet-filtered
WM_SETTINGCHANGE notifications (landed in Task 1); this fills in the
TabManager side so a Windows dark-mode toggle repaints the strip
live. detect_dark_mode runs against the parent HWND because DWM's
immersive-dark attribute is a per-top-level-window property."
```

---

## Task 10: Manual QA pass + bug fixes

**Goal:** Walk through the 12-point QA checklist from the design doc, fix any regressions found, and leave the working tree clean.

**Step 10.1: Run the full manual QA checklist**

Reference: `docs/plans/2026-04-19-tab-strip-polish-design.md` → "Testing" section. For each of the 12 items, launch `build/Release/litepdf.exe tests/fixtures/simple.pdf` (add more via File → Open) and verify:

1. 1 tab: strip visible, bounded, active obvious.
2. 3 tabs: 1 px separator between adjacent inactives, suppressed around active.
3. Hover inactive: bg tints + × appears.
4. Hover ×: red with white ×.
5. Click ×: closes, doesn't change active.
6. Click tab body: switches active.
7. Middle-click: closes (regression).
8. Ctrl+W: closes active (regression).
9. Ctrl+Tab / Ctrl+Shift+Tab: cycle active (regression).
10. Long filename: `…` truncation.
11. Dark-mode toggle: live repaint without restart.
12. HiDPI: height/font/× scale correctly (drag to a HiDPI monitor if available; else document "not tested on HiDPI in this pass").

**Step 10.2: Run full test suite**

```bash
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```
Expected: all green, including the 9 TabList unit tests.

**Step 10.3: Fix any bugs found**

For each bug, prefer minimal commits (one bug = one commit). Typical gotchas to watch:

- On WM_LBUTTONDOWN that is not on the close button, make sure we still fall through to DefSubclassProc so TCM handles tab-select.
- `DT_END_ELLIPSIS` on Chinese labels: verify `測試.pdf` does not draw a partial glyph before the ellipsis.
- HiDPI: if font looks blurry, ensure `cached_dpi` is being compared against `GetDpiForWindow(impl_->hwnd)` (not the window's DPI).
- Dark-mode toggle: make sure WM_SETTINGCHANGE lParam string pointer is validated before `wcscmp` (can be 0 for some broadcast variants).

**Step 10.4: Update design doc (optional)**

If QA surfaced anything worth recording (e.g. "item 12 not tested — no HiDPI monitor available"), append a short note under "Open questions" in the design doc.

**Step 10.5: Final commit (if any fixes)**

```bash
git add -p  # review every hunk
git commit -m "fix(ui): <short description of the bug fixed in QA>"
```

**Step 10.6: Prepare PR**

```bash
git log --oneline main..HEAD
```
Should show 9-10 small commits. Ready for `/ship` or `superpowers:finishing-a-development-branch` (choose one, not both — per CLAUDE.md).

---

## Appendix: File-and-line summary

- `src/ui/TabManager.hpp` — +2 method decls (Task 1).
- `src/ui/TabManager.cpp` — ~215 LOC over Tasks 1–9:
  - anonymous-namespace constants + RAII alias + palette + detection + paint helpers
  - Impl: fonts, dark-mode, palette, hover state, press state
  - ctor: owner-draw flag, fonts, ITEMSIZE, theme detection
  - handle_draw_item, handle_theme_change
  - tab_subclass_proc: WM_MOUSEMOVE, WM_MOUSELEAVE, WM_LBUTTONDOWN, WM_LBUTTONUP, WM_CAPTURECHANGED additions
  - strip_height: use our constant
- `src/ui/MainWindow.cpp` — +~15 LOC (Task 1): WM_DRAWITEM + WM_SETTINGCHANGE delegates.
- `src/core/TabList.*` — unchanged.
- `tests/unit/TabListTests.cpp` — unchanged; must remain green throughout.
