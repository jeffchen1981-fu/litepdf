// LitePDF -- Phase 5: bootstrap with single-instance gate.
#include "app/SingleInstance.hpp"
#include "ui/ColdStartTimer.hpp"
#include "ui/MainWindow.hpp"

#include <windows.h>
#include <shellapi.h>
#include <filesystem>
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

    // Single-instance gate: if another litepdf.exe is already running for this
    // user, forward our command-line argument to it and exit.
    bool already = false;
    HANDLE mutex = litepdf::app::try_acquire_single_instance(already);
    if (already) {
        litepdf::app::forward_to_running_instance(initial_path);
        return 0;
    }
    // If CreateMutex failed entirely (mutex == nullptr && !already),
    // fall through as a normal instance -- a single stale process is less
    // bad than refusing to run.

    litepdf::ui::MainWindow app;
    app.set_log_timings(log_timings);
    int rc = app.run(hInstance, nCmdShow, initial_path);

    if (mutex) CloseHandle(mutex);
    return rc;
}
