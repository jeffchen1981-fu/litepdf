#include "ui/ThumbnailPane.hpp"

#include "core/ThumbCache.hpp"
#include "core/ThumbnailModel.hpp"
#include "core/ThumbnailRenderer.hpp"

#include <commctrl.h>
#include <dwmapi.h>
#include <windowsx.h>

#include <algorithm>
#include <atomic>
#include <cwchar>
#include <stdexcept>
#include <string>
#include <unordered_set>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "dwmapi.lib")

namespace litepdf::ui {

namespace {

// Distinct IDs so the ListView subclass + the parent-WM_DRAWITEM subclass
// can coexist with TabManager's parent-side WM_DRAWITEM handler. Using
// hand-picked magic numbers (mirrors TabManager's kTabSubclassId style).
//
// kListSubclassId is shared safely across instances because each pane
// owns its own list HWND — SetWindowSubclass keys on (window, proc, id)
// so different windows don't collide.
//
// The parent subclass id, by contrast, MUST be unique per ThumbnailPane:
// per D11 each tab gets its own pane and they all subclass the SAME
// MainWindow HWND. Reusing one id across instances would make the second
// SetWindowSubclass overwrite the first pane's ref_data (MSDN: "If this
// function is called twice with the same uIdSubclass and pfnSubclass
// values, the second call replaces the data..."), and the dtor's
// RemoveWindowSubclass would strip a sibling pane's hook. We hand out
// fresh ids from an atomic counter so multi-tab is correct by design.
constexpr UINT_PTR kListSubclassId = 0xAB02;
std::atomic<UINT_PTR> g_next_parent_subclass_id{0xAB03};

// T6 worker->UI marshalling. The renderer's on_done callback fires on a
// worker thread; it cannot touch HWND / HBITMAP cache / pending_renders_
// directly, so it PostMessageWs this user message back to the pane's list
// HWND. The UI thread handler unpacks (page, HBITMAP) and updates state
// atomically with respect to other UI-thread work.
//
//   wParam = page index (int)
//   lParam = HBITMAP (may be null for cancelled/failed renders; the handler
//            still erases the page from pending_renders_ but does NOT put a
//            null HBITMAP into the cache)
constexpr UINT WM_USER_THUMB_READY = WM_USER + 17;

// Tile geometry: matches ThumbnailModel defaults (120x160 dip) plus a small
// margin on each edge and a single label line below the placeholder.
constexpr int kTileMarginDip   = 6;   // padding inside each row, all sides
constexpr int kLabelHeightDip  = 16;  // height of the "Page N" label line
constexpr int kBorderWidthDip  = 2;   // current-page highlight border width
constexpr int kPlaceholderInsetDip = 1;  // 1-px outline around the tile box

struct Palette {
    COLORREF pane_bg;       // pane background (around tiles)
    COLORREF tile_fill;     // placeholder rectangle fill
    COLORREF tile_border;   // placeholder rectangle outline
    COLORREF text;          // page-number label color
    COLORREF current_border;// current-page highlight border (uses accent)
};

// M2 carry-over: pre-created GDI brushes for the four palette colors used
// per row paint. Defined at namespace scope (rather than nested inside
// Impl) so the anonymous-namespace painters can take it by reference
// without having to friend `Impl`. Owned by Impl; rebuilt whenever
// `palette` changes (ctor / on_dpi_changed / future WM_SETTINGCHANGE).
struct PaletteBrushes {
    HBRUSH pane_bg        = nullptr;
    HBRUSH tile_fill      = nullptr;
    HBRUSH tile_border    = nullptr;
    HBRUSH current_border = nullptr;
};

COLORREF resolve_accent_color() {
    DWORD argb = 0;
    BOOL  opaque = FALSE;
    if (SUCCEEDED(DwmGetColorizationColor(&argb, &opaque))) {
        const BYTE r = (argb >> 16) & 0xFF;
        const BYTE g = (argb >>  8) & 0xFF;
        const BYTE b = (argb >>  0) & 0xFF;
        return RGB(r, g, b);
    }
    return GetSysColor(COLOR_HOTLIGHT);
}

Palette make_palette(bool dark) {
    if (dark) {
        return {
            /*pane_bg*/        RGB(0x1F, 0x1F, 0x1F),
            /*tile_fill*/      RGB(0x2D, 0x2D, 0x2D),
            /*tile_border*/    RGB(0x55, 0x55, 0x55),
            /*text*/           RGB(0xE0, 0xE0, 0xE0),
            /*current_border*/ resolve_accent_color(),
        };
    }
    return {
        /*pane_bg*/        RGB(0xF5, 0xF5, 0xF5),
        /*tile_fill*/      RGB(0xFF, 0xFF, 0xFF),
        /*tile_border*/    RGB(0xC8, 0xC8, 0xC8),
        /*text*/           RGB(0x30, 0x30, 0x30),
        /*current_border*/ resolve_accent_color(),
    };
}

bool detect_dark_mode(HWND hwnd) {
    BOOL dark = FALSE;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd,
            DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark)))) {
        if (dark) return true;
    }
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
    return false;
}

inline int dp(int dip, unsigned dpi) {
    return MulDiv(dip, static_cast<int>(dpi), 96);
}

// Total row pitch in pixels — matches what we tell ListView via
// LVM_SETITEMHEIGHT and what ThumbnailModel uses for tile geometry.
int row_height_px(unsigned dpi, int tile_h_px) {
    return tile_h_px + 2 * dp(kTileMarginDip, dpi) + dp(kLabelHeightDip, dpi);
}

}  // namespace

// Forward-declared at namespace scope so the friend access from the subclass
// procs is well-formed by name.
LRESULT CALLBACK thumb_list_subclass_proc(HWND, UINT, WPARAM, LPARAM,
                                          UINT_PTR, DWORD_PTR);
LRESULT CALLBACK thumb_parent_subclass_proc(HWND, UINT, WPARAM, LPARAM,
                                            UINT_PTR, DWORD_PTR);

struct ThumbnailPane::Impl {
    HWND                       list_hwnd = nullptr;
    HWND                       parent    = nullptr;
    litepdf::core::ThumbnailModel model;
    ThumbnailPane::NavigateCb  on_navigate;

    // Allocated in the ctor from g_next_parent_subclass_id so each pane
    // has its own (proc, id) key on the shared parent HWND. Stored here
    // so the dtor can RemoveWindowSubclass with the same id without
    // stomping on a sibling pane's entry.
    UINT_PTR                   parent_subclass_id = 0;

    // Populated by T6's set_cache / set_renderer. Held as nullable raw
    // pointers because the cache + renderer are owned by DocumentView and
    // outlive the pane (D6/D7). on_dpi_changed nullptr-guards both so it
    // is correct to call before T6 has wired them.
    litepdf::core::ThumbCache*        cache    = nullptr;
    litepdf::core::ThumbnailRenderer* renderer = nullptr;

    bool    dark_mode = false;
    Palette palette   = make_palette(false);
    UINT    cached_dpi = 96;

    // M2 carry-over: pre-create the four brushes paint_placeholder /
    // paint_thumb use on every row paint. Without caching, a 30-tile
    // repaint at 4K DPI would CreateSolidBrush / DeleteObject 120-150
    // times per pass; the GDI object table churn is measurable. Rebuilt
    // whenever the palette changes (ctor, on_dpi_changed, future
    // WM_SETTINGCHANGE for dark-mode flip). Type lives at namespace scope
    // so the anonymous-namespace painters can accept it by reference.
    PaletteBrushes brushes_;

    // T6: the set of pages for which a render is currently in flight (i.e.
    // submitted to renderer_ but no WM_USER_THUMB_READY has come back yet).
    // UI-thread-only: mutated at exactly six points (WM_DRAWITEM submit
    // path, WM_USER_THUMB_READY handler, set_page_count, clear/hide, dtor).
    // Worker threads NEVER touch this set.
    std::unordered_set<int> pending_renders_;

    void rebuild_brushes() {
        delete_brushes();
        brushes_.pane_bg        = CreateSolidBrush(palette.pane_bg);
        brushes_.tile_fill      = CreateSolidBrush(palette.tile_fill);
        brushes_.tile_border    = CreateSolidBrush(palette.tile_border);
        brushes_.current_border = CreateSolidBrush(palette.current_border);
    }

    void delete_brushes() {
        HBRUSH* slots[] = {
            &brushes_.pane_bg,
            &brushes_.tile_fill,
            &brushes_.tile_border,
            &brushes_.current_border,
        };
        for (HBRUSH* slot : slots) {
            if (*slot) {
                DeleteObject(*slot);
                *slot = nullptr;
            }
        }
    }

    // Cancel any in-flight renders and drop the pending set. Called at
    // every pending_renders_ "erase + cancel" point (set_page_count,
    // hide, dtor — and conceptually clear() once that lands). The
    // renderer pointer is nullptr-guarded because callers (T8 controller)
    // may legitimately set_page_count before set_renderer.
    void cancel_in_flight() {
        if (renderer) renderer->cancel_pending();
        pending_renders_.clear();
    }

    // Compute the desired row height for the current DPI / tile size.
    // The actual row height is set via WM_MEASUREITEM (reflected from
    // parent) which the parent subclass proc fills in — LVM_SETITEMHEIGHT
    // is comctl32-v6-only and not exposed by the SDK we ship against.
    int row_height() const {
        return row_height_px(cached_dpi, model.tile_h_px());
    }

    // Force the listview to re-query WM_MEASUREITEM after a row-height
    // change (DPI, etc.). The standard trick is to call SetRedraw(false /
    // true) around an item count refresh; simpler: nudge the column width
    // which makes ListView re-layout. As a final stop-gap we invalidate
    // the whole window so the new row height takes effect on the next
    // paint cycle.
    void refresh_item_height() {
        if (!list_hwnd) return;
        // Toggle a no-op style change to flush the cached metric. This is
        // the most portable equivalent of LVM_SETITEMHEIGHT before v6.
        const LONG_PTR style = GetWindowLongPtrW(list_hwnd, GWL_STYLE);
        SetWindowLongPtrW(list_hwnd, GWL_STYLE, style);
        SetWindowPos(list_hwnd, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER
                         | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        InvalidateRect(list_hwnd, nullptr, TRUE);
    }

    // Re-stretch the single column to fill the client width. ListView
    // owner-draw paints whatever rcItem we give it, so the column width
    // gates the per-row paint rectangle.
    void sync_column_width() {
        if (!list_hwnd) return;
        RECT rc{};
        GetClientRect(list_hwnd, &rc);
        const int w = std::max(0, static_cast<int>(rc.right - rc.left));
        ListView_SetColumnWidth(list_hwnd, 0, w);
    }

    // Push the model's current scroll_y to the listview's scrollbar so the
    // thumb position matches the model state (e.g. after set_scroll_y_px
    // clamps on resize).
    void sync_scroll_pos() {
        if (!list_hwnd) return;
        SCROLLINFO si{};
        si.cbSize = sizeof(si);
        si.fMask  = SIF_POS;
        si.nPos   = model.scroll_y_px();
        SetScrollInfo(list_hwnd, SB_VERT, &si, TRUE);
    }
};

ThumbnailPane::ThumbnailPane(HINSTANCE hInstance, HWND parent)
    : impl_(std::make_unique<Impl>())
{
    impl_->parent = parent;

    // ListView style: report (single column, vertical scroll), owner-draw
    // (we paint placeholder + page-number ourselves), no header (single
    // column has nothing to label), single-select keeps the highlighted
    // row obvious.
    impl_->list_hwnd = CreateWindowExW(
        WS_EX_CLIENTEDGE,
        WC_LISTVIEWW,
        L"",
        WS_CHILD | WS_VSCROLL | WS_TABSTOP
            | LVS_REPORT | LVS_OWNERDRAWFIXED | LVS_NOCOLUMNHEADER
            | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
        0, 0, 200, 400,
        parent,
        nullptr,
        hInstance,
        nullptr);
    if (!impl_->list_hwnd)
        throw std::runtime_error("Failed to create ThumbnailPane ListView HWND");

    ListView_SetExtendedListViewStyle(impl_->list_hwnd, LVS_EX_DOUBLEBUFFER);

    // Cache DPI + dark-mode state for the palette. detect_dark_mode reads
    // from the parent so the pane matches the rest of the chrome.
    impl_->cached_dpi = GetDpiForWindow(impl_->list_hwnd);
    impl_->dark_mode  = detect_dark_mode(parent);
    impl_->palette    = make_palette(impl_->dark_mode);
    impl_->rebuild_brushes();  // M2: cache GDI brushes, refreshed on DPI / theme change.

    ListView_SetBkColor    (impl_->list_hwnd, impl_->palette.pane_bg);
    ListView_SetTextBkColor(impl_->list_hwnd, impl_->palette.pane_bg);
    ListView_SetTextColor  (impl_->list_hwnd, impl_->palette.text);

    // Single full-width column (LVCF_WIDTH only — no header text needed
    // since LVS_NOCOLUMNHEADER hides the bar).
    LVCOLUMNW col{};
    col.mask = LVCF_WIDTH | LVCF_FMT;
    col.fmt  = LVCFMT_LEFT;
    col.cx   = 200;  // sync_column_width will stretch this on first WM_SIZE
    ListView_InsertColumn(impl_->list_hwnd, 0, &col);

    // Seed model defaults at the pane's current DPI. tile_dip / gap_dip
    // come from ThumbnailModel's defaults (120x160 dip, 8 dip gap).
    // Row height itself is communicated to the listview via WM_MEASUREITEM
    // which the parent subclass fills in from impl_->row_height(); see
    // thumb_parent_subclass_proc.
    impl_->model.set_dpi(impl_->cached_dpi);

    // Subclass the listview for WM_LBUTTONDOWN, WM_VSCROLL, WM_SIZE,
    // WM_MOUSEWHEEL. Subclass the parent for WM_DRAWITEM (LVS_OWNERDRAWFIXED
    // reflects WM_DRAWITEM up to the parent — we tap that path with a
    // separate subclass ID so MainWindow's existing WM_DRAWITEM handler for
    // the tab strip continues to run via DefSubclassProc).
    SetWindowSubclass(impl_->list_hwnd, thumb_list_subclass_proc,
                      kListSubclassId,
                      reinterpret_cast<DWORD_PTR>(this));
    impl_->parent_subclass_id =
        g_next_parent_subclass_id.fetch_add(1, std::memory_order_relaxed);
    SetWindowSubclass(parent, thumb_parent_subclass_proc,
                      impl_->parent_subclass_id,
                      reinterpret_cast<DWORD_PTR>(this));
}

ThumbnailPane::~ThumbnailPane() {
    if (impl_) {
        // pending_renders_ mutation point #6 (dtor): cancel before tearing
        // down the HWND so any worker that has not yet PostMessageWd a
        // WM_USER_THUMB_READY observes the cancel and produces nullptr
        // HBITMAPs (D17). DocumentView (T8) is responsible for ordering
        // ~ThumbnailPane *before* ~ThumbnailRenderer so renderer_ is still
        // valid here; the renderer's own dtor then drains its workers
        // (D16) so by the time ~ThumbnailRenderer returns, no PostMessageW
        // is in flight that could target the freed HWND.
        if (impl_->renderer) impl_->renderer->cancel_pending();
        impl_->pending_renders_.clear();

        if (impl_->parent) {
            RemoveWindowSubclass(impl_->parent, thumb_parent_subclass_proc,
                                 impl_->parent_subclass_id);
        }
        if (impl_->list_hwnd) {
            RemoveWindowSubclass(impl_->list_hwnd, thumb_list_subclass_proc,
                                 kListSubclassId);
            DestroyWindow(impl_->list_hwnd);
            impl_->list_hwnd = nullptr;
        }
        impl_->delete_brushes();  // M2: release the cached GDI handles.
    }
}

HWND ThumbnailPane::hwnd() const { return impl_ ? impl_->list_hwnd : nullptr; }

void ThumbnailPane::set_on_navigate(NavigateCb cb) {
    impl_->on_navigate = std::move(cb);
}

void ThumbnailPane::set_renderer(litepdf::core::ThumbnailRenderer* renderer) {
    // UI-thread-only. If we're swapping renderers (or clearing to nullptr),
    // any pending requests submitted to the OLD renderer become orphans -
    // their callbacks may still PostMessageW WM_USER_THUMB_READY for the
    // pages we still consider "pending", which would then re-enter the
    // renderer pointer to handle the result. That is fine: the handler
    // just routes the HBITMAP into the cache. But the bookkeeping must
    // stay in sync, so we cancel against the OLD renderer first if any
    // requests are outstanding, then drop the pending set. The new
    // renderer (if non-null) starts with an empty in-flight set; the next
    // WM_DRAWITEM cycle will re-submit cache misses through it.
    if (impl_->renderer && impl_->renderer != renderer) {
        impl_->renderer->cancel_pending();
        impl_->pending_renders_.clear();
    }
    impl_->renderer = renderer;
}

void ThumbnailPane::set_cache(litepdf::core::ThumbCache* cache) {
    // UI-thread-only. Switching caches means the previously-rendered tiles
    // live in a different cache the pane no longer queries; any HBITMAP
    // already produced for the OLD cache will arrive via WM_USER_THUMB_READY
    // and be inserted into the NEW one. That is benign (it's still a valid
    // page->bitmap mapping) but means there's a transient window where the
    // pane paints placeholders for tiles whose old-cache copies still
    // exist. We don't try to migrate; T8 only ever swaps caches as part of
    // a tab-switch, where placeholders flashing momentarily is acceptable.
    impl_->cache = cache;
}

void ThumbnailPane::set_page_count(int n) {
    if (!impl_->list_hwnd) return;
    n = std::max(0, n);

    // pending_renders_ mutation point #3 (document swap / page-count change).
    // Any in-flight render targeting a page that no longer exists in the new
    // document — or even one that exists but at a different rendered scale —
    // is wasted work. D17 guarantees the cancelled callbacks still fire with
    // HBITMAP=nullptr so the renderer's own pending_tasks counter drains.
    impl_->cancel_in_flight();

    impl_->model.set_page_count(n);
    impl_->model.set_scroll_y_px(0);

    // Drop stale thumbs too — the new document's page 0 has a completely
    // different visual content from the old document's page 0. Without this
    // the pane would briefly paint the previous document's thumbs at the
    // new page indices until WM_USER_THUMB_READY messages overwrite them.
    if (impl_->cache) impl_->cache->clear();

    // LVS_OWNERDRAWFIXED still walks the item list to fire WM_DRAWITEM, so
    // we need the listview's item count to match the model's page count.
    // Use ListView_SetItemCountEx with LVSICF_NOSCROLL to avoid the visual
    // jump-to-top artifact, then call SetItemCount-style reset by deleting
    // and reinserting via LVM_SETITEMCOUNT (works for both standard and
    // virtual variants — without LVS_OWNERDATA, this just truncates or
    // expands the item list with placeholder NULL text).
    ListView_DeleteAllItems(impl_->list_hwnd);
    for (int i = 0; i < n; ++i) {
        LVITEMW it{};
        it.mask    = LVIF_TEXT;
        it.iItem   = i;
        // Empty string — owner-draw paints the page number directly from
        // the item index, no need for the listview to store text.
        it.pszText = const_cast<LPWSTR>(L"");
        ListView_InsertItem(impl_->list_hwnd, &it);
    }

    impl_->sync_column_width();
    impl_->sync_scroll_pos();
    InvalidateRect(impl_->list_hwnd, nullptr, TRUE);
}

void ThumbnailPane::set_current_page(int page) {
    if (!impl_->list_hwnd) return;
    auto change = impl_->model.set_current_page(page);
    if (change.first < 0 && change.second < 0) return;  // model no-op

    // Repaint old + new highlight tiles. ListView_RedrawItems handles
    // the row-coordinate translation correctly via the listview's own
    // layout (more robust than InvalidateRect with a model-derived
    // tile_rect, given the model/listview pitch discrepancy).
    if (change.first  >= 0) ListView_RedrawItems(impl_->list_hwnd,
                                                  change.first,  change.first);
    if (change.second >= 0) ListView_RedrawItems(impl_->list_hwnd,
                                                  change.second, change.second);

    // T7: auto-scroll to make the new page visible.
    // Approach A (per plan line 1377):
    //   1. Update the model's scroll_y_ via scroll_to_make_visible —
    //      no-op if already in visible_range (see ThumbnailModel.cpp).
    //   2. Push model.scroll_y_px to the listview's SCROLLINFO via
    //      sync_scroll_pos() so the scrollbar thumb tracks the model.
    //   3. Call ListView_EnsureVisible to move the listview's internal
    //      iTopIndex (SCROLLINFO alone does NOT move items).
    //   4. Invalidate to force a repaint with the new state.
    //
    // CRITICAL: ListView_EnsureVisible does NOT synthesize WM_VSCROLL
    // (verified empirically by reviewer Win32 probe — the listview
    // bypasses the WM_VSCROLL pathway and calls Scroll/Invalidate
    // directly). M4's WM_VSCROLL fall-through therefore does NOT update
    // the model for us, so we must drive the model side explicitly
    // here to keep both sources of truth in sync. Without this, a
    // subsequent WM_MOUSEWHEEL would compute deltas off a stale
    // model.scroll_y_ == 0 and snap the listview back near the top.
    //
    // The prev/new scroll guard preserves the "already visible -> no
    // scroll work" early-exit: scroll_to_make_visible no-ops in that
    // case, leaving scroll_y_ unchanged, and we skip sync/scroll/
    // invalidate. The highlight repaint above still runs, which is
    // correct (the highlight moved even if no scroll was needed).
    if (change.second >= 0) {
        const int prev_scroll = impl_->model.scroll_y_px();
        impl_->model.scroll_to_make_visible(change.second);
        const int new_scroll = impl_->model.scroll_y_px();
        if (new_scroll != prev_scroll) {
            impl_->sync_scroll_pos();
            ListView_EnsureVisible(impl_->list_hwnd, change.second, FALSE);
            InvalidateRect(impl_->list_hwnd, nullptr, FALSE);
        }
    }

    UpdateWindow(impl_->list_hwnd);
}

void ThumbnailPane::show() {
    if (impl_->list_hwnd) ShowWindow(impl_->list_hwnd, SW_SHOW);
}

void ThumbnailPane::hide() {
    if (impl_->list_hwnd) ShowWindow(impl_->list_hwnd, SW_HIDE);
    // pending_renders_ mutation point #5 (pane hidden). The user is no
    // longer looking at the pane; finishing in-flight thumb renders would
    // burn CPU on a hidden tile set. Re-submission is organic: when the
    // pane is shown again, WM_DRAWITEM walks visible_range and re-queues
    // cache misses. The dropped HBITMAPs are not leaked — D17 still fires
    // every cancelled callback with HBITMAP=nullptr so pending_tasks
    // drains, and any HBITMAP already produced for an in-flight render
    // arrives via WM_USER_THUMB_READY (HWND remains valid while hidden)
    // and lands in the cache as usual.
    impl_->cancel_in_flight();
}

bool ThumbnailPane::visible() const {
    return impl_->list_hwnd && IsWindowVisible(impl_->list_hwnd);
}

void ThumbnailPane::on_dpi_changed(unsigned new_dpi) {
    if (new_dpi == 0) new_dpi = 96;
    impl_->cached_dpi = new_dpi;
    impl_->model.set_dpi(new_dpi);

    // DPI change implies tile size change → every cached thumb is now the
    // wrong pixel size. Clear the cache and cancel in-flight renders;
    // pending_renders_ also clears (mutation behaves like #3 / set_page_count).
    if (impl_->cache) impl_->cache->clear();
    impl_->cancel_in_flight();

    // M2: palette colors are DPI-invariant in our scheme, but rebuild
    // brushes for symmetry with future WM_SETTINGCHANGE handlers — and
    // because if the dark-mode probe ever runs here we'd otherwise leak
    // the old brushes when palette swaps.
    impl_->rebuild_brushes();

    impl_->refresh_item_height();
    impl_->sync_column_width();
    if (impl_->list_hwnd) {
        InvalidateRect(impl_->list_hwnd, nullptr, TRUE);
    }
}

// ---------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------

namespace {

struct PaintArgs {
    UINT                  dpi;
    int                   tile_w_px;
    int                   tile_h_px;
    int                   current_page;
    int                   page_count;
    const Palette&        palette;
    const PaletteBrushes& brushes;  // M2: cached, owned by Impl.
};

// Compute the centered tile rectangle within a row's rcItem. Shared by
// the placeholder painter and T6's cache-hit blit path so they always
// agree on tile geometry.
RECT compute_tile_rect(const RECT& rc, const PaintArgs& args) {
    const int margin = dp(kTileMarginDip, args.dpi);
    const int row_w  = std::max(0, static_cast<int>(rc.right - rc.left));
    RECT tile{};
    tile.left   = rc.left + std::max(0, (row_w - args.tile_w_px) / 2);
    tile.top    = rc.top + margin;
    tile.right  = tile.left + args.tile_w_px;
    tile.bottom = tile.top  + args.tile_h_px;
    return tile;
}

// Draw the current-page accent frame. Used by both placeholder and
// cache-hit paths so the highlight tracks the active page regardless of
// whether the thumb is rendered yet.
void paint_current_highlight(HDC hdc, const RECT& tile, int page,
                              const PaintArgs& args) {
    if (page != args.current_page || args.page_count <= 0) return;
    const int bw = std::max(1, dp(kBorderWidthDip, args.dpi));
    HBRUSH hb = args.brushes.current_border;
    RECT hrc = tile;
    InflateRect(&hrc, bw, bw);
    // Top, bottom, left, right strips drawn via FillRect for crisp pixel
    // alignment (FrameRect's default thickness is 1).
    RECT t = { hrc.left, hrc.top,            hrc.right,      hrc.top + bw  };
    RECT b = { hrc.left, hrc.bottom - bw,    hrc.right,      hrc.bottom    };
    RECT l = { hrc.left, hrc.top,            hrc.left + bw,  hrc.bottom    };
    RECT r = { hrc.right - bw, hrc.top,      hrc.right,      hrc.bottom    };
    FillRect(hdc, &t, hb);
    FillRect(hdc, &b, hb);
    FillRect(hdc, &l, hb);
    FillRect(hdc, &r, hb);
}

// Draw the "Page N" label below the tile. Shared by both paint paths.
void paint_label(HDC hdc, const RECT& rc, const RECT& tile, int page,
                 const PaintArgs& args) {
    const int margin  = dp(kTileMarginDip, args.dpi);
    const int label_h = dp(kLabelHeightDip, args.dpi);

    wchar_t buf[32];
    std::swprintf(buf, sizeof(buf) / sizeof(buf[0]), L"%d", page + 1);

    RECT label{};
    label.left   = rc.left;
    label.right  = rc.right;
    label.top    = tile.bottom + margin;
    label.bottom = label.top + label_h;

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, args.palette.text);
    DrawTextW(hdc, buf, -1, &label,
              DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
}

void paint_placeholder(const DRAWITEMSTRUCT* dis, const PaintArgs& args) {
    HDC hdc = dis->hDC;
    RECT rc = dis->rcItem;
    const int page = static_cast<int>(dis->itemID);

    // Pane background (full row). M2: cached brushes throughout this fn.
    FillRect(hdc, &rc, args.brushes.pane_bg);

    const RECT tile = compute_tile_rect(rc, args);

    // Fill placeholder.
    FillRect(hdc, &tile, args.brushes.tile_fill);

    // 1-px placeholder outline. FrameRect uses the brush color.
    RECT outline = tile;
    InflateRect(&outline, dp(kPlaceholderInsetDip, args.dpi),
                          dp(kPlaceholderInsetDip, args.dpi));
    FrameRect(hdc, &outline, args.brushes.tile_border);

    paint_current_highlight(hdc, tile, page, args);
    paint_label(hdc, rc, tile, page, args);
}

// T6: blit a real thumb HBITMAP into the row. The cache lookup happened
// in WM_DRAWITEM; here we just composite and add the standard chrome
// (background fill outside the tile, current-page highlight, label).
void paint_thumb(const DRAWITEMSTRUCT* dis, HBITMAP bm,
                 const PaintArgs& args) {
    HDC hdc = dis->hDC;
    RECT rc = dis->rcItem;
    const int page = static_cast<int>(dis->itemID);

    // Background first so any sliver outside the tile rect (e.g. when the
    // bitmap is narrower than expected) gets the pane color, not GDI's
    // default white.
    FillRect(hdc, &rc, args.brushes.pane_bg);

    const RECT tile = compute_tile_rect(rc, args);

    // BitBlt the cached HBITMAP into the tile rect. Use a memory DC to
    // select the bitmap; SRCCOPY is correct because pixmap_to_hbitmap
    // produced a 32-bit BGRA top-down DIB section with alpha=1 (opaque).
    HDC mem_dc = CreateCompatibleDC(hdc);
    if (mem_dc) {
        HGDIOBJ prev = SelectObject(mem_dc, bm);
        BITMAP info{};
        if (GetObject(bm, sizeof(info), &info)) {
            const int src_w = info.bmWidth;
            const int src_h = std::abs(info.bmHeight);  // top-down: negative
            const int dst_w = tile.right  - tile.left;
            const int dst_h = tile.bottom - tile.top;
            // If the cached thumb was rendered at a slightly different
            // scale (e.g. cached at 96 dpi but pane is now at 144 dpi for
            // a fraction of a frame mid-DPI-swap), StretchBlt avoids a
            // crashy out-of-bounds blit. Set HALFTONE for crisper
            // downscaling; SetBrushOrgEx is the documented prerequisite.
            if (src_w == dst_w && src_h == dst_h) {
                BitBlt(hdc, tile.left, tile.top, dst_w, dst_h,
                       mem_dc, 0, 0, SRCCOPY);
            } else if (src_w > 0 && src_h > 0) {
                const int prev_mode = SetStretchBltMode(hdc, HALFTONE);
                SetBrushOrgEx(hdc, 0, 0, nullptr);
                StretchBlt(hdc, tile.left, tile.top, dst_w, dst_h,
                           mem_dc, 0, 0, src_w, src_h, SRCCOPY);
                SetStretchBltMode(hdc, prev_mode);
            }
        }
        SelectObject(mem_dc, prev);
        DeleteDC(mem_dc);
    }

    // Outline the rendered thumb so it sits cleanly inside the row,
    // matching the placeholder visual rhythm.
    RECT outline = tile;
    InflateRect(&outline, dp(kPlaceholderInsetDip, args.dpi),
                          dp(kPlaceholderInsetDip, args.dpi));
    FrameRect(hdc, &outline, args.brushes.tile_border);

    paint_current_highlight(hdc, tile, page, args);
    paint_label(hdc, rc, tile, page, args);
}

}  // namespace

// ---------------------------------------------------------------------------
// Subclass procs
// ---------------------------------------------------------------------------

LRESULT CALLBACK thumb_list_subclass_proc(HWND hwnd, UINT msg, WPARAM w,
                                          LPARAM l, UINT_PTR /*id*/,
                                          DWORD_PTR ref_data) {
    auto* self = reinterpret_cast<ThumbnailPane*>(ref_data);
    auto& impl = *self->impl_;

    switch (msg) {
        case WM_USER_THUMB_READY: {
            // T6 worker->UI bridge. Runs on UI thread (PostMessageW guarantee).
            // pending_renders_ mutation point #2: erase the page regardless
            // of HBITMAP success/null. Either the render succeeded (insert
            // into cache) or it failed/was cancelled (D17 fires on_complete
            // with nullptr) — in both cases the request is no longer pending,
            // so a future WM_DRAWITEM may re-queue it if needed.
            const int     page = static_cast<int>(w);
            const HBITMAP bm   = reinterpret_cast<HBITMAP>(l);
            impl.pending_renders_.erase(page);
            if (bm) {
                if (impl.cache) {
                    // Cache takes ownership (ThumbCache::put contract).
                    impl.cache->put(page, bm);
                } else {
                    // Defensive: if the cache was unset between submit and
                    // completion (e.g. set_cache(nullptr) during tab swap),
                    // we must DeleteObject ourselves or the bitmap leaks.
                    DeleteObject(bm);
                }
                // Repaint just this row — no need to invalidate the whole
                // pane. ListView_RedrawItems queues a paint on the UI thread
                // which will re-enter WM_DRAWITEM, hit the cache, and blit.
                if (page >= 0 && page < impl.model.page_count()) {
                    ListView_RedrawItems(hwnd, page, page);
                }
            }
            // Null bm path: cancelled / failed render. No repaint needed —
            // the placeholder is already on screen and we'll re-queue when
            // the row scrolls back into view.
            return 0;
        }
        case WM_SIZE: {
            const int new_h = HIWORD(l);
            impl.model.set_viewport_h_px(new_h);

            // Update scrollbar range to reflect the new viewport / total
            // height. Without this the scrollbar would either over- or
            // under-shoot when the user drags the thumb.
            SCROLLINFO si{};
            si.cbSize = sizeof(si);
            si.fMask  = SIF_RANGE | SIF_PAGE | SIF_POS;
            si.nMin   = 0;
            si.nMax   = std::max(0, impl.model.total_height_px() - 1);
            si.nPage  = static_cast<UINT>(std::max(0, new_h));
            si.nPos   = impl.model.scroll_y_px();
            SetScrollInfo(hwnd, SB_VERT, &si, TRUE);

            impl.sync_column_width();
            break;  // let DefSubclassProc do the listview-internal layout
        }
        case WM_VSCROLL: {
            // Compute the new requested scroll position and forward it to
            // the model, then ask the model what we actually applied (it
            // clamps to [0, total - viewport]).
            SCROLLINFO si{};
            si.cbSize = sizeof(si);
            si.fMask  = SIF_ALL;
            GetScrollInfo(hwnd, SB_VERT, &si);
            const int old_pos = si.nPos;
            int new_pos = old_pos;

            // Use one tile pitch as a "line" step; one viewport as a "page".
            const int line = std::max(1, impl.model.tile_h_px());

            switch (LOWORD(w)) {
                case SB_TOP:        new_pos = si.nMin; break;
                case SB_BOTTOM:     new_pos = si.nMax; break;
                case SB_LINEUP:     new_pos = old_pos - line; break;
                case SB_LINEDOWN:   new_pos = old_pos + line; break;
                case SB_PAGEUP:     new_pos = old_pos -
                                         static_cast<int>(si.nPage); break;
                case SB_PAGEDOWN:   new_pos = old_pos +
                                         static_cast<int>(si.nPage); break;
                case SB_THUMBPOSITION:
                case SB_THUMBTRACK: new_pos = static_cast<int>(si.nTrackPos);
                                    break;
                default: break;
            }
            impl.model.set_scroll_y_px(new_pos);
            impl.sync_scroll_pos();
            InvalidateRect(hwnd, nullptr, FALSE);
            // M4 carry-over: fall through to DefSubclassProc instead of
            // `return 0`. The listview's internal iTopIndex must move so
            // that the WM_DRAWITEM messages it queues for the next paint
            // pass come in for the rows we want; without this, the model's
            // scroll_y_ moved but the listview kept its old iTopIndex, so
            // `dis->rcItem` arrived at stale coordinates and tiles did not
            // visually move. Letting Def... run keeps the two scroll
            // sources of truth in sync.
            break;
        }
        case WM_MOUSEWHEEL: {
            const int delta = GET_WHEEL_DELTA_WPARAM(w);
            // One notch = three "lines" (matches Win32 default for non-zero
            // SPI_GETWHEELSCROLLLINES — we don't read the SPI in T5, the
            // 3-tile heuristic is fine for placeholder behavior).
            const int line = std::max(1, impl.model.tile_h_px());
            const int dy   = -(delta / WHEEL_DELTA) * 3 * line;
            impl.model.set_scroll_y_px(impl.model.scroll_y_px() + dy);
            impl.sync_scroll_pos();
            InvalidateRect(hwnd, nullptr, FALSE);
            // M4: fall through (see WM_VSCROLL note) so the listview's
            // iTopIndex tracks the model.
            break;
        }
        case WM_LBUTTONDOWN: {
            const int y = GET_Y_LPARAM(l);
            auto page = impl.model.page_at_y(y);
            if (page && impl.on_navigate) {
                impl.on_navigate(*page);
            }
            // Fall through so the listview still updates selection state.
            break;
        }
    }
    return DefSubclassProc(hwnd, msg, w, l);
}

LRESULT CALLBACK thumb_parent_subclass_proc(HWND hwnd, UINT msg, WPARAM w,
                                            LPARAM l, UINT_PTR /*id*/,
                                            DWORD_PTR ref_data) {
    auto* self = reinterpret_cast<ThumbnailPane*>(ref_data);
    auto& impl = *self->impl_;

    if (msg == WM_DRAWITEM) {
        auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(l);
        if (dis && dis->hwndItem == impl.list_hwnd) {
            const int page = static_cast<int>(dis->itemID);
            if (page >= 0 && page < impl.model.page_count()) {
                PaintArgs args{
                    impl.cached_dpi,
                    impl.model.tile_w_px(),
                    impl.model.tile_h_px(),
                    impl.model.current_page(),
                    impl.model.page_count(),
                    impl.palette,
                    impl.brushes_,
                };

                // T6: cache hit → blit; miss → placeholder + submit.
                HBITMAP bm = impl.cache ? impl.cache->get(page) : nullptr;
                if (bm) {
                    paint_thumb(dis, bm, args);
                } else {
                    paint_placeholder(dis, args);

                    // pending_renders_ mutation point #1 (cache miss).
                    // Insert BEFORE submit so a fast worker that fires the
                    // callback synchronously cannot PostMessage WM_USER_THUMB_READY
                    // before we've recorded the page as in-flight (which would
                    // let the next WM_DRAWITEM iteration re-submit a duplicate).
                    if (impl.renderer && impl.cache &&
                        impl.pending_renders_.insert(page).second) {
                        // Capture the list HWND, NOT `self` or `&impl`. The
                        // worker thread fires the callback after the renderer's
                        // own dtor (D16) has guaranteed liveness — but the pane
                        // could have already null'd its renderer pointer or
                        // even moved cache references in between. Targeting
                        // the HWND is safe across pane recovery: Win32
                        // PostMessageW gracefully fails (returns FALSE) if the
                        // HWND is destroyed, in which case we DeleteObject the
                        // bitmap immediately to avoid a leak.
                        HWND target = impl.list_hwnd;
                        impl.renderer->submit(page,
                            [target, page](HBITMAP rendered) {
                                // Worker thread. Do NOT touch any pane state
                                // here — only the message queue.
                                if (!PostMessageW(target,
                                                  WM_USER_THUMB_READY,
                                                  static_cast<WPARAM>(page),
                                                  reinterpret_cast<LPARAM>(rendered))) {
                                    // HWND gone (pane destroyed mid-render or
                                    // posted-message queue full). Drop the
                                    // bitmap so it does not leak. nullptr is
                                    // fine — D17 cancellations send nullptr
                                    // here too and DeleteObject(nullptr) is a
                                    // no-op per MSDN.
                                    if (rendered) DeleteObject(rendered);
                                }
                            });
                    }
                }
            }
            return TRUE;
        }
        // Not ours - let MainWindow's WM_DRAWITEM (tab strip) run via the
        // subclass chain. DefSubclassProc forwards to the next subclass /
        // window proc.
    }
    if (msg == WM_MEASUREITEM) {
        auto* mis = reinterpret_cast<MEASUREITEMSTRUCT*>(l);
        // ODT_LISTVIEW (decimal 102) is sent for owner-draw listview rows.
        // CtlID is the dialog control ID — we created the listview with a
        // null hMenu so CtlID will be 0; instead we route based on the
        // CtlType + parent ownership. Since this subclass is unique per
        // ThumbnailPane on this parent HWND, any owner-draw listview
        // measure that arrives here is ours.
        if (mis && mis->CtlType == ODT_LISTVIEW) {
            mis->itemHeight = static_cast<UINT>(impl.row_height());
            return TRUE;
        }
    }
    return DefSubclassProc(hwnd, msg, w, l);
}

}  // namespace litepdf::ui
