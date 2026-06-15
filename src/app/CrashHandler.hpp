// src/app/CrashHandler.hpp
#pragma once
#include <filesystem>

namespace litepdf::app {

// Installs SetUnhandledExceptionFilter. On an unhandled exception, writes a
// minidump to <crashes_dir>\litepdf-<pid>-<ticks>.dmp, then returns
// EXCEPTION_CONTINUE_SEARCH so WER/debuggers still run. Call once, early in
// the primary instance. No-op if crashes_dir is empty.
void install_crash_handler(const std::filesystem::path& crashes_dir);

}  // namespace litepdf::app
