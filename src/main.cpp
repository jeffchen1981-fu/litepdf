// LitePDF — Phase 3: bootstrap that delegates to ui::MainWindow.
#include "ui/MainWindow.hpp"

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    litepdf::ui::MainWindow app;
    return app.run(hInstance, nCmdShow);
}
