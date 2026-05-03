// LitePDF -- ui::MainWindow: top-level window, menu, message pump.
#include "ui/MainWindow.hpp"
#include "MainMenu.rc.h"

#include "app/SearchDispatcher.hpp"
#include "app/SingleInstance.hpp"
#include "core/Document.hpp"
#include "core/DocumentView.hpp"
#include "core/SearchSession.hpp"
#include "core/TabList.hpp"
#include "ui/ColdStartTimer.hpp"
#include "ui/PasswordDialog.hpp"  // Phase 8 Task 1
#include "ui/password_retry.hpp"  // Phase 8 Task 1
#include "ui/ThumbnailPane.hpp"  // Phase 7 Task 8: F4 toggle uses ThumbnailPane methods.

#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <stdlib.h>
#include <algorithm>
#include <climits>
#include <cwctype>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")

namespace {
constexpr wchar_t kWindowClassName[] = L"LitePDFMainWindow";
constexpr wchar_t kWindowTitle[]     = L"LitePDF";

constexpr UINT WM_USER_OPEN_OK       = WM_USER + 1;
constexpr UINT WM_USER_OPEN_FAILED   = WM_USER + 2;
// WM_USER + 3 is WM_USER_RENDER_DONE, reserved by PdfCanvas.
// Phase 8 Task 1: encrypted-PDF password handshake. Worker thread can't
// run a modal dialog (no message pump, no parent HWND), so on
// OpenError::NeedsPassword the worker posts this message carrying the
// still-alive Document + path; the UI thread runs the 3-attempt loop,
// calls authenticate() on the same Document, and either constructs a
// DocumentView/Tab inline or drops the Document on cancel/exhaustion.
constexpr UINT WM_USER_PASSWORD_PROMPT = WM_USER + 12;
// Phase 6 Task 10: per-tab SearchSession on_update() observer fires on a
// worker thread and PostMessage-marshals to this message on the UI thread.
// Posted once per completed page scan (plus a final post when
// scan_complete flips). Cheap — handler just reads hit_count/scan_complete
// and refreshes the counter + invalidates the canvas.
constexpr UINT WM_USER_SEARCH_UPDATE = WM_USER + 10;
// Phase 6 Tasks 11-14: CrossTabSearch aggregation observer fires on a
// worker thread as each per-tab SearchSession's page scan completes.
// PostMessage-marshals to this message on the UI thread so the docked
// ResultsPanel can ListView_SetItemCountEx without cross-thread UI calls.
constexpr UINT WM_USER_CROSS_TAB_UPDATE = WM_USER + 11;

// Phase 8 Task 1: bundle handed across the worker→UI thread boundary
// when Document::open returns NeedsPassword. The worker posts a heap
// pointer; the UI handler adopts ownership via unique_ptr.
struct PendingPasswordOpen {
    litepdf::core::Document doc;        // already opened, awaiting authenticate()
    std::filesystem::path   path;        // for tab label + status feedback
};

// "&1 foo.pdf", "&2 foo.pdf", ..., "&9 foo.pdf", "1&0 foo.pdf".
// The '&' marks the mnemonic character (Alt-shortcut).
std::wstring format_mru_label(std::size_t i, const std::wstring& full_path) {
    std::wstring fname = std::filesystem::path(full_path).filename().wstring();
    if (fname.empty()) fname = full_path;  // defensive: path ends in separator
    std::wstring prefix = (i < 9)
        ? (L"&" + std::to_wstring(i + 1) + L" ")
        : std::wstring(L"1&0 ");
    return prefix + fname;
}
}  // namespace

namespace litepdf::ui {

MainWindow::MainWindow()
    // Phase 6: one dispatcher, shared by every tab's SearchSession.
    // 2 workers is the Phase 6 design default (§4 D5); re-tune once we
    // have perf data on large corpora.
    : search_dispatcher_(
          std::make_unique<litepdf::app::ThreadPoolDispatcher>(2)) {
    mru_.load();
}
MainWindow::~MainWindow() {
    if (haccel_) { DestroyAcceleratorTable(haccel_); haccel_ = nullptr; }
    // search_dispatcher_ is destroyed AFTER tabs_ (reverse declaration
    // order in MainWindow.hpp) — its dtor drains queued tasks so any
    // in-flight search finishes before the pool disappears.
}

std::filesystem::path MainWindow::canonicalize_for_mru(
    const std::filesystem::path& p)
{
    std::error_code ec;
    auto canon = std::filesystem::weakly_canonical(p, ec);
    return ec ? p : canon;
}

LRESULT CALLBACK MainWindow::WndProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    MainWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(l);
        self = reinterpret_cast<MainWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    } else {
        self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self) {
        return self->handle_message(hwnd, msg, w, l);
    }
    return DefWindowProcW(hwnd, msg, w, l);
}

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
    std::wstring title = L"LitePDF \u2014 ";
    title += t->label;
    SetWindowTextW(hwnd_, title.c_str());
}

void MainWindow::open_tab_async(std::filesystem::path path) {
    HWND hwnd = hwnd_;
    // Capture dispatcher by raw ref — it lives for the entire MainWindow
    // lifetime (declared before tabs_ in MainWindow.hpp) and the UI
    // thread waits on window messages, so the dispatcher is guaranteed
    // to be alive when we either construct a DocumentView here or post
    // WM_USER_OPEN_OK. If hwnd is destroyed before PostMessageW runs,
    // the ownership-recovery path (delete raw) still works because
    // DocumentView's dtor tears down its SearchSession before anything
    // else touches the dispatcher.
    auto& dispatcher = *search_dispatcher_;
    std::thread([hwnd, &dispatcher, path = std::move(path)]() {
        litepdf::core::Document doc;
        auto err = doc.open(path);
        if (err.has_value()) {
            // Phase 8 Task 1: NeedsPassword is now a non-fatal handshake.
            // Hand the still-alive Document + path to the UI thread; it
            // runs the modal + retry loop and either re-enters tab
            // construction (success) or drops the Document (cancel /
            // exhaustion).
            if (*err == litepdf::core::Document::OpenError::NeedsPassword) {
                auto* bundle = new PendingPasswordOpen{
                    std::move(doc), std::move(path)};
                // Mirrors the WM_USER_OPEN_OK handoff pattern below: if
                // PostMessageW returns FALSE the bundle is freed here.
                // If it returns TRUE but the HWND is destroyed before
                // dispatch, the OS drops the queued message and the
                // bundle leaks. The leak is bounded (at most one bundle
                // in flight per shutdown) and the OS reclaims process
                // memory; Phase 12 crash-protection may track in-flight
                // bundles in MainWindow if the leak window matters.
                if (!PostMessageW(hwnd, WM_USER_PASSWORD_PROMPT,
                                  reinterpret_cast<WPARAM>(bundle), 0)) {
                    delete bundle;
                }
                return;
            }
            // Heap-allocate the path string so the UI handler can include
            // it in the MessageBox. Handler adopts ownership and deletes.
            // Without this, the error dialog says "File not found" but
            // gives no clue WHICH path failed — a real UX gap when a
            // relative CLI arg doesn't resolve against the user's cwd.
            auto* path_copy = new std::wstring(path.wstring());
            if (!PostMessageW(hwnd, WM_USER_OPEN_FAILED,
                              static_cast<WPARAM>(*err),
                              reinterpret_cast<LPARAM>(path_copy))) {
                delete path_copy;
            }
            return;
        }
        try {
            auto tab = std::make_unique<litepdf::core::Tab>();
            tab->path  = path;
            tab->label = path.filename().wstring();
            tab->view  = std::make_unique<litepdf::core::DocumentView>(
                std::move(doc), dispatcher);
            // Transfer ownership across thread boundary via raw ptr.
            auto* raw = tab.release();
            // Ownership has crossed the thread boundary. If PostMessageW
            // fails (target HWND destroyed between the hwnd capture and
            // now — e.g., user closed the window while we were opening),
            // there is no consumer to adopt the Tab; delete it here to
            // avoid a leak. Cannot use RAII (unique_ptr scope guard)
            // because release() has already happened — the ownership is
            // external to this scope until the message is received.
            if (!PostMessageW(hwnd, WM_USER_OPEN_OK,
                              reinterpret_cast<WPARAM>(raw), 0)) {
                delete raw;
            }
        } catch (...) {
            // DocumentView ctor can throw (e.g., ui-ctx clone or worker
            // thread creation failed). Map to the closest existing error.
            PostMessageW(hwnd, WM_USER_OPEN_FAILED,
                         static_cast<WPARAM>(
                             litepdf::core::Document::OpenError::Other), 0);
        }
    }).detach();
}

void MainWindow::kick_render(int page) {
    auto* view = active_view();
    if (!view || !canvas_) return;

    RECT rc;
    GetClientRect(canvas_->hwnd(), &rc);
    UINT dpi = GetDpiForWindow(hwnd_);
    view->set_zoom_mode(view->zoom_mode(),
        static_cast<float>(rc.right - rc.left),
        static_cast<float>(rc.bottom - rc.top),
        static_cast<float>(dpi));

    HWND target = canvas_->hwnd();
    view->request_render_with_prefetch(page,
        [target](fz_pixmap* p, fz_context* worker_ctx) {
            PdfCanvas::post_render_done(target, p, worker_ctx);
        });
}

HWND MainWindow::left_pane_hwnd() const {
    // Phase 7 Task 8: return whichever left-dock pane is visible.
    // Mutual exclusion (F4/F5 handlers) guarantees at most one is
    // visible at a time, so order of these checks doesn't matter.
    if (outline_ && outline_->visible()) return outline_->hwnd();
    auto* v = const_cast<MainWindow*>(this)->active_view();
    if (v) {
        if (auto* tp = v->thumb_pane(); tp && tp->visible()) {
            return tp->hwnd();
        }
    }
    return nullptr;
}

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

    // Phase 6 Tasks 12-13: reserve bottom space for splitter + results
    // panel when visible. Splitter is 4 DIP tall, docked immediately
    // above the panel. When the panel is hidden (panel_h == 0), both
    // vanish and the canvas spans the full remaining vertical strip.
    const int panel_h = (results_panel_ && results_panel_->visible())
                         ? results_panel_height_px_ : 0;
    const int splitter_h = (panel_h > 0)
                           ? MulDiv(4, static_cast<int>(dpi), 96) : 0;
    const int canvas_bottom = h - panel_h - splitter_h;
    const int row_h = std::max(0, canvas_bottom - row_y);

    // Phase 7 Task 8: left dock — at most one of outline / thumb pane
    // is visible at a time (F4/F5 mutual exclusion). When either is
    // visible, the VerticalSplitter (4 DIP wide) sits immediately to
    // its right and the canvas takes the remaining width.
    HWND left_hwnd = left_pane_hwnd();
    const int v_splitter_w = (left_hwnd != nullptr)
                              ? MulDiv(4, static_cast<int>(dpi), 96) : 0;
    // Clamp left_pane_width_px_ to the live client width — handles
    // window-shrink races where the saved width would push the canvas
    // off-screen. Same min/max as the v_splitter_->set_on_drag clamp.
    int left_w = 0;
    if (left_hwnd != nullptr) {
        const int min_w = MulDiv(120, static_cast<int>(dpi), 96);
        const int max_w = std::max(min_w + 1, w - 100);
        left_w = std::clamp(left_pane_width_px_, min_w, max_w);
    }

    if (left_hwnd != nullptr) {
        SetWindowPos(left_hwnd, nullptr,
                     0, row_y, left_w, row_h,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        if (v_splitter_ && v_splitter_->hwnd()) {
            RECT vr = { left_w, row_y,
                        left_w + v_splitter_w, row_y + row_h };
            v_splitter_->set_bounds(vr);
            ShowWindow(v_splitter_->hwnd(), SW_SHOW);
        }
        if (canvas_) {
            // Guard: if window is narrower than the left dock + splitter,
            // clamp canvas width to 0 rather than passing a negative
            // value to SetWindowPos (undefined behavior).
            const int canvas_w = std::max(0, w - left_w - v_splitter_w);
            SetWindowPos(canvas_->hwnd(), nullptr,
                         left_w + v_splitter_w, row_y, canvas_w, row_h,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
    } else {
        if (v_splitter_ && v_splitter_->hwnd()) {
            ShowWindow(v_splitter_->hwnd(), SW_HIDE);
        }
        if (canvas_) {
            SetWindowPos(canvas_->hwnd(), nullptr,
                         0, row_y, w, row_h,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }

    // Splitter + results panel bottom strip.
    if (splitter_) {
        if (panel_h > 0 && splitter_h > 0) {
            RECT sr = { 0, canvas_bottom, w, canvas_bottom + splitter_h };
            splitter_->set_bounds(sr);
            if (splitter_->hwnd()) {
                ShowWindow(splitter_->hwnd(), SW_SHOW);
            }
        } else if (splitter_->hwnd()) {
            ShowWindow(splitter_->hwnd(), SW_HIDE);
        }
    }
    if (results_panel_ && panel_h > 0) {
        RECT pr = { 0, canvas_bottom + splitter_h, w, h };
        results_panel_->set_bounds(pr);
    }

    // Phase 6 Task 10: anchor the find bar to the canvas's top-right.
    // Only when visible — reposition() calls SetWindowPos which would
    // otherwise force a move even though the window stays hidden.
    if (find_bar_ && find_bar_->visible() && canvas_) {
        RECT cr;
        GetWindowRect(canvas_->hwnd(), &cr);
        // Convert canvas screen rect to MainWindow client-space so
        // reposition() (which treats the rect as child coordinates)
        // anchors inside the canvas region.
        POINT tl = { cr.left,  cr.top    };
        POINT br = { cr.right, cr.bottom };
        ScreenToClient(hwnd_, &tl);
        ScreenToClient(hwnd_, &br);
        RECT cr_client = { tl.x, tl.y, br.x, br.y };
        find_bar_->reposition(cr_client);
    }
}

void MainWindow::toggle_outline() {
    if (!outline_) return;
    if (outline_->visible()) {
        outline_->hide();
    } else {
        // Phase 7 Task 8: F4/F5 mutual exclusion — hide the active tab's
        // thumb pane (if currently visible) before showing outline.
        if (auto* v = active_view()) {
            if (auto* tp = v->thumb_pane(); tp && tp->visible()) {
                tp->hide();
                if (auto* t = tabs_->active_tab()) t->thumb_visible = false;
            }
        }
        // Populate from the active tab's outline before showing. The on-
        // tab-switch restore path (case TCN_SELCHANGE) repopulates from
        // the incoming tab, but a fresh F5 on a tab that was never
        // switched away from would otherwise show an empty TreeView —
        // observed regression (handoff doc 2026-05-01) on bookmarks.pdf
        // first-launch + F5 with a single tab open. Re-populating on
        // every show is cheap (O(entries) and outlines are small) and
        // keeps the F5 and tab-switch paths aligned.
        if (auto* v = active_view()) {
            const auto& entries = v->document().outline();
            outline_->clear();
            if (!entries.empty()) outline_->populate(entries);
        }
        outline_->show();
    }
    on_layout();
    // Rendering follows the new canvas size so FitWidth stays correct.
    // on_tab_switch's outgoing-snapshot writes outline_visible back into
    // the active Tab when the user switches away -- that's the single
    // source of truth, so we don't write it here.
    if (auto* view = active_view()) kick_render(view->current_page());
}

void MainWindow::toggle_thumbs() {
    // Phase 7 Task 8: F4 handler. Implements D10 logic:
    //   - F4 with thumb visible → hide thumb (no need to touch outline).
    //   - F4 with thumb hidden  → if outline visible, hide outline; show thumb.
    //   - VerticalSplitter visibility tracks left_pane_hwnd() (handled in on_layout).
    auto* v = active_view();
    if (!v || !hwnd_) return;

    // Lazily create the thumb pane on first F4 for this tab. Construction
    // also seeds set_renderer / set_cache / set_page_count + current page,
    // so the very first F4 immediately starts rendering visible thumbs.
    HINSTANCE hinst = reinterpret_cast<HINSTANCE>(
        GetWindowLongPtrW(hwnd_, GWLP_HINSTANCE));
    auto* tp = v->ensure_thumb_pane(hinst, hwnd_);
    // Click-to-navigate. Re-set on every toggle (idempotent — ThumbnailPane
    // just overwrites the std::function). Route through
    // PdfCanvas::change_current_page (same as the outline pane) so the T7
    // page-change observer fires — keeps the thumb pane's own current-page
    // highlight in sync with the canvas, and matches the behavior of
    // every other navigation site (PgUp/PgDn, outline click, search jump).
    // Capture `this` because MainWindow outlives every DocumentView
    // (tabs_ is destroyed during ~MainWindow before the HWND tear-down
    // completes via OS WM_NCDESTROY cascade).
    tp->set_on_navigate([this](int page) {
        if (canvas_ && canvas_->change_current_page(page)) {
            kick_render(page);
        }
    });

    if (tp->visible()) {
        tp->hide();
        if (auto* t = tabs_ ? tabs_->active_tab() : nullptr) {
            t->thumb_visible = false;
        }
    } else {
        // Mutual exclusion: hide outline if it was the visible left pane.
        // Reset outline_visible too so on_tab_switch's restore path
        // doesn't bring outline back on a tab-switch round-trip.
        // Symmetric with toggle_outline()'s thumb_visible=false reset.
        if (outline_ && outline_->visible()) {
            outline_->hide();
            if (auto* t = tabs_ ? tabs_->active_tab() : nullptr) {
                t->outline_visible = false;
            }
        }
        tp->show();
        if (auto* t = tabs_ ? tabs_->active_tab() : nullptr) {
            t->thumb_visible = true;
        }
    }
    on_layout();
    // Canvas width may have changed — recompute fit-zoom + resubmit.
    kick_render(v->current_page());
}

void MainWindow::on_outline_navigate(int page) {
    auto* view = active_view();
    if (!view || !canvas_) return;
    // Route through PdfCanvas::change_current_page so the T7 page-change
    // observer fires (T8 wires this to the per-tab thumbnail pane).
    // Falls back to true == "page actually moved" — same semantics as
    // the prior direct view->set_current_page call.
    if (canvas_->change_current_page(page)) {
        kick_render(page);
    }
}

void MainWindow::on_tab_switch(int new_index, int old_index) {
    // Snapshot outgoing state. cancel_stale_renders(INT_MAX) drains
    // the outgoing view's priority queue so workers don't continue on
    // now-irrelevant P1/P2 prefetches. This is a performance
    // optimization, not a correctness measure: the cross-tab drop
    // safety of any in-flight pixmap is guaranteed by the per-render
    // escrow ctx (see PdfCanvas::post_render_done).
    if (old_index >= 0) {
        if (auto* outgoing = tabs_->tab_at(old_index)) {
            auto p = canvas_ ? canvas_->pan() : PdfCanvas::Pan{0.0f, 0.0f};
            outgoing->pan_x = p.x;
            outgoing->pan_y = p.y;
            outgoing->outline_visible = outline_ && outline_->visible();
            // Phase 7 Task 8 / D11: snapshot outgoing tab's thumb-pane
            // visibility into the Tab struct. Live pane->visible() is
            // the source of truth (the Tab struct flag may have lagged
            // if the pane was hidden via mutual exclusion in the F5
            // handler), then hide the pane unconditionally so the
            // incoming tab's pane lifecycle starts clean.
            if (outgoing->view) {
                if (auto* tp = outgoing->view->thumb_pane()) {
                    outgoing->thumb_visible = tp->visible();
                    if (tp->visible()) tp->hide();
                }
                outgoing->view->cancel_stale_renders(INT_MAX);
            }
        }
    }

    auto* incoming = tabs_->active_tab();
    if (canvas_) {
        canvas_->set_view(incoming ? incoming->view.get() : nullptr);
        if (incoming) canvas_->set_pan(incoming->pan_x, incoming->pan_y);
        else          canvas_->set_pan(0.0f, 0.0f);
        // (Phase 8 D9) Carry the incoming tab's Invert Colors polarity
        // onto the canvas chrome so a switch lands on a chrome that
        // matches the tab's page bitmap. Empty-tabs case clears it.
        canvas_->set_invert_chrome(incoming && incoming->view
                                       ? incoming->view->invert_colors()
                                       : false);
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

    // Phase 7 Task 8 / D11: restore incoming tab's thumb-pane visibility.
    // Mutual exclusion still holds — if incoming has BOTH thumb_visible
    // and outline_visible (shouldn't happen post-T8, but defensive),
    // outline wins because its show() above already ran.
    if (incoming && incoming->view && incoming->thumb_visible
        && (!outline_ || !outline_->visible()))
    {
        // Lazy-create the pane if this tab's first F4 was followed by a
        // tab switch before any second F4. ensure_thumb_pane is
        // idempotent — returns the existing pane on subsequent calls.
        HINSTANCE hinst = reinterpret_cast<HINSTANCE>(
            GetWindowLongPtrW(hwnd_, GWLP_HINSTANCE));
        auto* tp = incoming->view->ensure_thumb_pane(hinst, hwnd_);
        // Re-bind navigate callback — see toggle_thumbs for rationale.
        // Each tab has its own pane instance, so the callback storage
        // is per-tab; binding once on first show is enough but
        // re-binding on every show is cheap and trivially correct.
        tp->set_on_navigate([this](int page) {
            if (canvas_ && canvas_->change_current_page(page)) {
                kick_render(page);
            }
        });
        // The page count may have shifted between when ensure_thumb_pane
        // first ran and now (it shouldn't, since DocumentView's page
        // count is immutable for a fixed Document, but defense-in-depth
        // for future re-open paths). set_page_count also clears any
        // pending in-flight renders, which is fine: WM_DRAWITEM will
        // re-queue cache misses on the next paint.
        tp->set_page_count(incoming->view->page_count());
        tp->set_current_page(incoming->view->current_page());
        tp->show();
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

    // Phase 6 Task 10: refresh canvas hits-source + counter for the new
    // active view. The HitsFn callback itself just closes over `this`
    // and looks up active_view() each paint, so the binding is stable
    // across tab switches — but we still need to clear the
    // current-hit highlight carried over from the outgoing tab, and
    // refresh the find bar counter.
    update_canvas_hits_source();
    if (canvas_) canvas_->set_current_hit(std::nullopt);
    update_find_counter();

    (void)new_index;
}

void MainWindow::on_tab_close_request(int index) {
    if (!tabs_) return;
    // Defense-in-depth against C2: if a cross-tab scan is in flight
    // (results panel visible == dispatch active), drop its sentinel and
    // restore per-session observers BEFORE destroying the tab. The
    // chained observer lambda held by each SearchSession captures
    // `view` / `sess` by raw pointer; clearing now means any task that
    // finishes during ~SearchSession's drain (C1) will see an expired
    // weak_sentinel and return early without touching the dying view.
    if (cross_tab_ && results_panel_ && results_panel_->visible()) {
        cross_tab_->clear();
        // Push the now-empty hit count down to the virtual ListView so it
        // invalidates any stale row display. Without this the list keeps
        // SetItemCountEx(N) from the prior dispatch; subsequent paint or
        // focus events would issue LVN_GETDISPINFO for rows 0..N-1,
        // which still have proper bounds checks but leave the list
        // visually stuck on dead rows. Belt-and-braces hygiene.
        results_panel_->refresh_count();
    }
    tabs_->close_tab(index);
    // close_tab() fires on_switch (with new_active=-1 when the last tab
    // is dropped); on_tab_switch() performs all the canvas/outline/layout
    // teardown. No further work needed here.
}

LRESULT MainWindow::handle_message(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
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
            // Phase 6 Task 10: floating find bar, initially hidden. Wire
            // callbacks before Ctrl+F can ever fire.
            find_bar_ = std::make_unique<FindBar>(cs->hInstance, hwnd);
            find_bar_->set_on_query_changed(
                [this](std::wstring q, bool mc) {
                    on_find_query_changed(q, mc);
                });
            find_bar_->set_on_next ([this] { on_find_next();  });
            find_bar_->set_on_prev ([this] { on_find_prev();  });
            find_bar_->set_on_close([this] { on_find_close(); });
            // No active tab yet, so canvas_->set_hits_source wiring is a
            // no-op. We set it once on first tab open via
            // update_canvas_hits_source() (called from on_tab_switch).

            // Phase 6 Tasks 11-14: cross-tab search orchestrator + bottom
            // results panel + drag splitter. Created at WM_CREATE for
            // cleaner lifetime than lazy allocation on first Ctrl+Shift+F.
            // ResultsPanel + Splitter are hidden via results_panel_->hide()
            // and the layout branch in on_layout() (panel_h == 0 suppresses
            // splitter positioning); they become visible on first
            // IDM_CROSS_TAB_FIND / IDM_TOGGLE_RESULTS.
            cross_tab_     = std::make_unique<litepdf::app::CrossTabSearch>();
            results_panel_ = std::make_unique<ResultsPanel>(
                cs->hInstance, hwnd, *cross_tab_);
            splitter_      = std::make_unique<Splitter>(cs->hInstance, hwnd);
            // Hide both until first Ctrl+Shift+F. ResultsPanel is created
            // with WS_CHILD only (no WS_VISIBLE) so hide() is the no-op
            // path; Splitter is created with WS_VISIBLE so we explicitly
            // hide it here.
            results_panel_->hide();
            if (splitter_->hwnd()) {
                ShowWindow(splitter_->hwnd(), SW_HIDE);
            }
            results_panel_->set_on_query_submit(
                [this](std::wstring q) { on_results_query(q); });
            results_panel_->set_on_row_click(
                [this](std::size_t i) { on_results_row_click(i); });
            results_panel_->set_on_close(
                [this] { on_results_close(); });
            splitter_->set_on_drag([this](int new_h) {
                // Clamp the dragged height so the splitter can't swallow
                // the canvas entirely — leave at least ~100 px of canvas
                // visible and refuse a panel shorter than ~80 px (below
                // which the ListView has no room for even a single row).
                RECT client; GetClientRect(hwnd_, &client);
                const int max_h = std::max(80,
                    static_cast<int>(client.bottom) - 100);
                results_panel_height_px_ = std::clamp(new_h, 80, max_h);
                on_layout();
            });
            // Observer chains: CrossTabSearch's own aggregator runs on a
            // worker thread when any tab's SearchSession finishes a page
            // scan. Marshal to UI thread so ResultsPanel's
            // ListView_SetItemCountEx is called from the right thread.
            {
                HWND target = hwnd;
                cross_tab_->set_on_update([target] {
                    PostMessageW(target, WM_USER_CROSS_TAB_UPDATE, 0, 0);
                });
            }

            // Phase 7 Task 8: vertical splitter for the left dock
            // (outline + thumb pane). Singleton — one MainWindow owns
            // one VerticalSplitter, regardless of which left pane is
            // visible. Hidden until F4 / F5 toggles a left pane on.
            v_splitter_ = std::make_unique<VerticalSplitter>(cs->hInstance, hwnd);
            if (v_splitter_->hwnd()) {
                ShowWindow(v_splitter_->hwnd(), SW_HIDE);
            }
            v_splitter_->set_on_drag([this](int new_w) {
                // Clamp left dock width: never wider than (client - 100)
                // so the canvas always has at least ~100 px of room, and
                // never narrower than ~120 px (below which the thumb
                // tile placeholders no longer fit centered).
                RECT client; GetClientRect(hwnd_, &client);
                const UINT dpi = GetDpiForWindow(hwnd_);
                const int min_w = MulDiv(120, static_cast<int>(dpi), 96);
                const int max_w = std::max(min_w + 1,
                    static_cast<int>(client.right) - 100);
                left_pane_width_px_ = std::clamp(new_w, min_w, max_w);
                on_layout();
            });

            // Seed left_pane_width_px_ to ~250 dip — matches the prior
            // hard-coded outline_w (no behavior change for users who
            // never resize). DPI-aware via MulDiv.
            {
                const UINT dpi = GetDpiForWindow(hwnd);
                left_pane_width_px_ = MulDiv(250, static_cast<int>(dpi), 96);
            }

            // Phase 7 Task 7 (deferred wiring, completed in T8): install
            // the canvas's page-change observer ONCE here. The callback
            // closes over `this` and looks up the active view's thumb
            // pane on every fire — works correctly across tab switches
            // without re-wiring. The observer is also fired by
            // PdfCanvas::set_view (T7), so a tab switch into a new
            // active view broadcasts the new view's current_page() into
            // its own pane immediately.
            canvas_->set_on_page_changed([this](int page) {
                if (auto* v = active_view()) {
                    if (auto* tp = v->thumb_pane()) {
                        // ThumbnailPane::set_current_page no-ops when
                        // the value matches what the model already has,
                        // so cross-tab search's documented double-fire
                        // (M2 from T7 review: tabs_->set_active fires,
                        // then change_current_page fires again) does
                        // not cause a flash.
                        tp->set_current_page(page);
                    }
                }
            });

            DragAcceptFiles(hwnd, TRUE);
            return 0;
        }
        case WM_DROPFILES: {
            auto hdrop = reinterpret_cast<HDROP>(w);
            UINT count = DragQueryFileW(hdrop, 0xFFFFFFFF, nullptr, 0);
            // Query required buffer length (includes null terminator).
            // Handles paths longer than MAX_PATH (260) on Windows 10 1607+.
            UINT len = (count > 0) ? DragQueryFileW(hdrop, 0, nullptr, 0) : 0;
            std::wstring buf(len, L'\0');
            if (len > 0 && DragQueryFileW(hdrop, 0, buf.data(), len + 1)) {
                std::filesystem::path p(buf);
                // Accept .pdf, .epub, .cbz, .xps (Phase 2 supported formats).
                auto ext = p.extension().wstring();
                std::transform(ext.begin(), ext.end(), ext.begin(),
                    [](wchar_t c) { return static_cast<wchar_t>(::towlower(c)); });
                if (ext == L".pdf" || ext == L".epub" ||
                    ext == L".cbz" || ext == L".xps") {
                    // Canonicalize before push so MRU doesn't accumulate
                    // spelling-variant duplicates of the same file.
                    std::filesystem::path normalized = canonicalize_for_mru(p);
                    mru_.push(normalized.wstring());
                    mru_.save();
                    open_tab_async(std::move(normalized));
                } else {
                    MessageBoxW(hwnd,
                        L"LitePDF only opens PDF/ePub/CBZ/XPS files.",
                        kWindowTitle, MB_ICONINFORMATION);
                }
            }
            DragFinish(hdrop);
            return 0;
        }
        case WM_SIZE: {
            on_layout();
            if (auto* view = active_view()) {
                // Recompute FitWidth (or whatever mode) scale for the new
                // viewport and resubmit. We resubmit on every WM_SIZE;
                // the brief stutter during drag-resize is acceptable.
                kick_render(view->current_page());
            }
            return 0;
        }
        case WM_SETFOCUS:
            if (canvas_) SetFocus(canvas_->hwnd());
            return 0;
        case WM_DPICHANGED: {
            // LPARAM points to a suggested RECT in physical pixels for the new DPI.
            auto* suggested = reinterpret_cast<const RECT*>(l);
            SetWindowPos(hwnd, nullptr,
                         suggested->left, suggested->top,
                         suggested->right - suggested->left,
                         suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            // The canvas (child) receives WM_DPICHANGED_BEFOREPARENT /
            // WM_DPICHANGED_AFTERPARENT; it discards its render target and
            // lets the next paint rebuild at the new DPI. Also re-seed the
            // active tab's zoom scale for the new DPI. Inactive tabs re-seed
            // their own zoom on switch, so a single active-tab kick suffices.
            if (auto* view = active_view(); view && canvas_) {
                kick_render(view->current_page());
            }
            // Phase 7 T8 #2 follow-up: forward new DPI to ALL tabs' thumb
            // panes, not just the active one. Multi-monitor drag between
            // mismatched-DPI displays must update inactive panes' cached
            // tile geometry + GDI brushes too, otherwise switching to an
            // inactive tab after the drag paints at the previous DPI's
            // pixel size until that pane next observes a layout event.
            if (tabs_) {
                for (int i = 0, n = tabs_->count(); i < n; ++i) {
                    auto* t = tabs_->tab_at(i);
                    if (t && t->view) {
                        if (auto* tp = t->view->thumb_pane()) {
                            tp->on_dpi_changed(HIWORD(w));
                        }
                    }
                }
            }
            // The user's left_pane_width_px_ is in physical px at the
            // OLD DPI. The on_layout clamp will keep it inside [120
            // dip, client - 100 px] at the NEW DPI; a too-narrow stored
            // value will snap up to the new min, a too-wide one will
            // snap down. Re-anchoring to 250 dip would lose the user's
            // drag — accept the clamp's slight imperfection instead.
            on_layout();
            return 0;
        }
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
        case WM_NOTIFY: {
            auto* hdr = reinterpret_cast<NMHDR*>(l);
            if (tabs_ && tabs_->handle_notify(hdr)) return 0;
            if (outline_ && hdr && hdr->hwndFrom == outline_->hwnd() &&
                hdr->code == TVN_SELCHANGEDW) {
                auto* nm = reinterpret_cast<NMTREEVIEWW*>(l);
                LPARAM lp = nm->itemNew.lParam;
                if (lp != -1) {
                    on_outline_navigate(static_cast<int>(lp));
                }
                return 0;
            }
            break;  // fall through to DefWindowProc for other notifications
        }
        case WM_INITMENUPOPUP: {
            // Skip window/system menu popups.
            if (HIWORD(l) != 0) return 0;
            auto popup = reinterpret_cast<HMENU>(w);
            HMENU main = GetMenu(hwnd);
            if (!main) return 0;
            // (Phase 8 T3/T4) View popup (index 1): reflect Invert Colors
            // and Two-Page Spread state on each show. The flags are
            // per-tab (D9), so the checkmark is read off active_view().
            // When no tab is open, both default to unchecked.
            if (popup == GetSubMenu(main, 1)) {
                auto* v = active_view();
                const bool invert_on = v && v->invert_colors();
                CheckMenuItem(popup, IDM_VIEW_INVERT,
                              MF_BYCOMMAND
                              | (invert_on ? MF_CHECKED : MF_UNCHECKED));
                return 0;
            }
            // Only rebuild MRU when the File popup (index 0) is about to show.
            if (popup != GetSubMenu(main, 0)) return 0;
            // Remove any existing MRU items + the dynamic SEP (safe when absent).
            DeleteMenu(popup, IDM_MRU_SEPARATOR, MF_BYCOMMAND);
            for (int id = IDM_MRU_1; id <= IDM_MRU_10; ++id) {
                DeleteMenu(popup, id, MF_BYCOMMAND);
            }
            // Bracket: only insert SEP + MRU entries when MRU has content.
            // Empty-MRU layout stays as Open / static SEP / Exit (single
            // separator). With entries: Open / SEP / MRU1..N / SEP / Exit.
            // Insert SEP first (before Exit), then MRU items in order before
            // SEP -- InsertMenuW pushes prior insertions further from anchor,
            // preserving &1, &2, ... order.
            const auto& entries = mru_.entries();
            if (!entries.empty()) {
                InsertMenuW(popup, IDM_FILE_EXIT,
                            MF_BYCOMMAND | MF_SEPARATOR,
                            IDM_MRU_SEPARATOR, nullptr);
                for (std::size_t i = 0; i < entries.size(); ++i) {
                    InsertMenuW(popup, IDM_MRU_SEPARATOR,
                                MF_BYCOMMAND | MF_STRING,
                                IDM_MRU_1 + static_cast<UINT>(i),
                                format_mru_label(i, entries[i]).c_str());
                }
            }
            return 0;
        }
        case WM_COMMAND: {
            const int id = LOWORD(w);
            // MRU range: click reopens the file, or warns + removes if gone.
            // Plan: MRU click does NOT re-rank (no mru_.push here); only real
            // Open/Drop sites push. Per D4 literal text.
            if (id >= IDM_MRU_1 && id <= IDM_MRU_10) {
                std::size_t index = static_cast<std::size_t>(id - IDM_MRU_1);
                const auto& e = mru_.entries();
                if (index < e.size()) {
                    std::filesystem::path p(e[index]);
                    // Non-throwing exists(): MRU paths go stale on removable
                    // drives or disconnected SMB shares, and the throwing
                    // overload would propagate out of handle_message and
                    // crash the UI thread. Any error (permission denied,
                    // network unreachable, ENOENT) collapses to "gone".
                    std::error_code ec;
                    if (std::filesystem::exists(p, ec)) {
                        open_tab_async(std::move(p));
                    } else {
                        MessageBoxW(hwnd,
                            (L"File not found:\n" + e[index]).c_str(),
                            kWindowTitle, MB_OK | MB_ICONWARNING);
                        mru_.remove(index);
                        mru_.save();
                    }
                }
                return 0;
            }
            // Ctrl+1..9 jumps to tab N. core::goto_tab_index returns -1
            // for the silent-no-op case (e.g., Ctrl+5 with 3 tabs).
            if (id >= IDM_TAB_GOTO_1 && id <= IDM_TAB_GOTO_9) {
                if (tabs_) {
                    const int one_indexed = id - IDM_TAB_GOTO_1 + 1;
                    const int target = litepdf::core::goto_tab_index(
                        one_indexed, tabs_->count());
                    if (target >= 0) tabs_->set_active(target);
                }
                return 0;
            }
            switch (id) {
                case IDM_VIEW_OUTLINE:
                    toggle_outline();
                    return 0;
                case IDM_VIEW_THUMBS:
                    toggle_thumbs();
                    return 0;
                case IDM_VIEW_INVERT: {
                    // Phase 8 D7/D9: per-tab Invert Colors toggle. Flips
                    // the engine flag (which drains in-flight renders at
                    // the old polarity), flips the canvas chrome
                    // palette, and kicks a fresh render of the active
                    // page so the new polarity lands immediately.
                    auto* v = active_view();
                    if (!v) return 0;
                    v->set_invert_colors(!v->invert_colors());
                    if (canvas_) canvas_->set_invert_chrome(v->invert_colors());
                    kick_render(v->current_page());
                    return 0;
                }
                case IDM_TAB_CLOSE:
                    if (tabs_ && tabs_->count() > 0) {
                        on_tab_close_request(tabs_->active_index());
                    }
                    return 0;
                case IDM_TAB_NEXT:
                    if (tabs_) {
                        const int n = litepdf::core::next_tab_index(
                            tabs_->active_index(), tabs_->count());
                        if (n >= 0) tabs_->set_active(n);
                    }
                    return 0;
                case IDM_TAB_PREV:
                    if (tabs_) {
                        const int n = litepdf::core::prev_tab_index(
                            tabs_->active_index(), tabs_->count());
                        if (n >= 0) tabs_->set_active(n);
                    }
                    return 0;
                case IDM_FILE_OPEN: {
                    wchar_t buf[MAX_PATH] = {0};
                    OPENFILENAMEW ofn = {0};
                    ofn.lStructSize = sizeof(ofn);
                    ofn.hwndOwner   = hwnd;
                    ofn.lpstrFile   = buf;
                    ofn.nMaxFile    = MAX_PATH;
                    ofn.lpstrFilter =
                        L"PDF files (*.pdf)\0*.pdf\0All files\0*.*\0";
                    ofn.nFilterIndex = 1;
                    ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY |
                                OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST;
                    if (GetOpenFileNameW(&ofn)) {
                        // Canonicalize before push so MRU doesn't accumulate
                        // spelling-variant duplicates of the same file.
                        std::filesystem::path normalized =
                            canonicalize_for_mru(std::filesystem::path(buf));
                        mru_.push(normalized.wstring());
                        mru_.save();
                        open_tab_async(std::move(normalized));
                    }
                    return 0;
                }
                case IDM_FILE_EXIT:
                    DestroyWindow(hwnd);
                    return 0;
                case IDM_HELP_ABOUT:
                    MessageBoxW(hwnd,
                        L"LitePDF v0.0.8\n\n"
                        L"A lightweight PDF / ePub / CBZ / XPS viewer for Windows.\n\n"
                        L"License: AGPL-3.0\n"
                        L"Engine: MuPDF 1.24.11\n"
                        L"Rendering: Direct2D",
                        kWindowTitle, MB_ICONINFORMATION);
                    return 0;
                case IDM_ZOOM_IN:
                    if (auto* view = active_view();
                        view && view->zoom_in()) {
                        kick_render(view->current_page());
                    }
                    return 0;
                case IDM_ZOOM_OUT:
                    if (auto* view = active_view();
                        view && view->zoom_out()) {
                        kick_render(view->current_page());
                    }
                    return 0;
                case IDM_ZOOM_RESET: {
                    if (auto* view = active_view(); view && canvas_) {
                        RECT rc; GetClientRect(canvas_->hwnd(), &rc);
                        UINT dpi = GetDpiForWindow(hwnd);
                        view->set_zoom_mode(
                            litepdf::core::DocumentView::ZoomMode::FitWidth,
                            static_cast<float>(rc.right - rc.left),
                            static_cast<float>(rc.bottom - rc.top),
                            static_cast<float>(dpi));
                        kick_render(view->current_page());
                    }
                    return 0;
                }
                // Phase 6 Task 10: find bar keyboard entrypoints.
                case IDM_FIND:
                    on_find_open();
                    return 0;
                case IDM_FIND_NEXT:
                    on_find_next();
                    return 0;
                case IDM_FIND_PREV:
                    on_find_prev();
                    return 0;
                case IDM_FIND_CLOSE:
                    // Scope ESC: only claim it when the find bar is the
                    // active UI. Otherwise fall through to DefWindowProc
                    // so other consumers (future modal dialogs, etc.) can
                    // see ESC as well.
                    if (find_bar_ && find_bar_->visible()) {
                        on_find_close();
                        return 0;
                    }
                    return 0;
                case IDM_CROSS_TAB_FIND:
                    on_cross_tab_find();
                    return 0;
                case IDM_TOGGLE_RESULTS:
                    on_toggle_results();
                    return 0;
            }
            return 0;
        }
        case WM_USER_OPEN_OK: {
            auto* raw = reinterpret_cast<litepdf::core::Tab*>(w);
            std::unique_ptr<litepdf::core::Tab> t(raw);
            ColdStartTimer::mark(2);  // Document fully loaded.

            // Phase 6 Task 10: install SearchSession observer BEFORE
            // handing ownership to tabs_. Observer fires on a worker
            // thread; we marshal to UI thread via PostMessage. Capture
            // hwnd_ by value — the lambda outlives `this` only if the
            // worker callback fires after MainWindow destruction, in
            // which case PostMessage to a dead hwnd is a silent no-op.
            if (t && t->view) {
                HWND target = hwnd_;
                t->view->search().set_on_update([target]() {
                    PostMessageW(target, WM_USER_SEARCH_UPDATE, 0, 0);
                });
            }

            const int new_index = tabs_->add_tab(std::move(t));
            // add_tab() fires on_switch synchronously with new_index -- that
            // callback is where canvas/outline/layout/kick_render happens
            // (and, via on_tab_switch, update_canvas_hits_source()).
            (void)new_index;
            return 0;
        }
        case WM_USER_OPEN_FAILED: {
            using OE = litepdf::core::Document::OpenError;
            const wchar_t* emsg = L"Failed to open the document.";
            switch (static_cast<OE>(w)) {
                case OE::FileNotFound:
                    emsg = L"File not found or inaccessible."; break;
                case OE::UnsupportedFormat:
                    emsg = L"Unsupported file format."; break;
                case OE::NeedsPassword:
                    // Phase 8 Task 1: NeedsPassword is now handled inline
                    // via WM_USER_PASSWORD_PROMPT; reaching this branch
                    // would mean the worker mis-routed. Fall through to
                    // the generic message rather than misleading the user.
                    emsg = L"Failed to open the document.";
                    break;
                case OE::BadPassword:
                    emsg = L"Incorrect password."; break;
                case OE::Corrupted:
                    emsg = L"Document is corrupted."; break;
                case OE::OutOfMemory:
                    emsg = L"Out of memory while opening the document."; break;
                case OE::Other:
                default:
                    break;
            }
            // Adopt ownership of the path string the worker allocated.
            // unique_ptr handles the delete even if MessageBoxW throws.
            std::unique_ptr<std::wstring> failed_path(
                reinterpret_cast<std::wstring*>(l));
            std::wstring full_msg = emsg;
            if (failed_path && !failed_path->empty()) {
                full_msg += L"\n\n";
                full_msg += *failed_path;
            }
            // Note: MessageBoxW runs a nested message loop. If another
            // worker posts WM_USER_OPEN_OK during the dialog, its
            // add_tab() + on_tab_switch() will run before this case
            // returns. Surfaces as "dialog dismissed -> active tab
            // changed". Non-fatal; revisit in Phase 5 Task 8 (or
            // Phase 12 crash-protection hardening) if the UX becomes
            // confusing.
            MessageBoxW(hwnd, full_msg.c_str(), kWindowTitle, MB_ICONWARNING);
            return 0;
        }
        case WM_USER_PASSWORD_PROMPT: {
            // Phase 8 Task 1: encrypted-PDF handshake. We adopt the
            // Document the worker handed us, run up to 3 prompt+auth
            // attempts inline on the UI thread (D2 addendum), and either
            // construct a Tab on success or drop the bundle on cancel /
            // exhaustion. MRU is NOT pruned on failure (design §6.1).
            std::unique_ptr<PendingPasswordOpen> bundle(
                reinterpret_cast<PendingPasswordOpen*>(w));
            if (!bundle) return 0;
            const std::wstring basename = bundle->path.filename().wstring();

            auto result = litepdf::ui::try_authenticate_with_retry(
                [parent = hwnd_, &basename](const std::wstring& status) {
                    return litepdf::ui::PasswordDialog::prompt(
                        parent, basename, status);
                },
                [&bundle](const std::string& pw) {
                    return bundle->doc.authenticate(pw);
                },
                [](int failed_count) {
                    // R10: "remaining" reads less anxious than "of 3" and
                    // matches Acrobat / Foxit phrasing.
                    const int remaining = 3 - failed_count;
                    if (remaining == 1) {
                        return std::wstring{
                            L"Incorrect password. (1 attempt remaining.)"};
                    }
                    return L"Incorrect password. ("
                         + std::to_wstring(remaining)
                         + L" attempts remaining.)";
                });

            if (!result.accepted) {
                // R11: only notify the user on attempt-exhaustion; Cancel
                // is a silent close (the user already chose to dismiss).
                if (result.attempts >= 3) {
                    std::wstring full_msg =
                        L"Failed to open " + basename
                        + L": password incorrect after 3 attempts.";
                    MessageBoxW(hwnd, full_msg.c_str(), kWindowTitle,
                                MB_OK | MB_ICONWARNING);
                }
                return 0;
            }

            // Authenticated. Build the Tab + DocumentView on the UI
            // thread (mirrors the worker-side path of WM_USER_OPEN_OK
            // but we avoid bouncing back to a worker for the few-µs
            // construction). If construction throws (clone_context OOM,
            // worker thread creation), surface the same generic error
            // path the original worker uses.
            try {
                auto tab = std::make_unique<litepdf::core::Tab>();
                tab->path  = bundle->path;
                tab->label = basename;
                tab->view  = std::make_unique<litepdf::core::DocumentView>(
                    std::move(bundle->doc), *search_dispatcher_);

                ColdStartTimer::mark(2);  // Document fully loaded.

                HWND target = hwnd_;
                tab->view->search().set_on_update([target]() {
                    PostMessageW(target, WM_USER_SEARCH_UPDATE, 0, 0);
                });
                (void)tabs_->add_tab(std::move(tab));
            } catch (...) {
                MessageBoxW(hwnd,
                            L"Failed to open the document after authentication.",
                            kWindowTitle, MB_OK | MB_ICONWARNING);
            }
            return 0;
        }
        case WM_USER_SEARCH_UPDATE:
            on_search_update_posted();
            return 0;
        case WM_USER_CROSS_TAB_UPDATE:
            if (results_panel_) results_panel_->refresh_count();
            return 0;
        case WM_COPYDATA:
            return on_copydata(hwnd, w, l);
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, w, l);
}

// ---------------- Phase 6 Task 10: find-bar helpers -----------------

void MainWindow::update_canvas_hits_source() {
    if (!canvas_) return;
    // Look up the currently-active view on every paint, rather than
    // capturing a raw DocumentView* here: on tab switch the target
    // session changes but the HitsFn binding stays the same. If there
    // is no active view, return empty.
    canvas_->set_hits_source(
        [this](std::size_t page)
            -> std::vector<litepdf::core::SearchSession::Hit>
        {
            auto* v = active_view();
            if (!v) return {};
            return v->search().hits_for_page(page);
        });
}

void MainWindow::on_find_open() {
    if (!find_bar_ || !active_view()) return;  // no tab, no find
    find_bar_->show_or_focus(last_find_query_);
    // Layout again — reposition() only runs when visible, and we just
    // flipped visibility on.
    on_layout();
    update_find_counter();
}

void MainWindow::on_find_query_changed(const std::wstring& q, bool mc) {
    last_find_query_ = q;
    auto* v = active_view();
    if (!v) return;
    v->search().set_query(q, {mc});
    update_find_counter();
    if (canvas_) {
        canvas_->set_current_hit(std::nullopt);
        InvalidateRect(canvas_->hwnd(), nullptr, FALSE);
    }
}

void MainWindow::on_find_next() {
    auto* v = active_view();
    if (!v || !canvas_) return;
    auto h = v->search().next();
    if (!h) return;
    canvas_->set_current_hit(*h);
    canvas_->scroll_into_view(*h);
    // scroll_into_view may have switched the current page; kick a
    // render for the (possibly new) page.
    kick_render(v->current_page());
    update_find_counter();
}

void MainWindow::on_find_prev() {
    auto* v = active_view();
    if (!v || !canvas_) return;
    auto h = v->search().prev();
    if (!h) return;
    canvas_->set_current_hit(*h);
    canvas_->scroll_into_view(*h);
    kick_render(v->current_page());
    update_find_counter();
}

void MainWindow::on_find_close() {
    if (!find_bar_ || !find_bar_->visible()) return;
    find_bar_->hide();
    if (canvas_) {
        canvas_->set_current_hit(std::nullopt);
    }
    // Phase 6 design Q7 7c: ESC clears highlights. clear() wipes the
    // hit cache and fires on_update so the canvas repaints with no
    // hits. A subsequent Ctrl+F reopens the bar with last_find_query_
    // prefilled, at which point the first keystroke (or an Enter)
    // re-runs the scan.
    if (auto* v = active_view()) {
        v->search().clear();
    }
    update_find_counter();
    if (canvas_) {
        SetFocus(canvas_->hwnd());
        InvalidateRect(canvas_->hwnd(), nullptr, FALSE);
    }
}

void MainWindow::update_find_counter() {
    if (!find_bar_) return;
    auto* v = active_view();
    if (!v) {
        find_bar_->set_counter(L"");
        return;
    }
    const std::size_t n   = v->search().hit_count();
    const bool        eof = v->search().scan_complete();
    if (n == 0) {
        find_bar_->set_counter(eof ? std::wstring(L"") : std::wstring(L"…"));
        return;
    }
    wchar_t buf[48];
    // TODO(phase-6.x): surface "m / n" current/total once SearchSession
    // exposes a cursor_index() accessor. Showing totals only for v1.
    if (eof) {
        _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"%zu hits", n);
    } else {
        _snwprintf_s(buf, _countof(buf), _TRUNCATE,
                     L"%zu hits (scanning)", n);
    }
    find_bar_->set_counter(buf);
}

void MainWindow::on_search_update_posted() {
    update_find_counter();
    if (canvas_) {
        InvalidateRect(canvas_->hwnd(), nullptr, FALSE);
    }
}

// ---------------- Phase 6 Tasks 11-14: cross-tab search --------------

void MainWindow::on_cross_tab_find() {
    if (!active_view()) return;      // nothing to search
    if (!results_panel_) return;

    // First-show sizing: one-third of the client height, floored at
    // 200 px so the ListView always has room for a handful of rows.
    if (results_panel_height_px_ == 0) {
        RECT client; GetClientRect(hwnd_, &client);
        results_panel_height_px_ = std::max(200,
            static_cast<int>(client.bottom - client.top) / 3);
    }
    results_panel_->show_and_focus_edit();
    on_layout();
}

void MainWindow::on_toggle_results() {
    if (!results_panel_) return;
    if (results_panel_->visible()) {
        results_panel_->hide();
    } else {
        if (!active_view()) return;  // nothing to search
        if (results_panel_height_px_ == 0) {
            RECT client; GetClientRect(hwnd_, &client);
            results_panel_height_px_ = std::max(200,
            static_cast<int>(client.bottom - client.top) / 3);
        }
        results_panel_->show_and_focus_edit();
    }
    on_layout();
}

void MainWindow::on_results_query(const std::wstring& q) {
    if (!cross_tab_ || !tabs_) return;

    // Snapshot open tabs for fan-out. Per §D9 the tab list at dispatch
    // time is frozen; tabs opened afterward are not joined.
    std::vector<litepdf::app::CrossTabSearch::TabRef> snapshot;
    snapshot.reserve(static_cast<std::size_t>(tabs_->count()));
    for (int i = 0; i < tabs_->count(); ++i) {
        auto* t = tabs_->tab_at(i);
        if (!t || !t->view) continue;
        std::wstring label = t->path.filename().wstring();
        if (label.empty()) label = t->label.empty() ? L"Untitled" : t->label;
        snapshot.push_back({t->view.get(), std::move(label)});
    }

    // Cross-tab is independent of the find-bar's case toggle per design —
    // an empty Flags{} matches the default (case-insensitive). Revisit if
    // the results panel gains its own case-toggle checkbox.
    litepdf::core::SearchSession::Flags f{};
    cross_tab_->dispatch(q, f, std::move(snapshot));

    // Refresh immediately so an empty-query / no-hits case doesn't leave
    // a stale row count visible until the first worker post lands.
    if (results_panel_) results_panel_->refresh_count();
}

void MainWindow::on_results_row_click(std::size_t idx) {
    if (!cross_tab_ || !tabs_ || !canvas_) return;

    const auto all_hits = cross_tab_->hits();  // snapshot copy
    if (idx >= all_hits.size()) return;
    const auto& h = all_hits[idx];

    // Liveness check: sentinel expires on clear() or the next dispatch().
    // If expired, the owning tab may also be gone; bail without navigation
    // to avoid dereferencing a dangling view_at_submit.
    if (h.session_state.expired()) return;

    // Resolve the tab whose DocumentView produced this hit. Linear search
    // is fine for Phase 6.2 (<=1000 hits, <=32 tabs in practice).
    int target_idx = -1;
    for (int i = 0; i < tabs_->count(); ++i) {
        auto* t = tabs_->tab_at(i);
        if (t && t->view.get() == h.view_at_submit) {
            target_idx = i;
            break;
        }
    }
    if (target_idx < 0) return;

    tabs_->set_active(target_idx);
    auto* v = active_view();
    if (!v || !canvas_) return;
    // Route through PdfCanvas::change_current_page so the T7 page-change
    // observer fires for cross-tab search jumps too. set_active above
    // already triggered on_tab_switch -> canvas_->set_view, which fired
    // the observer with the incoming tab's stored page; this second
    // fire reflects the search-jump's target page.
    canvas_->change_current_page(static_cast<int>(h.page));

    // Recompose a Hit for the canvas overlay + scroll. SearchSession::Hit
    // and CrossTabSearch::Hit share the (page, geom) pair; we copy into
    // the canvas-native shape.
    litepdf::core::SearchSession::Hit sh{h.page, h.geom};
    canvas_->set_current_hit(sh);
    canvas_->scroll_into_view(sh);
    kick_render(v->current_page());
}

void MainWindow::on_results_close() {
    if (!results_panel_) return;
    results_panel_->hide();
    // Drop the cross-tab sentinel (stale hits on this panel after reopen
    // would otherwise keep weak_ptrs alive) and restore each tab's
    // previous on_update observer so the per-tab find-bar counter
    // resumes immediately on Ctrl+F (I1 fix in CrossTabSearch::clear).
    if (cross_tab_) cross_tab_->clear();
    on_layout();
    if (canvas_) SetFocus(canvas_->hwnd());
}

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
        // IPC sender already canonicalizes before forwarding (see main.cpp
        // — sender's CWD is the authoritative context). We canonicalize
        // again here as a safety net in case an older sender / third-party
        // caller skipped sender-side normalization.
        std::filesystem::path normalized = canonicalize_for_mru(p);
        mru_.push(normalized.wstring());
        mru_.save();
        open_tab_async(std::move(normalized));
        return 1;
    }
    return 0;
}

int MainWindow::run(HINSTANCE hInstance, int nCmdShow,
                    const std::filesystem::path& initial_path) {
    INITCOMMONCONTROLSEX icc = { sizeof(icc),
        ICC_STANDARD_CLASSES | ICC_TAB_CLASSES |
        ICC_TREEVIEW_CLASSES | ICC_LISTVIEW_CLASSES };
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = kWindowClassName;
    wc.lpszMenuName  = nullptr;  // attach per-instance via CreateWindowEx
    if (!RegisterClassExW(&wc)) return 1;

    HMENU menu = LoadMenuW(hInstance, MAKEINTRESOURCEW(IDM_MAIN_MENU));

    // WS_CLIPCHILDREN: belt-and-braces with WS_CLIPSIBLINGS on PdfCanvas.
    // Ensures the frame window's paint never touches any child HWND's
    // area. Important for Phase 6 FindBar, which floats over the canvas
    // and would otherwise be clobbered by the frame's background paint.
    hwnd_ = CreateWindowExW(
        0, kWindowClassName, kWindowTitle,
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 1024, 768,
        nullptr, menu, hInstance, this);
    if (!hwnd_) return 1;

    // Accelerators: Ctrl+O for Open; Ctrl+= / Ctrl+- / Ctrl+0 for zoom;
    // F5 toggles the outline pane; F4 reserved for the Phase 7 thumbnail
    // pane (registered now so the accelerator slot is taken — handler
    // lands in Phase 7 Task 8). Phase 5 adds tab management:
    // Ctrl+W closes active tab, Ctrl+Tab / Ctrl+Shift+Tab cycles,
    // Ctrl+1..9 jumps to tab N.
    ACCEL accels[] = {
        { FCONTROL | FVIRTKEY, 'O',          IDM_FILE_OPEN     },
        { FCONTROL | FVIRTKEY, VK_OEM_PLUS,  IDM_ZOOM_IN       },  // Ctrl+=
        { FCONTROL | FVIRTKEY, VK_OEM_MINUS, IDM_ZOOM_OUT      },  // Ctrl+-
        { FCONTROL | FVIRTKEY, '0',          IDM_ZOOM_RESET    },
        { FVIRTKEY,            VK_F4,        IDM_VIEW_THUMBS   },
        { FVIRTKEY,            VK_F5,        IDM_VIEW_OUTLINE  },
        // Phase 8: Tier 3 view-mode toggles. Modifier-consistent
        // Ctrl+Shift+_ pair so they read as a deliberate group; bare
        // Ctrl+D is reserved for a future "Duplicate Tab" affordance.
        { FCONTROL | FSHIFT | FVIRTKEY, 'I', IDM_VIEW_INVERT    },
        { FCONTROL | FSHIFT | FVIRTKEY, 'D', IDM_VIEW_DUAL_PAGE },
        // Phase 5: tab management.
        { FCONTROL | FVIRTKEY,          'W',     IDM_TAB_CLOSE  },
        { FCONTROL | FVIRTKEY,          VK_TAB,  IDM_TAB_NEXT   },
        { FCONTROL | FSHIFT | FVIRTKEY, VK_TAB,  IDM_TAB_PREV   },
        { FCONTROL | FVIRTKEY, '1', IDM_TAB_GOTO_1 },
        { FCONTROL | FVIRTKEY, '2', IDM_TAB_GOTO_2 },
        { FCONTROL | FVIRTKEY, '3', IDM_TAB_GOTO_3 },
        { FCONTROL | FVIRTKEY, '4', IDM_TAB_GOTO_4 },
        { FCONTROL | FVIRTKEY, '5', IDM_TAB_GOTO_5 },
        { FCONTROL | FVIRTKEY, '6', IDM_TAB_GOTO_6 },
        { FCONTROL | FVIRTKEY, '7', IDM_TAB_GOTO_7 },
        { FCONTROL | FVIRTKEY, '8', IDM_TAB_GOTO_8 },
        { FCONTROL | FVIRTKEY, '9', IDM_TAB_GOTO_9 },
        // Phase 6: in-doc find + (stubbed) cross-tab find + results
        // panel toggle. ESC only fires when the find bar is the active
        // UI — see IDM_FIND_CLOSE handler in WM_COMMAND above.
        { FCONTROL | FVIRTKEY,          'F',       IDM_FIND           },
        { FVIRTKEY,                     VK_F3,     IDM_FIND_NEXT      },
        { FSHIFT   | FVIRTKEY,          VK_F3,     IDM_FIND_PREV      },
        { FCONTROL | FSHIFT | FVIRTKEY, 'F',       IDM_CROSS_TAB_FIND },
        { FVIRTKEY,                     VK_F6,     IDM_TOGGLE_RESULTS },
        { FVIRTKEY,                     VK_ESCAPE, IDM_FIND_CLOSE     },
    };
    haccel_ = CreateAcceleratorTableW(accels, _countof(accels));

    ShowWindow(hwnd_, nCmdShow);
    ColdStartTimer::mark(1);  // Window visible.
    UpdateWindow(hwnd_);

    if (!initial_path.empty()) {
        // Pre-existing bug (not Phase 5): initial-path open via double-click
        // / command-line argv bypassed mru_.push, so the user's first-ever
        // open never entered MRU. Folded into Task 8's canonicalization
        // sweep since the fix site is identical. Sender-side (main.cpp)
        // already canonicalizes before handing us initial_path, but we
        // re-canonicalize defensively for consistency with the other three
        // call sites.
        std::filesystem::path normalized = canonicalize_for_mru(initial_path);
        mru_.push(normalized.wstring());
        mru_.save();
        open_tab_async(std::move(normalized));
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (haccel_ && TranslateAcceleratorW(hwnd_, haccel_, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}

}  // namespace litepdf::ui
