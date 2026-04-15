#pragma once
#include <memory>
#include <windows.h>

#include "ui/PdfCanvas.hpp"

namespace litepdf::ui {

class MainWindow {
public:
    MainWindow();
    ~MainWindow();

    MainWindow(const MainWindow&)            = delete;
    MainWindow& operator=(const MainWindow&) = delete;

    // Registers window class, creates HWND, runs message pump.
    // Returns WM_QUIT's wParam (0 on clean exit).
    int run(HINSTANCE hInstance, int nCmdShow);

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT handle_message(HWND, UINT, WPARAM, LPARAM);

    HWND hwnd_ = nullptr;
    HACCEL haccel_ = nullptr;
    std::unique_ptr<PdfCanvas> canvas_;
    // Task 5+ will add: std::unique_ptr<litepdf::core::DocumentView> view_;
};

}  // namespace litepdf::ui
