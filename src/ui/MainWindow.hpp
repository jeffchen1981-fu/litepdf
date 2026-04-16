#pragma once
#include <atomic>
#include <filesystem>
#include <memory>
#include <windows.h>

#include "core/DocumentView.hpp"
#include "ui/OutlinePane.hpp"
#include "ui/PdfCanvas.hpp"

namespace litepdf::ui {

class MainWindow {
public:
    MainWindow();
    ~MainWindow();

    MainWindow(const MainWindow&)            = delete;
    MainWindow& operator=(const MainWindow&) = delete;

    // Registers window class, creates HWND, runs message pump.
    // If initial_path is non-empty, opens it asynchronously after ShowWindow.
    // Returns WM_QUIT's wParam (0 on clean exit).
    int run(HINSTANCE hInstance, int nCmdShow,
            const std::filesystem::path& initial_path = {});

    // When true, PdfCanvas mirrors the cold-start timing line to stderr in
    // addition to OutputDebugStringW. Set before run().
    void set_log_timings(bool on) { log_timings_ = on; }
    bool log_timings() const { return log_timings_; }

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT handle_message(HWND, UINT, WPARAM, LPARAM);

    void open_async(std::filesystem::path path);
    void kick_render(int page);  // recompute zoom, submit render, post to canvas

    void on_layout();                    // reposition canvas + outline for current state
    void toggle_outline();               // F5 handler
    void on_outline_navigate(int page);  // callback from OutlinePane

    HWND    hwnd_   = nullptr;
    HACCEL  haccel_ = nullptr;
    std::unique_ptr<PdfCanvas>                   canvas_;
    std::unique_ptr<litepdf::core::DocumentView> view_;
    std::unique_ptr<OutlinePane>                 outline_;
    std::atomic<int>                             open_epoch_{0};
    bool                                         outline_visible_ = false;
    bool                                         log_timings_     = false;
};

}  // namespace litepdf::ui
