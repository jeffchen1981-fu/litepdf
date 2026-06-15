// LitePDF -- Phase 5: bootstrap with single-instance gate.
#include "app/AppPaths.hpp"      // Phase 12: crashes_dir_under / running_marker_under
#include "app/AppRunGuard.hpp"   // Phase 12: abnormal-exit run marker
#include "app/CrashHandler.hpp"  // Phase 12: minidump on unhandled exception
#include "app/SingleInstance.hpp"
#include "ui/ColdStartTimer.hpp"
#include "ui/MainWindow.hpp"

#include <windows.h>
#include <shellapi.h>
#include <filesystem>
#include <optional>
#include <string>

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    litepdf::ui::ColdStartTimer::set_t0();

    // Parse command line. Recognized:
    //   --log-timings       Mirror cold-start line to stderr (Task 14 PS1 grep).
    //   <path>              Optional initial document to open after ShowWindow.
    bool log_timings = false;
    std::filesystem::path initial_path;

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv && i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"--log-timings") {
            log_timings = true;
        } else if (initial_path.empty()) {
            initial_path = a;
        }
    }
    if (argv) LocalFree(argv);

    // Canonicalize once up front so the IPC payload is always absolute
    // (the receiver has no CWD context for the sender) AND the normal
    // initial-path open gets the same normalization for free. Silent
    // fallback on error: a stale / unusual path beats losing the open.
    if (!initial_path.empty()) {
        std::error_code ec;
        auto canon = std::filesystem::weakly_canonical(initial_path, ec);
        if (!ec) initial_path = canon;
    }

    // Single-instance gate: if another litepdf.exe is already running for this
    // user, forward our command-line argument to it and exit.
    bool already = false;
    HANDLE mutex = litepdf::app::try_acquire_single_instance(already);
    if (already) {
        // Another instance is already running — forward our payload and exit.
        // Exit 0 on successful delivery, 1 if forwarding failed (target
        // HWND missing or SendMessageTimeoutW hung); never fall through
        // to becoming a normal instance — see plan D14.
        return litepdf::app::forward_to_running_instance(initial_path) ? 0 : 1;
    }
    // If CreateMutex failed entirely (mutex == nullptr && !already),
    // fall through as a normal instance -- a single stale process is less
    // bad than refusing to run.

    litepdf::ui::MainWindow app;
    app.set_log_timings(log_timings);

    // Phase 12: crash minidumps + abnormal-exit detection. Wired ONLY here in
    // the primary (message-loop-owning) instance — a forwarded second instance
    // already returned above, so it never creates the crash handler or the run
    // marker. Reuse the window's already-resolved app data dir (set in its
    // ctor) so the crash dir, run marker, and session file share one identical
    // path; an empty dir means resolution failed => persistence disabled and
    // NOTHING is created in the process CWD (often the user's documents folder
    // for a shell-association launch). AppRunGuard is constructed AFTER the
    // single-instance gate so only the loop-owning instance owns the marker;
    // it lives until after app.run() returns, outliving the window.
    std::optional<litepdf::app::AppRunGuard> run_guard;
    bool offer_restore = false;
    const std::filesystem::path& data_dir = app.app_data_dir();
    if (!data_dir.empty()) {
        const std::filesystem::path crashes = litepdf::app::crashes_dir_under(data_dir);
        litepdf::app::install_crash_handler(crashes);
        litepdf::app::prune_crash_dumps(crashes, 5);
        run_guard.emplace(litepdf::app::running_marker_under(data_dir));
        offer_restore = run_guard->previous_exit_was_abnormal();
    }
    app.set_run_guard(run_guard ? &*run_guard : nullptr);

    int rc = app.run(hInstance, nCmdShow, initial_path, offer_restore);

    if (mutex) CloseHandle(mutex);
    return rc;
}
