// LitePDF — ui::MainWindow: top-level window, menu, message pump.
#include "ui/MainWindow.hpp"
#include "MainMenu.rc.h"

#include "core/Document.hpp"
#include "ui/ColdStartTimer.hpp"

#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <stdlib.h>
#include <algorithm>
#include <atomic>
#include <cwctype>
#include <filesystem>
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
}  // namespace

namespace litepdf::ui {

MainWindow::MainWindow() = default;
MainWindow::~MainWindow() {
    if (haccel_) { DestroyAcceleratorTable(haccel_); haccel_ = nullptr; }
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

void MainWindow::open_async(std::filesystem::path path) {
    int my_epoch = ++open_epoch_;
    HWND hwnd = hwnd_;
    std::thread([hwnd, path = std::move(path), my_epoch]() {
        litepdf::core::Document doc;
        auto err = doc.open(path);
        if (err.has_value()) {
            PostMessageW(hwnd, WM_USER_OPEN_FAILED,
                         static_cast<WPARAM>(*err),
                         static_cast<LPARAM>(my_epoch));
            return;
        }
        try {
            auto* dv = new litepdf::core::DocumentView(std::move(doc));
            PostMessageW(hwnd, WM_USER_OPEN_OK,
                         reinterpret_cast<WPARAM>(dv),
                         static_cast<LPARAM>(my_epoch));
        } catch (...) {
            // DocumentView ctor can throw (e.g., ui-ctx clone or worker
            // thread creation failed). Map to the closest existing error.
            PostMessageW(hwnd, WM_USER_OPEN_FAILED,
                         static_cast<WPARAM>(
                             litepdf::core::Document::OpenError::Other),
                         static_cast<LPARAM>(my_epoch));
        }
    }).detach();
}

void MainWindow::kick_render(int page) {
    if (!view_ || !canvas_) return;

    RECT rc;
    GetClientRect(canvas_->hwnd(), &rc);
    UINT dpi = GetDpiForWindow(hwnd_);
    view_->set_zoom_mode(view_->zoom_mode(),
        static_cast<float>(rc.right - rc.left),
        static_cast<float>(rc.bottom - rc.top),
        static_cast<float>(dpi));

    HWND target = canvas_->hwnd();
    view_->request_render_with_prefetch(page,
        [target](fz_pixmap* p, fz_context* worker_ctx) {
            if (p) fz_keep_pixmap(worker_ctx, p);  // extend lifetime across PostMessage
            PostMessageW(target, WM_USER_RENDER_DONE,
                         reinterpret_cast<WPARAM>(p), 0);
        });
}

LRESULT MainWindow::handle_message(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
        case WM_CREATE: {
            canvas_ = std::make_unique<PdfCanvas>(
                reinterpret_cast<CREATESTRUCTW*>(l)->hInstance, hwnd);
            canvas_->set_log_timings(log_timings_);
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
                    open_async(std::move(p));
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
            if (canvas_)
                SetWindowPos(canvas_->hwnd(), nullptr,
                             0, 0, LOWORD(l), HIWORD(l),
                             SWP_NOZORDER | SWP_NOACTIVATE);
            if (view_) {
                // Recompute FitWidth (or whatever mode) scale for the new
                // viewport and resubmit. Phase 3 does this directly; the
                // brief stutter during drag-resize is acceptable.
                kick_render(view_->current_page());
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
            // lets the next paint rebuild at the new DPI. Phase 3 Task 12:
            // also recompute the zoom scale for the new DPI and kick a fresh
            // render so the canvas isn't left empty after the DPI transition.
            if (view_ && canvas_) {
                kick_render(view_->current_page());
            }
            return 0;
        }
        case WM_COMMAND: {
            const int id = LOWORD(w);
            switch (id) {
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
                        open_async(std::filesystem::path(buf));
                    }
                    return 0;
                }
                case IDM_FILE_EXIT:
                    DestroyWindow(hwnd);
                    return 0;
                case IDM_HELP_ABOUT:
                    MessageBoxW(hwnd,
                        L"LitePDF v0.0.4\n\n"
                        L"A lightweight PDF / ePub / CBZ / XPS viewer for Windows.\n\n"
                        L"License: AGPL-3.0\n"
                        L"Engine: MuPDF 1.24.11\n"
                        L"Rendering: Direct2D",
                        kWindowTitle, MB_ICONINFORMATION);
                    return 0;
                case IDM_ZOOM_IN:
                    if (view_ && view_->zoom_in()) kick_render(view_->current_page());
                    return 0;
                case IDM_ZOOM_OUT:
                    if (view_ && view_->zoom_out()) kick_render(view_->current_page());
                    return 0;
                case IDM_ZOOM_RESET: {
                    if (view_ && canvas_) {
                        RECT rc; GetClientRect(canvas_->hwnd(), &rc);
                        UINT dpi = GetDpiForWindow(hwnd);
                        view_->set_zoom_mode(
                            litepdf::core::DocumentView::ZoomMode::FitWidth,
                            static_cast<float>(rc.right - rc.left),
                            static_cast<float>(rc.bottom - rc.top),
                            static_cast<float>(dpi));
                        kick_render(view_->current_page());
                    }
                    return 0;
                }
            }
            return 0;
        }
        case WM_USER_OPEN_OK: {
            auto* dv = reinterpret_cast<litepdf::core::DocumentView*>(w);
            int epoch = static_cast<int>(l);
            if (epoch != open_epoch_.load()) {
                // User opened another file after this one; discard stale.
                delete dv;
                return 0;
            }
            ColdStartTimer::mark(2);  // Document fully loaded.
            view_.reset(dv);  // may destroy the old view (joins old workers)
            {
                const auto& path = view_->source_path();
                std::wstring title = L"LitePDF — ";
                title += path.filename().wstring();
                SetWindowTextW(hwnd, title.c_str());
            }
            // First render: tell the canvas which view owns the ui_ctx
            // it will need to fz_drop_pixmap, seed the zoom mode to
            // FitWidth against the current viewport, then kick a render.
            if (canvas_) canvas_->set_view(view_.get());
            view_->set_zoom_mode(
                litepdf::core::DocumentView::ZoomMode::FitWidth,
                0.0f, 0.0f, 96.0f);  // kick_render overrides with real viewport
            kick_render(view_->current_page());
            return 0;
        }
        case WM_USER_OPEN_FAILED: {
            int epoch = static_cast<int>(l);
            if (epoch != open_epoch_.load()) return 0;  // stale

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
            MessageBoxW(hwnd, emsg, kWindowTitle, MB_ICONWARNING);
            return 0;
        }
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, w, l);
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

    // Accelerators: Ctrl+O for Open; Ctrl+= / Ctrl+- / Ctrl+0 for zoom.
    ACCEL accels[] = {
        { FCONTROL | FVIRTKEY, 'O',          IDM_FILE_OPEN  },
        { FCONTROL | FVIRTKEY, VK_OEM_PLUS,  IDM_ZOOM_IN    },  // Ctrl+=
        { FCONTROL | FVIRTKEY, VK_OEM_MINUS, IDM_ZOOM_OUT   },  // Ctrl+-
        { FCONTROL | FVIRTKEY, '0',          IDM_ZOOM_RESET },
    };
    haccel_ = CreateAcceleratorTableW(accels, _countof(accels));

    ShowWindow(hwnd_, nCmdShow);
    ColdStartTimer::mark(1);  // Window visible.
    UpdateWindow(hwnd_);

    if (!initial_path.empty()) {
        open_async(initial_path);
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
