#include "ui/ThumbnailPane.hpp"

#include "core/ThumbCache.hpp"
#include "core/ThumbnailModel.hpp"
#include "core/ThumbnailRenderer.hpp"

#include <commctrl.h>
#include <dwmapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cwchar>
#include <stdexcept>
#include <string>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "dwmapi.lib")

namespace litepdf::ui {

namespace {

// Distinct IDs so the ListView subclass + the parent-WM_DRAWITEM subclass
// can coexist with TabManager's parent-side WM_DRAWITEM handler. Using
// hand-picked magic numbers (mirrors TabManager's kTabSubclassId style).
constexpr UINT_PTR kListSubclassId   = 0xAB02;
constexpr UINT_PTR kParentSubclassId = 0xAB03;

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

    // Populated by T6's set_cache / set_renderer. Held as nullable raw
    // pointers because the cache + renderer are owned by DocumentView and
    // outlive the pane (D6/D7). on_dpi_changed nullptr-guards both so it
    // is correct to call before T6 has wired them.
    litepdf::core::ThumbCache*        cache    = nullptr;
    litepdf::core::ThumbnailRenderer* renderer = nullptr;

    bool    dark_mode = false;
    Palette palette   = make_palette(false);
    UINT    cached_dpi = 96;

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
    SetWindowSubclass(parent, thumb_parent_subclass_proc,
                      kParentSubclassId,
                      reinterpret_cast<DWORD_PTR>(this));
}

ThumbnailPane::~ThumbnailPane() {
    if (impl_) {
        if (impl_->parent) {
            RemoveWindowSubclass(impl_->parent, thumb_parent_subclass_proc,
                                 kParentSubclassId);
        }
        if (impl_->list_hwnd) {
            RemoveWindowSubclass(impl_->list_hwnd, thumb_list_subclass_proc,
                                 kListSubclassId);
            DestroyWindow(impl_->list_hwnd);
            impl_->list_hwnd = nullptr;
        }
    }
}

HWND ThumbnailPane::hwnd() const { return impl_ ? impl_->list_hwnd : nullptr; }

void ThumbnailPane::set_on_navigate(NavigateCb cb) {
    impl_->on_navigate = std::move(cb);
}

void ThumbnailPane::set_page_count(int n) {
    if (!impl_->list_hwnd) return;
    n = std::max(0, n);
    impl_->model.set_page_count(n);
    impl_->model.set_scroll_y_px(0);

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
    if (change.first < 0 && change.second < 0) return;  // no-op

    // Invalidate both the previous and new tile rects so the highlight
    // border repaints. tile_rect returns coords relative to the listview
    // client, but using ListView_RedrawItems is cleaner because it accounts
    // for the row layout the control owns.
    if (change.first  >= 0) ListView_RedrawItems(impl_->list_hwnd,
                                                  change.first,  change.first);
    if (change.second >= 0) ListView_RedrawItems(impl_->list_hwnd,
                                                  change.second, change.second);
    UpdateWindow(impl_->list_hwnd);
}

void ThumbnailPane::show() {
    if (impl_->list_hwnd) ShowWindow(impl_->list_hwnd, SW_SHOW);
}

void ThumbnailPane::hide() {
    if (impl_->list_hwnd) ShowWindow(impl_->list_hwnd, SW_HIDE);
}

bool ThumbnailPane::visible() const {
    return impl_->list_hwnd && IsWindowVisible(impl_->list_hwnd);
}

void ThumbnailPane::on_dpi_changed(unsigned new_dpi) {
    if (new_dpi == 0) new_dpi = 96;
    impl_->cached_dpi = new_dpi;
    impl_->model.set_dpi(new_dpi);

    // T6 wires these in via setters. They are nullptr in T5 so the cache
    // clear / renderer cancel are no-ops; once T6 lands, the same call
    // does the right thing without any further wiring change.
    if (impl_->cache)    impl_->cache->clear();
    if (impl_->renderer) impl_->renderer->cancel_pending();

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
    UINT             dpi;
    int              tile_w_px;
    int              tile_h_px;
    int              current_page;
    int              page_count;
    const Palette&   palette;
};

void paint_placeholder(const DRAWITEMSTRUCT* dis, const PaintArgs& args) {
    HDC hdc = dis->hDC;
    RECT rc = dis->rcItem;
    const int page = static_cast<int>(dis->itemID);
    const UINT dpi = args.dpi;
    const Palette& pal = args.palette;

    // Pane background (full row).
    HBRUSH bg = CreateSolidBrush(pal.pane_bg);
    FillRect(hdc, &rc, bg);
    DeleteObject(bg);

    // Tile box: centered horizontally, top-aligned with kTileMarginDip
    // padding above and the label-strip below.
    const int margin = dp(kTileMarginDip, dpi);
    const int label_h = dp(kLabelHeightDip, dpi);
    const int tile_w = args.tile_w_px;
    const int tile_h = args.tile_h_px;
    const int row_w  = std::max(0, static_cast<int>(rc.right - rc.left));

    RECT tile{};
    tile.left   = rc.left + std::max(0, (row_w - tile_w) / 2);
    tile.top    = rc.top + margin;
    tile.right  = tile.left + tile_w;
    tile.bottom = tile.top  + tile_h;

    // Fill placeholder.
    HBRUSH fill = CreateSolidBrush(pal.tile_fill);
    FillRect(hdc, &tile, fill);
    DeleteObject(fill);

    // 1-px placeholder outline. FrameRect uses the brush color.
    HBRUSH border = CreateSolidBrush(pal.tile_border);
    RECT outline = tile;
    InflateRect(&outline, dp(kPlaceholderInsetDip, dpi),
                          dp(kPlaceholderInsetDip, dpi));
    FrameRect(hdc, &outline, border);
    DeleteObject(border);

    // Current-page highlight: thicker accent-colored frame around the tile.
    if (page == args.current_page && args.page_count > 0) {
        const int bw = std::max(1, dp(kBorderWidthDip, dpi));
        HBRUSH hb = CreateSolidBrush(pal.current_border);
        RECT hrc = tile;
        InflateRect(&hrc, bw, bw);
        // Top, bottom, left, right strips drawn via FillRect for crisp
        // pixel alignment (FrameRect's default thickness is 1).
        RECT t = { hrc.left, hrc.top, hrc.right, hrc.top + bw };
        RECT b = { hrc.left, hrc.bottom - bw, hrc.right, hrc.bottom };
        RECT l = { hrc.left, hrc.top, hrc.left + bw, hrc.bottom };
        RECT r = { hrc.right - bw, hrc.top, hrc.right, hrc.bottom };
        FillRect(hdc, &t, hb);
        FillRect(hdc, &b, hb);
        FillRect(hdc, &l, hb);
        FillRect(hdc, &r, hb);
        DeleteObject(hb);
    }

    // Page-number label. Display 1-indexed for the user; itemID is 0-based.
    wchar_t buf[32];
    std::swprintf(buf, sizeof(buf) / sizeof(buf[0]), L"%d", page + 1);

    RECT label{};
    label.left   = rc.left;
    label.right  = rc.right;
    label.top    = tile.bottom + margin;
    label.bottom = label.top + label_h;

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, pal.text);
    DrawTextW(hdc, buf, -1, &label,
              DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
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
            return 0;  // we handled it; suppress DefSubclassProc's own scroll
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
            return 0;
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
                };
                paint_placeholder(dis, args);
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
