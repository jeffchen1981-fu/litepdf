#include "ui/password_retry.hpp"

#include <windows.h>  // SecureZeroMemory

namespace litepdf::ui {

namespace {

constexpr int kMaxAttempts = 3;

void wipe(std::string& s) {
    if (!s.empty()) {
        SecureZeroMemory(s.data(), s.size());
    }
}

}  // namespace

PasswordRetryResult try_authenticate_with_retry(PromptCallback   prompt_cb,
                                                AuthCallback     auth_cb,
                                                StatusFormatter  status_for_attempt) {
    PasswordRetryResult result{};
    std::wstring status;  // empty on first prompt

    for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
        auto pw = prompt_cb(status);
        if (!pw.has_value()) {
            // User cancelled. attempts stays at the count before this prompt.
            return result;
        }
        result.attempts = attempt;

        const bool ok = auth_cb(*pw);
        wipe(*pw);
        if (ok) {
            result.accepted = true;
            return result;
        }
        // Failed; format status text for the next prompt iff there is one.
        if (attempt < kMaxAttempts) {
            status = status_for_attempt(attempt);
        }
    }

    // Exhausted attempts without acceptance.
    return result;
}

}  // namespace litepdf::ui
