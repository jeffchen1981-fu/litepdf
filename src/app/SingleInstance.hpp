#pragma once

// Per-user, single-instance gate for litepdf.exe.
// Uses Local\ namespace so the gate is scoped to the user's login session.
//
// IPC payloads (WM_COPYDATA):
//   dwData = kIpcOpenPath   ('LPDF'): lpData is a null-terminated UTF-16
//                                     absolute path; receiver opens it as a
//                                     new tab.
//   dwData = kIpcBringToFront ('LPDB'): empty payload; receiver restores
//                                       + foregrounds its main window.

#include <filesystem>
#include <windows.h>

namespace litepdf::app {

inline constexpr ULONG_PTR kIpcOpenPath     = 0x4C504446;  // 'LPDF'
inline constexpr ULONG_PTR kIpcBringToFront = 0x4C504442;  // 'LPDB'

// Attempts to acquire the single-instance mutex. Returns:
//   - owned HANDLE (caller must keep it alive for the app's lifetime) when
//     this process is the first instance.
//   - nullptr when another instance already holds the mutex.
// Out-param `already_running` is set true in the second case.
HANDLE try_acquire_single_instance(bool& already_running);

// Locates the running instance's main window by class name and forwards
// the command-line argument. If `path` is empty, sends kIpcBringToFront.
// Returns true on successful delivery.
bool forward_to_running_instance(const std::filesystem::path& path);

}  // namespace litepdf::app
