#include "app/SearchDispatcher.hpp"

#include <cstdio>
#include <utility>

namespace litepdf::app {

namespace {

// A task is runnable only if its owning state is still alive AND the
// task's epoch matches the current epoch on that state. Either check
// missing (empty std::function) falls back to "assume still current".
bool is_runnable(const ISearchDispatcher::Task& t) {
    if (t.state_weak.expired()) return false;
    if (t.is_current_epoch && !t.is_current_epoch()) return false;
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// InlineDispatcher
// ---------------------------------------------------------------------------
void InlineDispatcher::submit(Task t) {
    if (!is_runnable(t)) return;
    if (!t.run) return;
    try {
        t.run();
    } catch (...) {
        // Search work should not throw; swallow to keep the caller thread
        // alive. Production uses ThreadPoolDispatcher; this path is tests.
        std::fprintf(stderr, "InlineDispatcher: task threw exception\n");
    }
}

// ---------------------------------------------------------------------------
// ThreadPoolDispatcher
// ---------------------------------------------------------------------------
ThreadPoolDispatcher::ThreadPoolDispatcher(std::size_t num_workers) {
    workers_.reserve(num_workers);
    try {
        for (std::size_t i = 0; i < num_workers; ++i) {
            workers_.emplace_back(&ThreadPoolDispatcher::worker_loop, this);
        }
    } catch (...) {
        // Rollback: signal stop, join whatever we spawned, rethrow.
        {
            std::lock_guard<std::mutex> g(m_);
            stop_.store(true);
        }
        cv_.notify_all();
        for (auto& w : workers_) {
            if (w.joinable()) w.join();
        }
        throw;
    }
}

ThreadPoolDispatcher::~ThreadPoolDispatcher() {
    {
        std::lock_guard<std::mutex> g(m_);
        stop_.store(true);
    }
    cv_.notify_all();
    for (auto& w : workers_) {
        if (w.joinable()) w.join();
    }
}

void ThreadPoolDispatcher::submit(Task t) {
    {
        std::lock_guard<std::mutex> g(m_);
        q_.push(std::move(t));
    }
    cv_.notify_one();
}

void ThreadPoolDispatcher::worker_loop() {
    for (;;) {
        Task task;
        {
            std::unique_lock<std::mutex> lk(m_);
            cv_.wait(lk, [this] { return stop_.load() || !q_.empty(); });
            // Drain-before-exit: only return once BOTH the stop flag is set
            // AND the queue is empty. This guarantees in-flight queued tasks
            // complete before ~ThreadPoolDispatcher() returns.
            if (stop_.load() && q_.empty()) return;
            // std::priority_queue::top() returns const&, so we copy the task
            // out before popping. Task holds two std::function objects and
            // a handful of PODs; the copy cost is a couple of shared_ptr
            // refcount bumps inside the lambdas — acceptable for the
            // clarity win over const_cast gymnastics.
            task = q_.top();
            q_.pop();
        }
        if (!is_runnable(task)) continue;
        if (!task.run) continue;
        try {
            task.run();
        } catch (...) {
            std::fprintf(stderr, "SearchDispatcher: worker caught exception\n");
        }
    }
}

}  // namespace litepdf::app
