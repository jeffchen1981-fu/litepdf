// LitePDF -- ui::MainWindow: top-level window, menu, message pump.
#include "ui/MainWindow.hpp"
#include "MainMenu.rc.h"

#include "app/SingleInstance.hpp"
#include "core/Document.hpp"
#include "core/DocumentView.hpp"
#include "core/TabList.hpp"
#include "ui/ColdStartTimer.hpp"

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

// Forward-decl so we don't have to pull <mupdf/fitz.h> into the UI TU.
// fz_context / fz_pixmap are forward-declared via core/DocumentView.hpp.
extern "C" {
    struct fz_pixmap* fz_keep_pixmap(struct fz_context*, struct fz_pixmap*);
}

namespace {
constexpr wchar_t kWindowClassName[] = L"LitePDFMainWindow";
constexpr wchar_t kWindowTitle[]     = L"LitePDF";

constexpr UINT WM_USER_OPEN_OK     = WM_USER + 1;
constexpr UINT WM_USER_OPEN_FAILED = WM_USER + 2;
// Must match ui::WM_USER_RENDER_DONE in PdfCanvas.hpp.
constexpr UINT WM_USER_RENDER_DONE = WM_USER + 3;

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

MainWindow::MainWindow() { mru_.load(); }
MainWindow::~MainWindow() {
    if (haccel_) { DestroyAcceleratorTable(haccel_); haccel_ = nullptr; }
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
            if (p) fz_keep_pixmap(worker_ctx, p);  // extend lifetime across PostMessage
            PostMessageW(target, WM_USER_RENDER_DONE,
                         reinterpret_cast<WPARAM>(p), 0);
        });
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
    const int row_h = std::max(0, h - tab_h);

    const int outline_w = MulDiv(250, static_cast<int>(dpi), 96);

    if (outline_ && outline_->visible()) {
        SetWindowPos(outline_->hwnd(), nullptr,
                     0, row_y, outline_w, row_h,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        if (canvas_) {
            // Guard: if window is narrower than the outline pane, clamp
            // canvas width to 0 rather than passing a negative value to
            // SetWindowPos (undefined behavior).
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

void MainWindow::toggle_outline() {
    if (!outline_) return;
    if (outline_->visible()) outline_->hide(); else outline_->show();
    on_layout();
    // Rendering follows the new canvas size so FitWidth stays correct.
    // on_tab_switch's outgoing-snapshot writes outline_visible back into
    // the active Tab when the user switches away -- that's the single
    // source of truth, so we don't write it here.
    if (auto* view = active_view()) kick_render(view->current_page());
}

void MainWindow::on_outline_navigate(int page) {
    auto* view = active_view();
    if (!view) return;
    if (view->set_current_page(page)) {
        kick_render(page);
    }
}

void MainWindow::on_tab_switch(int new_index, int old_index) {
    // Snapshot outgoing state + drain its render queue to narrow the
    // cross-tab render-bleed race window. If a P0 is already with a
    // worker, its pixmap will still arrive at WM_USER_RENDER_DONE after
    // the set_view() below -- canvas handling of that edge is tracked
    // as a Phase 6 hardening item (see plan "Known Limitations").
    // Cancelling P0/P1/P2 here drops all queued work, leaving at most
    // one in-flight pixmap per worker thread.
    if (old_index >= 0) {
        if (auto* outgoing = tabs_->tab_at(old_index)) {
            auto p = canvas_ ? canvas_->pan() : PdfCanvas::Pan{0.0f, 0.0f};
            outgoing->pan_x = p.x;
            outgoing->pan_y = p.y;
            outgoing->outline_visible = outline_ && outline_->visible();
            if (outgoing->view) {
                outgoing->view->cancel_stale_renders(INT_MAX);
            }
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

    (void)new_index;
}

void MainWindow::on_tab_close_request(int index) {
    if (!tabs_) return;
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
            return 0;
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
            // Only rebuild MRU when the File popup (index 0) is about to show.
            if (!main || popup != GetSubMenu(main, 0)) return 0;
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
            // Ctrl+1..9 jumps to tab N (0-indexed internally). No-op if
            // the requested tab doesn't exist (e.g., Ctrl+5 with 3 tabs).
            if (id >= IDM_TAB_GOTO_1 && id <= IDM_TAB_GOTO_9) {
                const int target = id - IDM_TAB_GOTO_1;  // 0-indexed
                if (tabs_ && target < tabs_->count()) {
                    tabs_->set_active(target);
                }
                return 0;
            }
            switch (id) {
                case IDM_VIEW_OUTLINE:
                    toggle_outline();
                    return 0;
                case IDM_TAB_CLOSE:
                    if (tabs_ && tabs_->count() > 0) {
                        on_tab_close_request(tabs_->active_index());
                    }
                    return 0;
                case IDM_TAB_NEXT:
                    if (tabs_ && tabs_->count() > 1) {
                        const int n = (tabs_->active_index() + 1) % tabs_->count();
                        tabs_->set_active(n);
                    }
                    return 0;
                case IDM_TAB_PREV:
                    if (tabs_ && tabs_->count() > 1) {
                        const int n = (tabs_->active_index() - 1 + tabs_->count()) % tabs_->count();
                        tabs_->set_active(n);
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
                        L"LitePDF v0.0.6\n\n"
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
            }
            return 0;
        }
        case WM_USER_OPEN_OK: {
            auto* raw = reinterpret_cast<litepdf::core::Tab*>(w);
            std::unique_ptr<litepdf::core::Tab> t(raw);
            ColdStartTimer::mark(2);  // Document fully loaded.

            const int new_index = tabs_->add_tab(std::move(t));
            // add_tab() fires on_switch synchronously with new_index -- that
            // callback is where canvas/outline/layout/kick_render happens.
            // So here we have nothing further to do.
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
                    emsg = L"Password-protected PDFs are not yet supported.";
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
            // Note: MessageBoxW runs a nested message loop. If another
            // worker posts WM_USER_OPEN_OK during the dialog, its
            // add_tab() + on_tab_switch() will run before this case
            // returns. Surfaces as "dialog dismissed -> active tab
            // changed". Non-fatal; revisit in Phase 5 Task 8 (or
            // Phase 12 crash-protection hardening) if the UX becomes
            // confusing.
            MessageBoxW(hwnd, emsg, kWindowTitle, MB_ICONWARNING);
            return 0;
        }
        case WM_COPYDATA:
            return on_copydata(hwnd, w, l);
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, w, l);
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

    hwnd_ = CreateWindowExW(
        0, kWindowClassName, kWindowTitle, WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 1024, 768,
        nullptr, menu, hInstance, this);
    if (!hwnd_) return 1;

    // Accelerators: Ctrl+O for Open; Ctrl+= / Ctrl+- / Ctrl+0 for zoom;
    // F5 toggles the outline pane. Phase 5 adds tab management:
    // Ctrl+W closes active tab, Ctrl+Tab / Ctrl+Shift+Tab cycles,
    // Ctrl+1..9 jumps to tab N.
    ACCEL accels[] = {
        { FCONTROL | FVIRTKEY, 'O',          IDM_FILE_OPEN     },
        { FCONTROL | FVIRTKEY, VK_OEM_PLUS,  IDM_ZOOM_IN       },  // Ctrl+=
        { FCONTROL | FVIRTKEY, VK_OEM_MINUS, IDM_ZOOM_OUT      },  // Ctrl+-
        { FCONTROL | FVIRTKEY, '0',          IDM_ZOOM_RESET    },
        { FVIRTKEY,            VK_F5,        IDM_VIEW_OUTLINE  },
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
