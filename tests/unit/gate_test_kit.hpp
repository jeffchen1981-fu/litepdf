#pragma once

// Helpers for the tests of the Document's priority gate and of the borrowers
// that wait on it (#229). Header-only.
//
// Two rules these helpers exist for:
//   - No sleep is an oracle. A wait for a state is a deadline poll
//     (poll_until), and a call that a gate defect could leave blocked runs
//     through BoundedRun, whose helper thread is detached, so a defect is
//     reported as a failure instead of hanging the test process. Each Catch2
//     case runs in its own process under ctest (catch_discover_tests), so a
//     thread left blocked dies with it.
//   - Catch2 assertions run on the test's main thread only. A helper thread
//     records what it saw into state the main thread reads afterwards.

#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace litepdf::test_kit {

// Runs a function on a detached thread. The thread owns the run's state
// through its own shared_ptr, so the state stays valid however long the
// function blocks, even past the test's own scope.
//
// The function must not touch objects on the caller's stack once wait_for has
// timed out: callers hand it heap-owned objects (a shared_ptr) or objects that
// outlive the process.
class BoundedRun {
public:
    // Starts `fn` and returns at once.
    static std::shared_ptr<BoundedRun> start(std::function<void()> fn) {
        auto run = std::shared_ptr<BoundedRun>(new BoundedRun());
        std::thread([run, f = std::move(fn)]() mutable {
            f();
            {
                std::lock_guard<std::mutex> g(run->m_);
                run->done_ = true;
            }
            run->cv_.notify_all();
        }).detach();
        return run;
    }

    // True once `fn` has returned; false if `limit` passes first, leaving the
    // thread where it is.
    bool wait_for(std::chrono::milliseconds limit) {
        std::unique_lock<std::mutex> lk(m_);
        return cv_.wait_for(lk, limit, [this] { return done_; });
    }

private:
    BoundedRun() = default;

    std::mutex              m_;
    std::condition_variable cv_;
    bool                    done_ = false;
};

// Runs `fn` on a detached thread and BLOCKS until it returns or `limit`
// passes. True if it returned in time. Never wrap a call here that waits for
// something only the caller would do next: use BoundedRun::start, act, then
// wait_for.
inline bool finishes_within(std::chrono::milliseconds limit, std::function<void()> fn) {
    return BoundedRun::start(std::move(fn))->wait_for(limit);
}

// Polls `pred` until it holds or `limit` passes. True if it held.
inline bool poll_until(const std::function<bool()>& pred,
                       std::chrono::milliseconds limit = std::chrono::seconds(5)) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    for (;;) {
        if (pred()) return true;
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::yield();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

// Joins every thread it holds when destroyed. Declare it BEFORE a case's
// GateGuard: the guard then opens the latches first, so the threads can
// finish, and they are joined rather than left joinable (std::terminate) when
// a REQUIRE fails.
class JoinAll {
public:
    JoinAll() = default;
    JoinAll(const JoinAll&)            = delete;
    JoinAll& operator=(const JoinAll&) = delete;

    ~JoinAll() { join(); }

    std::thread& add(std::thread t) {
        threads_.push_back(std::move(t));
        return threads_.back();
    }

    void join() {
        for (std::thread& t : threads_) {
            if (t.joinable()) t.join();
        }
    }

private:
    std::vector<std::thread> threads_;
};

}  // namespace litepdf::test_kit
