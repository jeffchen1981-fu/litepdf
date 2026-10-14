#pragma once

// app::SearchDispatcher — 2-worker task pool for search work, shared
// across all tabs. See Phase 6 design §4 D5.
//
// Tasks carry a std::weak_ptr<void> (type-erased SearchState from
// core/SearchSession.cpp); workers skip expired weaks (tab closed).
// Epoch-based cancellation is folded into the task's own run lambda:
// the caller snapshots the epoch at submit time and checks it against
// state->epoch at run time, returning early on mismatch.

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace litepdf::app {

class ISearchDispatcher {
public:
    struct Task {
        std::weak_ptr<void>   state_weak;        // type-erased SearchState
        std::uint8_t          priority = 2;      // 0=P0 highest
        std::function<void()> run;               // performs the search;
                                                 // caller encodes epoch check inside

        Task() = default;
        // Move-only: std::function payloads can be heavy (lambdas with
        // captured shared_ptr + state), and the dispatcher pops-then-moves
        // from its queue — copies would be wasteful and error-prone.
        Task(const Task&)            = delete;
        Task& operator=(const Task&) = delete;
        Task(Task&&) noexcept        = default;
        Task& operator=(Task&&) noexcept = default;
    };

    virtual ~ISearchDispatcher() = default;
    // If submit throws, `t` was neither run nor queued and never will be:
    // SearchSession rolls back its drain count on that assumption.
    virtual void submit(Task t) = 0;
    // Removes the owner's queued (not yet popped) tasks without running
    // them and returns how many it removed. A task already popped is
    // unaffected: it runs as usual. The base has no queue and returns 0.
    virtual std::size_t discard(const std::weak_ptr<void>& owner);
};

// Synchronous dispatcher for unit tests. Runs tasks on submit() caller
// thread. Weak check still honored. Does NOT reorder — tasks run in
// submission order. The ordering contract below is ThreadPoolDispatcher's
// (tests that care about priority or turns should use it).
class InlineDispatcher final : public ISearchDispatcher {
public:
    void submit(Task t) override;
};

// Production dispatcher: N worker threads, one FIFO queue per owner (a
// task's state_weak) in each priority lane.
//
// Ordering contract: among queued tasks whose owner is ready (none of its
// tasks running), the lowest priority number runs first. An owner runs one
// task at a time across all lanes, in submission order within a lane.
// Within a lane, the least recently served ready owner goes next. A lower
// lane can therefore starve a higher one while it has ready work.
//
// On destruction the worker_loop predicate drains any remaining queued
// tasks before workers exit (stop_ flag only causes exit once nothing is
// queued). Callers that need to abandon queued work should discard() it,
// or cancel via the per-task epoch / weak_ptr mechanisms, before
// destroying the pool.
class ThreadPoolDispatcher final : public ISearchDispatcher {
public:
    // Clamped to at least one worker: with none, nothing would ever drain.
    explicit ThreadPoolDispatcher(std::size_t num_workers = 2);
    ~ThreadPoolDispatcher();

    ThreadPoolDispatcher(const ThreadPoolDispatcher&) = delete;
    ThreadPoolDispatcher& operator=(const ThreadPoolDispatcher&) = delete;

    void submit(Task t) override;
    std::size_t discard(const std::weak_ptr<void>& owner) override;

private:
    struct OwnerQueue {
        std::weak_ptr<void> owner;            // Task::state_weak of its tasks
        std::deque<Task>    tasks;            // submission order
        std::uint64_t       last_served = 0;  // 0 = never served
    };

    std::mutex                                             m_;
    std::condition_variable                                cv_;
    // Key: priority. No lane is ever empty. A queue is empty only while its
    // owner is in flight: it keeps the owner's last_served until release.
    std::map<std::uint8_t, std::list<OwnerQueue>>          lanes_;
    // Owners with a task popped and not yet released. Reserved to the
    // worker count before any worker starts, so a pop never allocates.
    std::vector<std::weak_ptr<void>>                       in_flight_;
    std::size_t                                            queued_ = 0;
    std::uint64_t                                          serve_seq_ = 0;
    std::vector<std::thread>                               workers_;
    // Guarded by m_ on every read and write; all writers notify cv_ after
    // release. Plain bool (not atomic) is deliberate: mixing atomic stores
    // with condition_variable waits is a well-known lost-wakeup footgun,
    // and making it atomic invites future contributors to skip the lock.
    bool                                                   stop_ = false;

    void worker_loop();
    // is_in_flight and next_ready require m_ held; release takes it.
    bool        is_in_flight(const std::weak_ptr<void>& owner) const;
    OwnerQueue* next_ready();
    void        release(const std::weak_ptr<void>& owner);
};

}  // namespace litepdf::app
