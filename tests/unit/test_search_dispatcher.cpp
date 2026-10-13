#include "app/SearchDispatcher.hpp"

#include "search_test_kit.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace litepdf::app;
using litepdf::test_kit::Events;
using litepdf::test_kit::Gate;
using litepdf::test_kit::GateGuard;

namespace {

struct MockState {
    std::atomic<std::uint64_t> epoch{0};
    std::vector<int>           pages_processed;
    std::mutex                 m;
};

// Build a task whose run() encodes the epoch check itself. This mirrors
// the real call site (Task 5): the caller snapshots `epoch` at submit
// time and compares it to state->epoch.load() at run time, returning
// early on a stale epoch.
ISearchDispatcher::Task make_task(std::shared_ptr<MockState> s, int page,
                                  std::uint64_t epoch,
                                  std::uint8_t priority = 2) {
    ISearchDispatcher::Task t{};
    t.state_weak = s;
    t.priority   = priority;
    t.run        = [s, page, epoch]() {
        if (s->epoch.load() != epoch) return;  // stale — skip
        std::lock_guard<std::mutex> g(s->m);
        s->pages_processed.push_back(page);
    };
    return t;
}

}  // namespace

TEST_CASE("InlineDispatcher runs tasks synchronously in submission order",
          "[app][dispatcher]") {
    auto s = std::make_shared<MockState>();
    InlineDispatcher d;
    d.submit(make_task(s, 5, 0));
    d.submit(make_task(s, 7, 0));
    std::lock_guard<std::mutex> g(s->m);
    REQUIRE(s->pages_processed == std::vector<int>{5, 7});
}

TEST_CASE("Dispatcher skips task when epoch is stale", "[app][dispatcher]") {
    auto s = std::make_shared<MockState>();
    InlineDispatcher d;
    d.submit(make_task(s, 1, /*epoch=*/0));
    s->epoch.store(1);  // bump — task 2's epoch 0 is now stale
    d.submit(make_task(s, 2, /*epoch=*/0));
    d.submit(make_task(s, 3, /*epoch=*/1));
    std::lock_guard<std::mutex> g(s->m);
    REQUIRE(s->pages_processed == std::vector<int>{1, 3});
}

TEST_CASE("Dispatcher skips task when weak state expired", "[app][dispatcher]") {
    auto s = std::make_shared<MockState>();
    InlineDispatcher d;
    auto t = make_task(s, 9, 0);
    s.reset();  // last strong ref gone; weak expired
    d.submit(std::move(t));
    SUCCEED("did not crash on expired weak");
}

TEST_CASE("ThreadPoolDispatcher processes all tasks and joins on destruction",
          "[app][dispatcher][thread]") {
    auto s = std::make_shared<MockState>();
    {
        ThreadPoolDispatcher pool(2);
        for (int i = 0; i < 50; ++i) pool.submit(make_task(s, i, 0));
        // Destructor drains the queue before workers exit (see worker_loop
        // predicate: return stop_ && queued_ == 0), so all 50 tasks run.
    }
    std::lock_guard<std::mutex> g(s->m);
    REQUIRE(s->pages_processed.size() == 50);
}

TEST_CASE("ThreadPoolDispatcher respects task priority",
          "[app][dispatcher][thread][priority]") {
    auto s = std::make_shared<MockState>();

    // Gate: hold the single worker with a slow first task so subsequent
    // submissions stack in the owner queues before any dequeue.
    std::mutex                 gate_m;
    std::condition_variable    gate_cv;
    bool                       gate_entered = false;
    std::atomic<bool>          gate_release{false};

    ThreadPoolDispatcher pool(1);  // single worker — deterministic ordering

    // The gate task itself. We submit this first and then block submission
    // of the other tasks until the worker is inside the gate's run().
    {
        ISearchDispatcher::Task gate{};
        gate.state_weak = s;
        gate.priority   = 2;  // same P2 as the bulk, doesn't matter — it's
                              // already the only queued item when submitted.
        gate.run = [&, s]() {
            if (s->epoch.load() != 0) return;  // epoch check folded in
            {
                std::lock_guard<std::mutex> gl(gate_m);
                gate_entered = true;
            }
            gate_cv.notify_all();
            while (!gate_release.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            std::lock_guard<std::mutex> g(s->m);
            s->pages_processed.push_back(999);
        };
        pool.submit(std::move(gate));
    }

    // Wait for the worker to actually be inside the gate so any tasks we
    // submit next queue up rather than race with the first dequeue.
    {
        std::unique_lock<std::mutex> lk(gate_m);
        gate_cv.wait(lk, [&] { return gate_entered; });
    }

    // Submit 5 P2 tasks, then 1 P0 task. The P0 should pop first after the
    // gate releases.
    for (int i = 0; i < 5; ++i) {
        pool.submit(make_task(s, i, 0, /*priority=*/2));
    }
    pool.submit(make_task(s, 99, 0, /*priority=*/0));

    // Release the gate.
    gate_release.store(true);

    // Wait for drain.
    for (int i = 0; i < 500; ++i) {
        {
            std::lock_guard<std::mutex> g(s->m);
            if (s->pages_processed.size() >= 7) break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    std::lock_guard<std::mutex> g(s->m);
    REQUIRE(s->pages_processed.size() == 7);
    // Gate ran first (it was alone when dequeued).
    REQUIRE(s->pages_processed[0] == 999);
    // After gate releases, P0 (page 99) must come before any P2 page.
    REQUIRE(s->pages_processed[1] == 99);
}

// ---------------------------------------------------------------------------
// Owner queues (#140): one task in flight per owner, owners take turns,
// discard. Owners are plain shared_ptr<int>s; a task's run captures only
// gates, events and mutexes, never its owner, so the owner's lifetime is the
// test's alone. "Gated and started": the task records its name and then
// blocks on its gate, and the test waits for the name before going on.
// ---------------------------------------------------------------------------

namespace {

using Owner = std::shared_ptr<int>;

Owner make_owner() { return std::make_shared<int>(0); }

ISearchDispatcher::Task owned_task(const Owner& owner, std::function<void()> run,
                                   std::uint8_t priority = 2) {
    ISearchDispatcher::Task t{};
    t.state_weak = owner;
    t.priority   = priority;
    t.run        = std::move(run);
    return t;
}

// Records `name` and returns.
ISearchDispatcher::Task recording(const Owner& owner, Events& ev, std::string name,
                                  std::uint8_t priority = 2) {
    return owned_task(
        owner, [&ev, name = std::move(name)] { ev.record(name); }, priority);
}

// Records `name`, then blocks until `gate` opens.
ISearchDispatcher::Task gated(const Owner& owner, Events& ev, Gate& gate,
                              std::string name, std::uint8_t priority = 2) {
    return owned_task(
        owner,
        [&ev, &gate, name = std::move(name)] {
            ev.record(name);
            gate.wait();
        },
        priority);
}

using Order = std::vector<std::string>;

}  // namespace

TEST_CASE("SearchDispatcher: one owner runs in submission order",
          "[app][dispatcher][thread]") {
    Events ev;
    Gate   gG;
    auto   g = make_owner();
    auto   a = make_owner();
    ThreadPoolDispatcher pool(1);
    GateGuard guard(gG);

    pool.submit(gated(g, ev, gG, "G"));
    REQUIRE(ev.wait_for("G"));
    for (int i = 0; i < 5; ++i) pool.submit(recording(a, ev, "A" + std::to_string(i)));
    gG.open();

    for (int i = 0; i < 5; ++i) REQUIRE(ev.wait_for("A" + std::to_string(i)));
    REQUIRE(ev.snapshot() == Order{"G", "A0", "A1", "A2", "A3", "A4"});
}

TEST_CASE("SearchDispatcher: a lower priority number runs first",
          "[app][dispatcher][thread]") {
    Events ev;
    Gate   gG;
    auto   g = make_owner();
    auto   a = make_owner();
    auto   b = make_owner();
    ThreadPoolDispatcher pool(1);
    GateGuard guard(gG);

    pool.submit(gated(g, ev, gG, "G"));
    REQUIRE(ev.wait_for("G"));
    pool.submit(recording(b, ev, "B0", 2));
    pool.submit(recording(b, ev, "B1", 2));
    pool.submit(recording(a, ev, "A0", 1));
    gG.open();

    REQUIRE(ev.wait_for("A0"));
    REQUIRE(ev.wait_for("B0"));
    REQUIRE(ev.wait_for("B1"));
    const auto order = ev.snapshot();
    REQUIRE(order.size() == 4);
    REQUIRE(order[1] == "A0");
}

TEST_CASE("SearchDispatcher: owners take turns", "[app][dispatcher][thread]") {
    Events ev;
    Gate   gG;
    auto   g = make_owner();
    auto   a = make_owner();
    auto   b = make_owner();
    ThreadPoolDispatcher pool(1);
    GateGuard guard(gG);

    pool.submit(gated(g, ev, gG, "G"));
    REQUIRE(ev.wait_for("G"));
    for (int i = 0; i < 3; ++i) pool.submit(recording(a, ev, "A" + std::to_string(i)));
    for (int i = 0; i < 3; ++i) pool.submit(recording(b, ev, "B" + std::to_string(i)));
    gG.open();

    for (const char* name : {"A0", "A1", "A2", "B0", "B1", "B2"}) {
        REQUIRE(ev.wait_for(name));
    }
    REQUIRE(ev.snapshot() == Order{"G", "A0", "B0", "A1", "B1", "A2", "B2"});
}

TEST_CASE("SearchDispatcher: one task in flight per owner",
          "[app][dispatcher][thread]") {
    Events ev;
    Gate   gA1;
    auto   a = make_owner();
    ThreadPoolDispatcher pool(2);
    GateGuard guard(gA1);

    pool.submit(gated(a, ev, gA1, "A1"));
    REQUIRE(ev.wait_for("A1"));
    pool.submit(recording(a, ev, "A2"));

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    REQUIRE_FALSE(ev.started("A2"));
    gA1.open();
    REQUIRE(ev.wait_for("A2"));
}

TEST_CASE("SearchDispatcher: the second worker takes another owner",
          "[app][dispatcher][thread]") {
    Events     ev;
    Gate       gA1;
    std::mutex mA;  // owner A's per-page lock, like SearchSession's scan_mtx
    auto       a = make_owner();
    auto       b = make_owner();
    ThreadPoolDispatcher pool(2);
    GateGuard guard(gA1);

    pool.submit(owned_task(a, [&] {
        std::lock_guard<std::mutex> lk(mA);
        ev.record("A1");
        gA1.wait();
    }));
    REQUIRE(ev.wait_for("A1"));
    pool.submit(owned_task(a, [&] {
        std::lock_guard<std::mutex> lk(mA);
        ev.record("A2");
    }));
    pool.submit(recording(b, ev, "B1"));

    REQUIRE(ev.wait_for("B1"));
    REQUIRE_FALSE(ev.started("A2"));
}

TEST_CASE("SearchDispatcher: a new owner takes the next free worker",
          "[app][dispatcher][thread]") {
    Events ev;
    Gate   gA1;
    Gate   gB1;
    auto   a = make_owner();
    auto   b = make_owner();
    auto   c = make_owner();
    ThreadPoolDispatcher pool(2);
    GateGuard guard(gA1, gB1);

    pool.submit(gated(a, ev, gA1, "A1"));
    REQUIRE(ev.wait_for("A1"));
    pool.submit(gated(b, ev, gB1, "B1"));
    REQUIRE(ev.wait_for("B1"));
    pool.submit(recording(a, ev, "A2"));
    pool.submit(recording(b, ev, "B2"));
    pool.submit(recording(c, ev, "C1"));
    REQUIRE(ev.snapshot() == Order{"A1", "B1"});

    gA1.open();
    REQUIRE(ev.wait_for("C1"));
    const auto order = ev.snapshot();
    REQUIRE(order.size() >= 3);
    REQUIRE(order[2] == "C1");
}

TEST_CASE("SearchDispatcher: a ready lower-priority owner runs while a higher one is in flight",
          "[app][dispatcher][thread]") {
    Events ev;
    Gate   gA1;
    auto   a = make_owner();
    auto   b = make_owner();
    ThreadPoolDispatcher pool(2);
    GateGuard guard(gA1);

    pool.submit(gated(a, ev, gA1, "A1", 1));
    REQUIRE(ev.wait_for("A1"));
    pool.submit(recording(a, ev, "A2", 1));
    pool.submit(recording(b, ev, "B1", 2));

    REQUIRE(ev.wait_for("B1"));
}

TEST_CASE("SearchDispatcher: a task with no run still releases its owner",
          "[app][dispatcher][thread]") {
    // Without the release the owner stays pinned in flight, A1 never runs,
    // and the pool's destructor waits on it forever: RED ends at ctest's
    // 120 s TIMEOUT after the failed assertion (accepted, spec §6.1).
    Events ev;
    auto   a = make_owner();
    ThreadPoolDispatcher pool(2);

    pool.submit(owned_task(a, nullptr));
    pool.submit(recording(a, ev, "A1"));
    REQUIRE(ev.wait_for("A1"));
}

TEST_CASE("SearchDispatcher: an expired owner's tasks release the owner",
          "[app][dispatcher][thread]") {
    // Without the release the expired owner stays pinned in flight and its
    // second task stays queued. No assertion fails (B0 still runs); the
    // pool's destructor waits forever on that second task, so RED shows as
    // ctest's 120 s TIMEOUT (accepted, spec §6.1).
    Events ev;
    Gate   gG;
    auto   g = make_owner();
    auto   e = make_owner();
    auto   b = make_owner();
    ThreadPoolDispatcher pool(1);
    GateGuard guard(gG);

    pool.submit(gated(g, ev, gG, "G"));
    REQUIRE(ev.wait_for("G"));
    auto e0 = recording(e, ev, "E0");
    auto e1 = recording(e, ev, "E1");
    e.reset();
    REQUIRE(e0.state_weak.expired());
    REQUIRE(e1.state_weak.expired());
    pool.submit(std::move(e0));
    pool.submit(std::move(e1));
    pool.submit(recording(b, ev, "B0"));
    gG.open();

    REQUIRE(ev.wait_for("B0"));
}

TEST_CASE("SearchDispatcher: zero workers still run tasks",
          "[app][dispatcher][thread]") {
    Events ev;
    auto   a = make_owner();
    {
        ThreadPoolDispatcher pool(0);
        pool.submit(recording(a, ev, "A0"));
    }
    REQUIRE(ev.started("A0"));
}

TEST_CASE("SearchDispatcher: discard removes only that owner's queued tasks",
          "[app][dispatcher][thread]") {
    Events ev;
    Gate   gG;
    auto   g = make_owner();
    auto   a = make_owner();
    auto   b = make_owner();
    ThreadPoolDispatcher pool(1);
    GateGuard guard(gG);

    pool.submit(gated(g, ev, gG, "G"));
    REQUIRE(ev.wait_for("G"));
    for (int i = 0; i < 3; ++i) pool.submit(recording(a, ev, "A" + std::to_string(i)));
    for (int i = 0; i < 2; ++i) pool.submit(recording(b, ev, "B" + std::to_string(i)));
    REQUIRE(pool.discard(a) == 3);
    gG.open();

    REQUIRE(ev.wait_for("B0"));
    REQUIRE(ev.wait_for("B1"));
    REQUIRE(ev.snapshot() == Order{"G", "B0", "B1"});
}

TEST_CASE("SearchDispatcher: discard does not count a running task",
          "[app][dispatcher][thread]") {
    // A count one too high would let SearchSession's drain count reach 0
    // under a running task: a use-after-free in ~SearchSession.
    Events ev;
    Gate   gA0;
    auto   a = make_owner();
    ThreadPoolDispatcher pool(2);
    GateGuard guard(gA0);

    pool.submit(owned_task(a, [&] {
        ev.record("A0");
        gA0.wait();
        ev.record("A0 done");
    }));
    REQUIRE(ev.wait_for("A0"));
    pool.submit(recording(a, ev, "A1"));
    pool.submit(recording(a, ev, "A2"));
    REQUIRE(pool.discard(a) == 2);
    gA0.open();
    REQUIRE(ev.wait_for("A0 done"));
}

TEST_CASE("SearchDispatcher: after discard a running owner keeps its turn",
          "[app][dispatcher][thread]") {
    // A's emptied queue is kept while A0 runs, so A2 goes into it and keeps
    // A's last turn. A re-created, never-served queue for A would tie with
    // C's and win on list order (A2 is submitted before C0).
    Events ev;
    Gate   gA0;
    Gate   gB0;
    auto   a = make_owner();
    auto   b = make_owner();
    auto   c = make_owner();
    ThreadPoolDispatcher pool(2);
    GateGuard guard(gA0, gB0);

    pool.submit(gated(a, ev, gA0, "A0"));
    REQUIRE(ev.wait_for("A0"));
    pool.submit(gated(b, ev, gB0, "B0"));
    REQUIRE(ev.wait_for("B0"));
    pool.submit(recording(a, ev, "A1"));
    REQUIRE(pool.discard(a) == 1);
    pool.submit(recording(a, ev, "A2"));
    pool.submit(recording(c, ev, "C0"));

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    REQUIRE_FALSE(ev.started("A2"));
    gA0.open();
    REQUIRE(ev.wait_for("C0"));
    REQUIRE(ev.wait_for("A2"));
    REQUIRE(ev.snapshot() == Order{"A0", "B0", "C0", "A2"});
}
