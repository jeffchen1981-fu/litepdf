#pragma once

// ui::try_authenticate_with_retry — pure-logic 3-attempt retry loop.
// Phase 8 D1/D2 wiring lives here so it can be unit-tested without any
// Win32 modal. Production callers (MainWindow::open_tab_async) inject
// PasswordDialog::prompt + Document::authenticate as the dependencies.

#include <functional>
#include <optional>
#include <string>

namespace litepdf::ui {

struct PasswordRetryResult {
    bool accepted = false;
    int  attempts = 0;  // number of prompt invocations that ran (0..3)
};

using PromptCallback   = std::function<std::optional<std::string>(const std::wstring& status)>;
using AuthCallback     = std::function<bool(const std::string& pw)>;
using StatusFormatter  = std::function<std::wstring(int failed_count)>;

// Run up to 3 password prompts.
//
//   prompt_cb           returns std::nullopt to signal Cancel.
//   auth_cb             returns true on successful authentication.
//   status_for_attempt  formats the status text shown next to the input
//                       on the next prompt (input: number of failures
//                       so far, range 1..2 — never 0 and never 3).
//
// The function performs NO Win32 calls. Memory hygiene: the std::string
// returned by prompt_cb is wiped via SecureZeroMemory after auth_cb has
// consumed it on each attempt.
PasswordRetryResult try_authenticate_with_retry(PromptCallback   prompt_cb,
                                                AuthCallback     auth_cb,
                                                StatusFormatter  status_for_attempt);

}  // namespace litepdf::ui
