#include "app/SearchDispatcher.hpp"

#include <cstdio>
#include <utility>

namespace litepdf::app {

namespace {

// A task is runnable only if its owning state is still alive. Epoch
// checking is the caller's responsibility — folded into t.run itself
// (see header). Trivial enough to inline; kept as a named helper for
// readability at the call sites.
bool is_runnable(const ISearchDispatcher::Task& t) {
    return !t.state_weak.expired();
}

}  // namespace

// ---------------------------------------------------------------------------
// InlineDispatcher
// ---------------------------------------------------------------------------
void InlineDispatcher::submit(Task t) {
    // Broad try/catch: even the runnability check or lambda invocation of
    // a user-provided functor could conceivably throw. Swallow everything
    // to keep the caller thread alive — search work should not throw, and
    // this path is primarily for tests.
    try {
        if (!is_runnable(t)) return;
        if (!t.run) return;
        t.run();
    } catch (...) {
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
            stop_ = true;
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
        stop_ = true;
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
            cv_.wait(lk, [this] { return stop_ || !q_.empty(); });
            // Drain-before-exit: only return once BOTH the stop flag is set
            // AND the queue is empty. This guarantees in-flight queued tasks
            // complete before ~ThreadPoolDispatcher() returns.
            if (stop_ && q_.empty()) return;
            // std::priority_queue::top() returns const&, which prevents a
            // plain move. const_cast is safe here because (a) we hold m_
            // exclusively, so no other thread can observe the moved-from
            // element, and (b) the next line pops it unconditionally. This
            // idiom is widely used (e.g. folly, abseil) as the standard
            // workaround for priority_queue's const top().
            task = std::move(const_cast<Task&>(q_.top()));
            q_.pop();
        }
        // Broad try/catch: is_runnable is trivial today, but wrapping it
        // (along with the run() call) defends against future changes or
        // user-provided functors that may throw. A bare throw here would
        // terminate the worker thread and silently shrink the pool.
        try {
            if (!is_runnable(task)) continue;
            if (!task.run) continue;
            task.run();
        } catch (...) {
            std::fprintf(stderr, "SearchDispatcher: worker caught exception\n");
        }
    }
}

}  // namespace litepdf::app
