#include "app/SearchDispatcher.hpp"

#include <algorithm>
#include <cstdio>
#include <iterator>
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

// Owner identity compares control blocks, so it stays correct after the
// owner expires; the stored weak_ptr keeps the control block (and so its
// address) alive while any of the owner's tasks is queued or running.
// Tasks with an empty state_weak share one owner.
bool same_owner(const std::weak_ptr<void>& a, const std::weak_ptr<void>& b) {
    return !a.owner_before(b) && !b.owner_before(a);
}

}  // namespace

std::size_t ISearchDispatcher::discard(const std::weak_ptr<void>&) {
    return 0;
}

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
    num_workers = std::max<std::size_t>(num_workers, 1);
    // Before any worker starts: a pop's push_back must never allocate.
    in_flight_.reserve(num_workers);
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
    // Strong guarantee (see ISearchDispatcher::submit): everything that
    // allocates happens before anything is linked in, or has the strong
    // guarantee itself; the link is a no-throw splice. No empty lane is
    // ever created.
    {
        std::lock_guard<std::mutex> g(m_);
        const std::uint8_t prio = t.priority;
        const auto lane = lanes_.find(prio);
        OwnerQueue* mine = nullptr;
        if (lane != lanes_.end()) {
            for (auto& q : lane->second) {
                if (same_owner(q.owner, t.state_weak)) {
                    mine = &q;
                    break;
                }
            }
        }
        if (mine) {
            mine->tasks.push_back(std::move(t));
        } else {
            std::list<OwnerQueue> fresh(1);  // never served
            fresh.front().owner = t.state_weak;
            fresh.front().tasks.push_back(std::move(t));
            if (lane == lanes_.end()) {
                lanes_.emplace(prio, std::move(fresh));
            } else {
                lane->second.splice(lane->second.end(), fresh);
            }
        }
        ++queued_;
    }
    cv_.notify_one();
}

std::size_t ThreadPoolDispatcher::discard(const std::weak_ptr<void>& owner) {
    std::lock_guard<std::mutex> g(m_);
    // A running owner's queue is kept, emptied: it carries the owner's
    // last_served, and release erases it.
    const bool running = is_in_flight(owner);
    std::size_t n = 0;
    for (auto lane = lanes_.begin(); lane != lanes_.end();) {
        auto& queues = lane->second;
        for (auto q = queues.begin(); q != queues.end();) {
            if (!same_owner(q->owner, owner)) {
                ++q;
                continue;
            }
            n += q->tasks.size();
            q->tasks.clear();
            q = running ? std::next(q) : queues.erase(q);
        }
        lane = queues.empty() ? lanes_.erase(lane) : std::next(lane);
    }
    queued_ -= n;
    return n;
}

bool ThreadPoolDispatcher::is_in_flight(const std::weak_ptr<void>& owner) const {
    return std::any_of(in_flight_.begin(), in_flight_.end(),
        [&](const std::weak_ptr<void>& f) { return same_owner(f, owner); });
}

ThreadPoolDispatcher::OwnerQueue* ThreadPoolDispatcher::next_ready() {
    // The smallest-key lane with a ready owner; in it, the ready owner
    // served least recently (never-served first; ties by list order).
    for (auto& lane : lanes_) {
        OwnerQueue* best = nullptr;
        for (auto& q : lane.second) {
            if (q.tasks.empty() || is_in_flight(q.owner)) continue;
            if (!best || q.last_served < best->last_served) best = &q;
        }
        if (best) return best;
    }
    return nullptr;
}

void ThreadPoolDispatcher::release(const std::weak_ptr<void>& owner) {
    {
        std::lock_guard<std::mutex> g(m_);
        const auto it = std::find_if(in_flight_.begin(), in_flight_.end(),
            [&](const std::weak_ptr<void>& f) { return same_owner(f, owner); });
        if (it != in_flight_.end()) {
            std::swap(*it, in_flight_.back());
            in_flight_.pop_back();
        }
        // The owner's queues emptied while it ran, and lanes left empty.
        for (auto lane = lanes_.begin(); lane != lanes_.end();) {
            lane->second.remove_if([&](const OwnerQueue& q) {
                return q.tasks.empty() && same_owner(q.owner, owner);
            });
            lane = lane->second.empty() ? lanes_.erase(lane) : std::next(lane);
        }
    }
    // notify_all: the owner may be the one every waiting worker is blocked
    // on, and at shutdown each worker must see queued_ == 0 to exit.
    cv_.notify_all();
}

void ThreadPoolDispatcher::worker_loop() {
    // Takes the popped task's owner out of flight on every exit from an
    // iteration: both continues, a normal return and a throw. Constructed
    // after the pop's lock scope has closed, so its destructor can take m_.
    struct Release {
        ThreadPoolDispatcher* self;
        std::weak_ptr<void>   owner;
        ~Release() { self->release(owner); }
    };

    for (;;) {
        Task task;
        std::weak_ptr<void> owner;
        {
            std::unique_lock<std::mutex> lk(m_);
            OwnerQueue* q = nullptr;
            cv_.wait(lk, [&] {
                return (stop_ && queued_ == 0) || (q = next_ready()) != nullptr;
            });
            // Drain-before-exit: only return once BOTH the stop flag is set
            // AND nothing is queued. This guarantees queued tasks complete
            // before ~ThreadPoolDispatcher() returns.
            if (stop_ && queued_ == 0) return;
            // No-throw from here: the pop, the copy of a weak_ptr, and a
            // push_back within in_flight_'s reserved capacity. An emptied
            // queue is kept: its owner is now in flight.
            task = std::move(q->tasks.front());
            q->tasks.pop_front();
            q->last_served = ++serve_seq_;
            owner = q->owner;
            in_flight_.push_back(owner);
            --queued_;
        }
        const Release release{this, std::move(owner)};
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
