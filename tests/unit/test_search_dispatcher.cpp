#include "app/SearchDispatcher.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

using namespace litepdf::app;

namespace {

struct MockState {
    std::atomic<std::uint64_t> epoch{0};
    std::vector<int>           pages_processed;
    std::mutex                 m;
};

ISearchDispatcher::Task make_task(std::shared_ptr<MockState> s, int page,
                                  std::uint64_t epoch,
                                  std::uint8_t priority = 2) {
    ISearchDispatcher::Task t{};
    t.state_weak = s;
    t.page       = page;
    t.epoch      = epoch;
    t.priority   = priority;
    t.run        = [s, page]() {
        std::lock_guard<std::mutex> g(s->m);
        s->pages_processed.push_back(page);
    };
    t.is_current_epoch = [s, epoch]() { return s->epoch.load() == epoch; };
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
        // predicate: return stop_ && q_.empty()), so all 50 tasks run.
    }
    std::lock_guard<std::mutex> g(s->m);
    REQUIRE(s->pages_processed.size() == 50);
}

TEST_CASE("ThreadPoolDispatcher respects task priority",
          "[app][dispatcher][thread][priority]") {
    auto s = std::make_shared<MockState>();

    // Gate: hold the single worker with a slow first task so subsequent
    // submissions stack in the priority queue before any dequeue.
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
        gate.page       = 999;
        gate.epoch      = 0;
        gate.priority   = 2;  // same P2 as the bulk, doesn't matter — it's
                              // already the only queued item when submitted.
        gate.run = [&, s]() {
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
        gate.is_current_epoch = [s]() { return s->epoch.load() == 0; };
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
