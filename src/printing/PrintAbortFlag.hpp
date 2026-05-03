#pragma once
// src/printing/PrintAbortFlag.hpp -- Phase 8.5 Task 3
// Shared cancel signal between the PrintProgressDlg's Cancel button
// (writer) and SetAbortProc / page loop (reader). Atomic for safety;
// single-threaded usage today, but cheap insurance and future-proof
// for an eventual background-printing variant.

#include <atomic>

namespace litepdf::printing {

class PrintAbortFlag {
public:
    PrintAbortFlag() = default;

    void request_abort() noexcept { aborted_.store(true, std::memory_order_release); }
    [[nodiscard]] bool is_aborted() const noexcept {
        return aborted_.load(std::memory_order_acquire);
    }

private:
    std::atomic<bool> aborted_{false};
};

} // namespace litepdf::printing
