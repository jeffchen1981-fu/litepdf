// LitePDF — Phase 3: bootstrap that delegates to ui::MainWindow.
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

    litepdf::ui::MainWindow app;
    app.set_log_timings(log_timings);
    return app.run(hInstance, nCmdShow, initial_path);
}
