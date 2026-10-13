#pragma once

// Shared helpers for the search dispatcher and SearchSession tests (#140).
// Header-only; included by the test .cpp files that need it: Gate,
// GateGuard, Events, and the DeferredDispatcher double.
//
// Lifetimes: declare Gates and Events BEFORE the dispatcher (its drain at
// scope end must not touch destroyed objects) and the GateGuard AFTER it
// (destroyed first, so a failed REQUIRE never leaves a worker blocked on a
// closed gate and the pool's destructor returns).

#include "app/SearchDispatcher.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <memory>
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

// Queues every submitted task without running it, so a test decides when
// (and inside which window) each one runs. run() honours the weak-state
// gate the real dispatchers apply. Single-threaded: drive it from the test
// thread only.
//
// A slot is never removed: a task that has run, or was discarded, leaves an
// empty Task{} behind, so run(i) indices stay those of submission order.
class DeferredDispatcher final : public litepdf::app::ISearchDispatcher {
public:
    void submit(Task t) override { tasks_.push_back(std::move(t)); }

    void run(std::size_t i) {
        Task t = std::move(tasks_.at(i));
        tasks_.at(i) = Task{};
        if (!t.state_weak.expired() && t.run) t.run();
    }

    // Runs every task not yet run, including ones submitted meanwhile.
    void run_all() {
        for (std::size_t i = 0; i < tasks_.size(); ++i) run(i);
    }

    // Slots still holding a task: neither run nor discarded.
    std::size_t queued() const {
        return static_cast<std::size_t>(std::count_if(
            tasks_.begin(), tasks_.end(),
            [](const Task& t) { return static_cast<bool>(t.run); }));
    }

    // Empties the owner's queued slots (same control block, as the pool
    // compares owners) and returns how many it emptied.
    std::size_t discard(const std::weak_ptr<void>& owner) override {
        std::size_t n = 0;
        for (Task& t : tasks_) {
            if (t.run && !t.state_weak.owner_before(owner)
                      && !owner.owner_before(t.state_weak)) {
                t = Task{};
                ++n;
            }
        }
        return n;
    }

private:
    std::vector<Task> tasks_;
};

}  // namespace litepdf::test_kit
