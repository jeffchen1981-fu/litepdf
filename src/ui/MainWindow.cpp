// LitePDF — ui::MainWindow: top-level window, menu, message pump.
#include "ui/MainWindow.hpp"
#include "MainMenu.rc.h"

#include <commctrl.h>
#include <stdlib.h>
#pragma comment(lib, "comctl32.lib")

namespace {
constexpr wchar_t kWindowClassName[] = L"LitePDFMainWindow";
constexpr wchar_t kWindowTitle[]     = L"LitePDF";
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
        case WM_COMMAND: {
            const int id = LOWORD(w);
            switch (id) {
                case IDM_FILE_OPEN:
                    MessageBoxW(hwnd, L"Open dialog — Task 5",
                                kWindowTitle, MB_ICONINFORMATION);
                    return 0;
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
