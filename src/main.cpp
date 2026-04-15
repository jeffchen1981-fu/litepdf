// LitePDF — bootstrap entry point.
// Phase 0: opens an empty top-level window. No PDF logic yet.

#include <windows.h>
#include <commctrl.h>

#pragma comment(lib, "comctl32.lib")

namespace {

constexpr wchar_t kWindowClassName[] = L"LitePDFMainWindow";
constexpr wchar_t kWindowTitle[]     = L"LitePDF";

LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

bool RegisterMainClass(HINSTANCE hInstance) {
    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = MainWndProc;
    wc.hInstance     = hInstance;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = kWindowClassName;
    return RegisterClassExW(&wc) != 0;
}

} // namespace

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_STANDARD_CLASSES | ICC_TAB_CLASSES | ICC_TREEVIEW_CLASSES | ICC_LISTVIEW_CLASSES };
    InitCommonControlsEx(&icc);

    if (!RegisterMainClass(hInstance)) {
        MessageBoxW(nullptr, L"Failed to register window class.", kWindowTitle, MB_ICONERROR);
        return 1;
    }

    HWND hwnd = CreateWindowExW(
        0, kWindowClassName, kWindowTitle,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 1024, 768,
        nullptr, nullptr, hInstance, nullptr);
    if (!hwnd) {
        MessageBoxW(nullptr, L"Failed to create main window.", kWindowTitle, MB_ICONERROR);
        return 1;
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}
