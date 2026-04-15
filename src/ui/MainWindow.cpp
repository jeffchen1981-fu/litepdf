// LitePDF — ui::MainWindow: top-level window, menu, message pump.
#include "ui/MainWindow.hpp"
#include "MainMenu.rc.h"

#include "core/Document.hpp"

#include <commctrl.h>
#include <commdlg.h>
#include <stdlib.h>
#include <atomic>
#include <filesystem>
#include <string>
#include <thread>
#include <utility>
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")

namespace {
constexpr wchar_t kWindowClassName[] = L"LitePDFMainWindow";
constexpr wchar_t kWindowTitle[]     = L"LitePDF";

constexpr UINT WM_USER_OPEN_OK     = WM_USER + 1;
constexpr UINT WM_USER_OPEN_FAILED = WM_USER + 2;
// WM_USER + 3 reserved for WM_USER_RENDER_DONE (Task 6).
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

LRESULT MainWindow::handle_message(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
        case WM_CREATE: {
            canvas_ = std::make_unique<PdfCanvas>(
                reinterpret_cast<CREATESTRUCTW*>(l)->hInstance, hwnd);
            return 0;
        }
        case WM_SIZE: {
            if (canvas_)
                SetWindowPos(canvas_->hwnd(), nullptr,
                             0, 0, LOWORD(l), HIWORD(l),
                             SWP_NOZORDER | SWP_NOACTIVATE);
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
            // lets the next paint rebuild at the new DPI.
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
                    MessageBoxW(hwnd, L"LitePDF — Phase 3 Task 1 scaffold",
                                kWindowTitle, MB_ICONINFORMATION);
                    return 0;
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
            view_.reset(dv);  // may destroy the old view (joins old workers)
            {
                const auto& path = view_->source_path();
                std::wstring title = L"LitePDF — ";
                title += path.filename().wstring();
                SetWindowTextW(hwnd, title.c_str());
            }
            // Task 6 will kick off the first render here.
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

int MainWindow::run(HINSTANCE hInstance, int nCmdShow) {
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

    // Accelerators: Ctrl+O for Open. Task 9 adds zoom accels.
    ACCEL accels[] = {
        { FCONTROL | FVIRTKEY, 'O', IDM_FILE_OPEN },
    };
    haccel_ = CreateAcceleratorTableW(accels, _countof(accels));

    ShowWindow(hwnd_, nCmdShow);
    UpdateWindow(hwnd_);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (haccel_ && TranslateAcceleratorW(hwnd_, haccel_, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}

}  // namespace litepdf::ui
