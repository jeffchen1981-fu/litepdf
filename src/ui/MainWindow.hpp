#pragma once
#include <filesystem>
#include <memory>
#include <windows.h>

#include "core/MruList.hpp"
#include "ui/OutlinePane.hpp"
#include "ui/PdfCanvas.hpp"
#include "ui/TabManager.hpp"

namespace litepdf::core { class DocumentView; }

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

    // Open a new tab for `path` asynchronously (one request -> one tab).
    void open_tab_async(std::filesystem::path path);
    void kick_render(int page);  // recompute zoom, submit render, post to canvas

    void on_layout();                    // reposition canvas + outline + tab strip
    void toggle_outline();               // F5 handler
    void on_outline_navigate(int page);  // callback from OutlinePane

    // Active-tab view accessor -- replaces the old `view_` raw reference.
    // Returns nullptr when no tab is active.
    litepdf::core::DocumentView* active_view();

    // Tab-switch handler registered with TabManager.
    void on_tab_switch(int new_index, int old_index);

    // Tab-close handler registered with TabManager (from middle-click).
    void on_tab_close_request(int index);

    // Rewrite window title based on the active tab (or reset to "LitePDF").
    void update_window_title();

    HWND    hwnd_   = nullptr;
    HACCEL  haccel_ = nullptr;
    std::unique_ptr<PdfCanvas>    canvas_;
    std::unique_ptr<TabManager>   tabs_;
    std::unique_ptr<OutlinePane>  outline_;
    litepdf::core::MruList        mru_;
    bool                          log_timings_ = false;
};

}  // namespace litepdf::ui
