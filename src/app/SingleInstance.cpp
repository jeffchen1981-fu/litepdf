#include "app/SingleInstance.hpp"

namespace {
constexpr wchar_t kMutexName[]  = L"Local\\LitePDF_SingleInstance_v1";
// Must match ui::MainWindow's kWindowClassName.
constexpr wchar_t kMainClass[]  = L"LitePDFMainWindow";
constexpr DWORD   kSendTimeoutMs = 3000;
}  // namespace

namespace litepdf::app {

HANDLE try_acquire_single_instance(bool& already_running) {
    already_running = false;
    HANDLE h = CreateMutexW(nullptr, FALSE, kMutexName);
    if (!h) return nullptr;  // CreateMutex failed -- treat as "fail open"
                             // and let the caller continue as normal instance.
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(h);
        already_running = true;
        return nullptr;
    }
    return h;  // we own it
}

bool forward_to_running_instance(const std::filesystem::path& path) {
    HWND target = FindWindowW(kMainClass, nullptr);
    if (!target) return false;

    COPYDATASTRUCT cds = {};
    std::wstring wpath;
    if (path.empty()) {
        cds.dwData = kIpcBringToFront;
        cds.cbData = 0;
        cds.lpData = nullptr;
    } else {
        wpath = path.wstring();
        cds.dwData = kIpcOpenPath;
        cds.cbData = static_cast<DWORD>(
            (wpath.size() + 1) * sizeof(wchar_t));
        cds.lpData = wpath.data();
    }

    DWORD_PTR result = 0;
    LRESULT ok = SendMessageTimeoutW(
        target, WM_COPYDATA,
        reinterpret_cast<WPARAM>(nullptr),
        reinterpret_cast<LPARAM>(&cds),
        SMTO_ABORTIFHUNG, kSendTimeoutMs, &result);
    return ok != 0;
}

}  // namespace litepdf::app
