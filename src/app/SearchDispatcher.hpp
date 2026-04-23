#pragma once

// app::SearchDispatcher — 2-worker task pool for search work, shared
// across all tabs. See Phase 6 design §4 D5.
//
// Tasks carry a std::weak_ptr<void> (type-erased SearchState from
// core/SearchSession.cpp); workers skip expired weaks (tab closed) and
// stale epochs (query superseded).

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace litepdf::app {

class ISearchDispatcher {
public:
    struct Task {
        std::weak_ptr<void>   state_weak;        // type-erased SearchState
        int                   page = 0;
        std::uint64_t         epoch = 0;
        std::uint8_t          priority = 2;      // 0=P0 highest
        std::function<void()> run;               // performs the search
        std::function<bool()> is_current_epoch;  // true if task epoch still current
    };

    virtual ~ISearchDispatcher() = default;
    virtual void submit(Task t) = 0;
};

// Synchronous dispatcher for unit tests. Runs tasks on submit() caller
// thread. Epoch + weak checks still honored. Does NOT ignore priority
// but does NOT reorder — tasks run in submission order (tests that
// care about priority should use ThreadPoolDispatcher).
class InlineDispatcher final : public ISearchDispatcher {
public:
    void submit(Task t) override;
};

// Production dispatcher: N worker threads, priority queue.
// On destruction the worker_loop predicate drains any remaining queued
// tasks before workers exit (stop_ flag only causes exit once q_ is
// empty). Callers that need to abandon queued work should cancel via
// the per-task epoch / weak_ptr mechanisms before destroying the pool.
class ThreadPoolDispatcher final : public ISearchDispatcher {
public:
    explicit ThreadPoolDispatcher(std::size_t num_workers = 2);
    ~ThreadPoolDispatcher();

    ThreadPoolDispatcher(const ThreadPoolDispatcher&) = delete;
    ThreadPoolDispatcher& operator=(const ThreadPoolDispatcher&) = delete;

    void submit(Task t) override;

private:
    struct TaskCmp {
        // Lower priority number = higher urgency. std::priority_queue is a
        // max-heap, so we invert: return true if `a` is LOWER urgency
        // (larger number) so top() yields the HIGHEST urgency (smallest
        // number).
        bool operator()(const Task& a, const Task& b) const {
            return a.priority > b.priority;
        }
    };

    std::mutex                                             m_;
    std::condition_variable                                cv_;
    std::priority_queue<Task, std::vector<Task>, TaskCmp>  q_;
    std::vector<std::thread>                               workers_;
    std::atomic<bool>                                      stop_{false};

    void worker_loop();
};

}  // namespace litepdf::app
