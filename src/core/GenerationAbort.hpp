#pragma once
// src/core/GenerationAbort.hpp
// Per-generation abort token for SearchSession. Each search generation (one
// set_query) owns a distinct token: 0 = keep scanning, non-zero = abort and
// return whatever Document::page_hits() has collected so far.
//
// begin_generation() flips the PREVIOUS generation's token to non-zero — so a
// worker still inside page_hits() for the superseded query bails mid-page
// instead of scanning a now-stale page to completion — and returns a fresh
// token (value 0) for the new generation. A single shared flag cannot do this:
// the next generation would reset it to 0 and strand the in-flight worker
// (the responsiveness bug this type fixes).
//
// The returned shared_ptr keeps the atomic alive for as long as a worker holds
// it, so a superseded token stays readable across a generation swap. relaxed
// ordering suffices — the token carries no companion state, only its own value,
// and page_hits() polls it each search-loop iteration for eventual visibility.
//
// Thread-safety: mutation (begin_generation / abort_active) is single-threaded
// (SearchSession's UI-thread set_query and destructor). Workers only read their
// own captured token copy, never this object, so no internal locking is needed.

#include <atomic>
#include <memory>

namespace litepdf::core {

class GenerationAbort {
public:
    // Abort the previous generation (if any) and start a fresh one. Returns
    // the new generation's token (value 0); callers hand its raw pointer to
    // Document::page_hits() and keep the shared_ptr alive for the scan.
    std::shared_ptr<std::atomic<int>> begin_generation() {
        if (current_) current_->store(1, std::memory_order_relaxed);
        current_ = std::make_shared<std::atomic<int>>(0);
        return current_;
    }

    // Abort the active generation's token (teardown path). No-op if no
    // generation has started.
    void abort_active() {
        if (current_) current_->store(1, std::memory_order_relaxed);
    }

private:
    std::shared_ptr<std::atomic<int>> current_;
};

}  // namespace litepdf::core
