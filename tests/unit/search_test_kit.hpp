#pragma once

// Shared helpers for the search dispatcher and SearchSession tests (#140).
// Header-only; included by the test .cpp files that need it.
//
// Lifetimes: declare Gates and Events BEFORE the dispatcher (its drain at
// scope end must not touch destroyed objects) and the GateGuard AFTER it
// (destroyed first, so a failed REQUIRE never leaves a worker blocked on a
// closed gate and the pool's destructor returns).

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace litepdf::test_kit {

// A one-shot latch: wait() blocks until open() has been called.
class Gate {
public:
    void open() {
        {
            std::lock_guard<std::mutex> g(m_);
            open_ = true;
        }
        cv_.notify_all();
    }

    void wait() {
        std::unique_lock<std::mutex> lk(m_);
        cv_.wait(lk, [this] { return open_; });
    }

private:
    std::mutex              m_;
    std::condition_variable cv_;
    bool                    open_ = false;
};

// Opens every gate it holds when destroyed.
class GateGuard {
public:
    template <class... Gates>
    explicit GateGuard(Gates&... gates) : gates_{&gates...} {}
    ~GateGuard() {
        for (Gate* g : gates_) g->open();
    }

    GateGuard(const GateGuard&)            = delete;
    GateGuard& operator=(const GateGuard&) = delete;

private:
    std::vector<Gate*> gates_;
};

// A mutex-guarded, ordered list of event names, recorded from any thread.
class Events {
public:
    void record(std::string name) {
        {
            std::lock_guard<std::mutex> g(m_);
            events_.push_back(std::move(name));
        }
        cv_.notify_all();
    }

    bool started(const std::string& name) const {
        std::lock_guard<std::mutex> g(m_);
        return contains(name);
    }

    // True once `name` has been recorded; false if `timeout` passes first.
    bool wait_for(const std::string& name,
                  std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
        std::unique_lock<std::mutex> lk(m_);
        return cv_.wait_for(lk, timeout, [&] { return contains(name); });
    }

    std::vector<std::string> snapshot() const {
        std::lock_guard<std::mutex> g(m_);
        return events_;
    }

private:
    bool contains(const std::string& name) const {
        return std::find(events_.begin(), events_.end(), name) != events_.end();
    }

    mutable std::mutex       m_;
    std::condition_variable  cv_;
    std::vector<std::string> events_;
};

}  // namespace litepdf::test_kit
