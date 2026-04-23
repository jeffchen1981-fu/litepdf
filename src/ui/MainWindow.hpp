#pragma once
#include <filesystem>
#include <memory>
#include <string>
#include <windows.h>

#include "core/MruList.hpp"
#include "ui/FindBar.hpp"
#include "ui/OutlinePane.hpp"
#include "ui/PdfCanvas.hpp"
#include "ui/TabManager.hpp"

namespace litepdf::app { class ThreadPoolDispatcher; }
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

    // Phase 6 Task 10: find-bar integration.
    void on_find_open();        // IDM_FIND: show or focus find bar
    void on_find_next();        // IDM_FIND_NEXT or Enter / F3
    void on_find_prev();        // IDM_FIND_PREV or Shift+Enter / Shift+F3
    void on_find_close();       // IDM_FIND_CLOSE: hide, clear highlight, refocus canvas
    void on_find_query_changed(const std::wstring& q, bool match_case);
    void update_find_counter();           // read SearchSession -> set_counter
    void update_canvas_hits_source();     // rebind canvas HitsFn to active view
    void on_search_update_posted();       // WM_USER_SEARCH_UPDATE handler

    // Active-tab view accessor -- replaces the old `view_` raw reference.
    // Returns nullptr when no tab is active.
    litepdf::core::DocumentView* active_view();

    // Tab-switch handler registered with TabManager.
    void on_tab_switch(int new_index, int old_index);

    // Tab-close handler registered with TabManager (from middle-click).
    void on_tab_close_request(int index);

    // Rewrite window title based on the active tab (or reset to "LitePDF").
    void update_window_title();

    // WM_COPYDATA handler for single-instance IPC (see app/SingleInstance.hpp).
    LRESULT on_copydata(HWND hwnd, WPARAM w, LPARAM l);

    // Absolutize + resolve the path so MRU entries don't accumulate
    // ".\foo.pdf" / "C:\tmp\foo.pdf" / "foo.pdf" duplicates. On
    // canonicalization failure (permission denied, race, etc.) the
    // input is returned unchanged -- MRU staleness beats data loss.
    static std::filesystem::path canonicalize_for_mru(
        const std::filesystem::path& p);

    HWND    hwnd_   = nullptr;
    HACCEL  haccel_ = nullptr;
    // Phase 6: single process-wide dispatcher shared by every tab's
    // SearchSession. Declared BEFORE tabs_/canvas_ so that on
    // MainWindow destruction it is destroyed AFTER them — each tab's
    // DocumentView holds a ref to this dispatcher via its SearchSession.
    // The dispatcher's dtor drains any queued task so in-flight work
    // finishes before the dispatcher goes away.
    std::unique_ptr<litepdf::app::ThreadPoolDispatcher> search_dispatcher_;
    std::unique_ptr<PdfCanvas>    canvas_;
    std::unique_ptr<TabManager>   tabs_;
    std::unique_ptr<OutlinePane>  outline_;
    // Phase 6 Task 10: floating find bar, child HWND of MainWindow. Created
    // in WM_CREATE, hidden until Ctrl+F. Destroyed by unique_ptr dtor
    // (which calls DestroyWindow on its HWND). Declared AFTER canvas_ so
    // that on destruction the find bar tears down before the canvas —
    // callbacks captured in find_bar_ reference this-> but never reach into
    // canvas_ at teardown (bar is hidden / destroyed first).
    std::unique_ptr<FindBar>      find_bar_;
    std::wstring                  last_find_query_;
    bool                          find_bar_match_case_ = false;
    litepdf::core::MruList        mru_;
    bool                          log_timings_ = false;
};

}  // namespace litepdf::ui
