#include "app/AppPaths.hpp"

#include <windows.h>
#include <shlobj.h>   // SHGetKnownFolderPath
#include <algorithm>
#include <system_error>
#include <vector>

#pragma comment(lib, "shell32.lib")

namespace litepdf::app {

std::filesystem::path compose_app_data_dir(const std::filesystem::path& root) {
    return root / L"LitePDF";
}

std::filesystem::path session_file_under(const std::filesystem::path& dir) {
    return dir / L"session.json";
}

std::filesystem::path crashes_dir_under(const std::filesystem::path& dir) {
    return dir / L"crashes";
}

std::filesystem::path running_marker_under(const std::filesystem::path& dir) {
    return dir / L"running.lock";
}

std::filesystem::path app_data_dir() {
    PWSTR raw = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &raw))) {
        if (raw) CoTaskMemFree(raw);
        return {};
    }
    std::filesystem::path base(raw);
    CoTaskMemFree(raw);

    std::filesystem::path dir = compose_app_data_dir(base);
    std::error_code ec;
    std::filesystem::create_directories(crashes_dir_under(dir), ec);  // also creates dir itself
    if (ec) return {};
    return dir;
}

void prune_crash_dumps(const std::filesystem::path& crashes_dir, std::size_t keep) {
    std::error_code ec;
    if (!std::filesystem::is_directory(crashes_dir, ec)) return;
    std::vector<std::filesystem::path> dumps;
    for (auto& e : std::filesystem::directory_iterator(crashes_dir, ec)) {
        if (e.is_regular_file(ec) && e.path().extension() == L".dmp")
            dumps.push_back(e.path());
    }
    if (dumps.size() <= keep) return;
    // Sort oldest-first by last-write time so the newest `keep` survive. A
    // lexical filename sort is wrong: names are litepdf-<pid>-<tick>.dmp, so it
    // is dominated by the PID, and the embedded GetTickCount also resets across
    // reboots — both can keep older dumps over newer ones.
    std::sort(dumps.begin(), dumps.end(),
        [](const std::filesystem::path& a, const std::filesystem::path& b) {
            std::error_code ea, eb;
            const auto ta = std::filesystem::last_write_time(a, ea);
            const auto tb = std::filesystem::last_write_time(b, eb);
            if (ea || eb) return a < b;  // fall back to name if a stat fails
            return ta < tb;              // oldest first
        });
    for (std::size_t i = 0; i + keep < dumps.size(); ++i)
        std::filesystem::remove(dumps[i], ec);
}

}  // namespace litepdf::app
