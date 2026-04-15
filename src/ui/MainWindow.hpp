#pragma once
#include <atomic>
#include <filesystem>
#include <memory>
#include <windows.h>

#include "core/DocumentView.hpp"
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

    void open_async(std::filesystem::path path);
    void kick_render(int page);  // recompute zoom, submit render, post to canvas

    HWND    hwnd_   = nullptr;
    HACCEL  haccel_ = nullptr;
    std::unique_ptr<PdfCanvas>                   canvas_;
    std::unique_ptr<litepdf::core::DocumentView> view_;
    std::atomic<int>                             open_epoch_{0};
};

}  // namespace litepdf::ui
