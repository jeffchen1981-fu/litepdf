#pragma once
#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <windows.h>

#include "app/AppPaths.hpp"      // Phase 12: app_data_dir / session_file_under
#include "app/AppRunGuard.hpp"   // Phase 12: abnormal-exit run marker
#include "app/CrossTabSearch.hpp"
#include "core/MruList.hpp"
#include "core/SessionStore.hpp" // Phase 12: SessionState + save_session
#include "ui/FindBar.hpp"
#include "ui/OutlinePane.hpp"
#include "ui/PdfCanvas.hpp"
#include "ui/ResultsPanel.hpp"
#include "ui/Splitter.hpp"
#include "ui/TabManager.hpp"
#include "ui/VerticalSplitter.hpp"

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
    // If initial_path is non-empty, opens it asynchronously after ShowWindow
    // (deferred until a restore, if any, completes — see Task 9).
    // offer_restore (set by wWinMain from AppRunGuard) drives the
    // prompt-after-abnormal-exit session restore.
    // Returns WM_QUIT's wParam (0 on clean exit).
    int run(HINSTANCE hInstance, int nCmdShow,
            const std::filesystem::path& initial_path = {},
            bool offer_restore = false);

    // When true, PdfCanvas mirrors the cold-start timing line to stderr in
    // addition to OutputDebugStringW. Set before run().
    void set_log_timings(bool on) { log_timings_ = on; }
    bool log_timings() const { return log_timings_; }

    // Phase 12: borrow the wWinMain-owned AppRunGuard (non-owning; it
    // outlives run()). Set before run() so on_clean_exit() can clear the
    // abnormal-exit marker. Left null => marker handling is skipped.
    void set_run_guard(litepdf::app::AppRunGuard* g) { run_guard_ = g; }

    // Phase 12: the resolved %LOCALAPPDATA%\LitePDF dir, or empty when
    // resolution failed (persistence disabled). wWinMain reuses this single
    // resolution for the crash handler + run marker so the crash dir, marker,
    // and session file all share one path. Valid after construction.
    const std::filesystem::path& app_data_dir() const { return app_data_dir_; }

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT handle_message(HWND, UINT, WPARAM, LPARAM);

    // Open a new tab for `path` asynchronously (one request -> one tab).
    void open_tab_async(std::filesystem::path path);
    void kick_render(int page);  // recompute zoom, submit render, post to canvas

    void on_layout();                    // reposition canvas + outline + tab strip
    void toggle_outline();               // F5 handler
    void toggle_thumbs();                // F4 handler (Phase 7 Task 8)
    void on_outline_navigate(int page);  // callback from OutlinePane

    // Phase 7 Task 8: which left-dock pane (if any) is currently
    // visible. Returns the pane's HWND for layout, or nullptr if both
    // outline + thumb pane are hidden. F4/F5 enforce mutual exclusion
    // so at most one is non-null at any time.
    HWND left_pane_hwnd() const;

    // Phase 6 Task 10: find-bar integration.
    void on_find_open();        // IDM_FIND: show or focus find bar
    void on_find_next();        // IDM_FIND_NEXT or Enter / F3
    void on_find_prev();        // IDM_FIND_PREV or Shift+Enter / Shift+F3
    void on_find_close();       // IDM_FIND_CLOSE: hide, clear highlight, refocus canvas
    void on_find_query_changed(const std::wstring& q, bool match_case,
                               bool whole_word, bool regex);
    void update_find_counter();           // read SearchSession -> set_counter
    void update_canvas_hits_source();     // rebind canvas HitsFn to active view
    void on_search_update_posted();       // WM_USER_SEARCH_UPDATE handler

    // Phase 6 Tasks 11-14: cross-tab search + bottom results panel.
    void on_cross_tab_find();                        // IDM_CROSS_TAB_FIND (Ctrl+Shift+F)
    void on_toggle_results();                        // IDM_TOGGLE_RESULTS (F6)
    void on_results_query(const std::wstring& q, bool mc, bool ww,
                          bool rx);                  // ResultsPanel Enter / toggle
    void on_results_row_click(std::size_t hit_idx);  // ResultsPanel row click
    void on_results_close();                         // ResultsPanel x button / cleanup

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
    // Phase 6 Task 11-14: cross-tab query orchestrator. Declared AFTER
    // search_dispatcher_ (it captures per-tab session observers whose
    // aggregation lambdas reach into CrossTabSearch::Impl — which in turn
    // gets modified by dispatch() that submits dispatcher tasks) and
    // BEFORE tabs_ so that cross_tab_ outlives every tab's SearchSession.
    // When a tab closes, its SearchSession destructs and drops the chained
    // observer lambda; cross_tab_ is still alive so the lambda's captures
    // (which include weak_sentinel) are safely torn down.
    std::unique_ptr<litepdf::app::CrossTabSearch> cross_tab_;
    std::unique_ptr<TabManager>   tabs_;
    std::unique_ptr<PdfCanvas>    canvas_;
    std::unique_ptr<OutlinePane>  outline_;
    // Phase 6 Task 10: floating find bar, child HWND of MainWindow. Created
    // in WM_CREATE, hidden until Ctrl+F. Destroyed by unique_ptr dtor
    // (which calls DestroyWindow on its HWND). Declared AFTER canvas_ so
    // that on destruction the find bar tears down before the canvas —
    // callbacks captured in find_bar_ reference this-> but never reach into
    // canvas_ at teardown (bar is hidden / destroyed first).
    std::unique_ptr<FindBar>      find_bar_;
    // Phase 6 Tasks 12-13: bottom-docked results panel + drag splitter.
    // Declared AFTER find_bar_/canvas_/tabs_ so that on destruction these
    // tear down first: their callbacks capture `this` and would otherwise
    // try to reach cross_tab_ / tabs_ via on_results_* helpers.
    std::unique_ptr<ResultsPanel> results_panel_;
    std::unique_ptr<Splitter>     splitter_;
    // Phase 7 Task 8: one VerticalSplitter for the left dock (shared by
    // OutlinePane + per-tab ThumbnailPane). Visible only when one of the
    // two left panes is. set_on_drag updates left_pane_width_px_ and
    // re-invokes on_layout. Declared after Splitter for symmetry; the
    // dtor order doesn't matter (no cross-references).
    std::unique_ptr<VerticalSplitter> v_splitter_;
    // 0 = hidden; first Ctrl+Shift+F / F6 seeds to max(200 px, 1/3 client).
    int                           results_panel_height_px_ = 0;
    // Phase 7 D12: width of the left dock (outline / thumb pane) in
    // device pixels. Single-instance app-wide value — no persistence,
    // no per-tab override. Seeded to ~250 dip on WM_CREATE; updated by
    // VerticalSplitter drag events. on_layout reads this; F4/F5 layout
    // path uses it to position the left pane + the splitter.
    int                           left_pane_width_px_ = 0;
    std::wstring                  last_find_query_;
    // TODO(phase-6.x): preserve match-case across find-bar reopens once
    // FindBar::show_or_focus grows a second arg. For now each reopen
    // inherits whatever checkbox state the bar keeps internally.
    litepdf::core::MruList        mru_;
    bool                          log_timings_ = false;

    // ---- Phase 12: crash-safe session persistence -----------------------
    // Snapshot the live window + tab state into a serializable struct.
    litepdf::core::SessionState capture_session() const;
    // Debounced auto-save (coalesces bursts). No-op when persistence is
    // disabled (empty app_data_dir_) or a restore is in flight.
    void schedule_session_save();
    // Flush a final save + clear the run marker. Shared by WM_DESTROY and
    // WM_ENDSESSION; idempotent. Skips both while restoring_ (Task 9) so a
    // mid-restore exit re-offers the FULL session next launch.
    void on_clean_exit();

    std::filesystem::path      app_data_dir_;          // empty => persistence disabled
    litepdf::app::AppRunGuard* run_guard_   = nullptr; // non-owning; lives in wWinMain
    bool                       restoring_   = false;   // suppresses auto-save mid-restore
    static constexpr UINT_PTR  kSessionSaveTimer = 0xC0DE;

    // ---- Phase 12 Task 9: sequential restore orchestrator ----------------
    // Restore is prompt-after-abnormal-exit only and opens tabs ONE AT A TIME
    // (saved order preserved, completion attributable, encrypted tabs handled).
    // The CLI initial_path is deferred until restore finishes (or is declined)
    // so it never races the restore chain. Every terminal of the in-flight
    // open (success / failure / cancel / exhaustion) funnels back here exactly
    // once so restoring_ can never get stuck true.
    void maybe_offer_restore(bool offer);   // prompt, filter, place window, start
    void restore_open_next();               // issue the next sequential open
    void restore_on_tab_ready(const std::filesystem::path& opened);  // OPEN_OK / password-success
    void restore_on_tab_failed();           // any non-success terminal of the in-flight open
    void restore_finish();                  // resolve active tab by path, re-enable saves
    void open_deferred_initial_path();      // honor the CLI file once, after restore / on decline

    std::vector<litepdf::core::SessionTab> restore_queue_;
    std::size_t                   restore_index_ = 0;
    std::filesystem::path         restore_pending_path_;  // path restore_open_next last issued
    std::filesystem::path         restore_active_path_;   // resolve active tab by PATH, not index
    std::optional<std::filesystem::path> deferred_initial_path_;
    static constexpr std::size_t  kMaxRestoreTabs = 24;   // cap a runaway restore
};

}  // namespace litepdf::ui
