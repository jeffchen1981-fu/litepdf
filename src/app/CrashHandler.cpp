// src/app/CrashHandler.cpp
#include "app/CrashHandler.hpp"

#include <windows.h>
#include <dbghelp.h>     // MiniDumpWriteDump
#include <strsafe.h>     // StringCchPrintfW (bounded)

#pragma comment(lib, "dbghelp.lib")

namespace litepdf::app {
namespace {

// Prebuilt at install time: "<crashes_dir>\" prefix in a fixed buffer, so the
// filter only appends a short bounded suffix and never formats the directory.
wchar_t g_prefix[1024] = {0};
LONG    g_in_filter = 0;

LONG WINAPI on_unhandled(EXCEPTION_POINTERS* ep) {
    if (InterlockedExchange(&g_in_filter, 1)) return EXCEPTION_CONTINUE_SEARCH;  // re-entrancy
    if (g_prefix[0]) {
        wchar_t name[1024];
        if (SUCCEEDED(StringCchPrintfW(name, ARRAYSIZE(name), L"%slitepdf-%lu-%lu.dmp",
                                       g_prefix, GetCurrentProcessId(), GetTickCount()))) {
            HANDLE f = CreateFileW(name, GENERIC_WRITE, 0, nullptr,
                                   CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (f != INVALID_HANDLE_VALUE) {
                MINIDUMP_EXCEPTION_INFORMATION mei{};
                mei.ThreadId = GetCurrentThreadId();
                mei.ExceptionPointers = ep;
                mei.ClientPointers = FALSE;
                const auto type = (MINIDUMP_TYPE)(MiniDumpNormal |
                                                  MiniDumpWithThreadInfo |
                                                  MiniDumpWithUnloadedModules);
                MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(),
                                  f, type, &mei, nullptr, nullptr);
                CloseHandle(f);
            }
        }
    }
    // NB: best-effort. On heap/stack corruption (esp. EXCEPTION_STACK_OVERFLOW)
    // the dump may fail to write — do NOT add stack-hungry work here.
    return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

void install_crash_handler(const std::filesystem::path& crashes_dir) {
    if (crashes_dir.empty()) return;
    std::wstring p = crashes_dir.wstring();
    if (!p.empty() && p.back() != L'\\') p.push_back(L'\\');
    StringCchCopyW(g_prefix, ARRAYSIZE(g_prefix), p.c_str());  // truncates safely if huge
    SetUnhandledExceptionFilter(on_unhandled);
}

}  // namespace litepdf::app
