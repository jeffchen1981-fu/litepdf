#pragma once

// App-local data directory paths (%LOCALAPPDATA%\LitePDF) for litepdf.exe.

#include <filesystem>

namespace litepdf::app {

// Pure: <local_appdata_root>/LitePDF. Unit-testable, no OS call.
std::filesystem::path compose_app_data_dir(const std::filesystem::path& local_appdata_root);

// Pure sub-path helpers off an already-composed app data dir.
std::filesystem::path session_file_under(const std::filesystem::path& app_data_dir);
std::filesystem::path crashes_dir_under(const std::filesystem::path& app_data_dir);
std::filesystem::path running_marker_under(const std::filesystem::path& app_data_dir);

// OS-backed: resolve FOLDERID_LocalAppData, append "LitePDF", create it (and
// crashes\). Returns an EMPTY path on failure. Callers MUST treat empty as
// "persistence disabled" and never compose against it. NOT unit-tested.
std::filesystem::path app_data_dir();

// Best-effort: keep only the newest `keep` *.dmp files in crashes_dir.
// Silently no-ops on any error. Call once at startup.
void prune_crash_dumps(const std::filesystem::path& crashes_dir, std::size_t keep);

}  // namespace litepdf::app
