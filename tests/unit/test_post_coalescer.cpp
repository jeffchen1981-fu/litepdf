// #135: at most one queued search-update message (ui/detail/PostCoalescer.hpp).
#include <catch2/catch_test_macros.hpp>

#include "ui/detail/PostCoalescer.hpp"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <thread>
#include <vector>

using litepdf::ui::PostCoalescer;

TEST_CASE("PostCoalescer only the first claim posts until the message is handled",
          "[ui][post_coalescer]") {
    PostCoalescer gate;
    REQUIRE(gate.claim());          // nothing queued: post
    REQUIRE_FALSE(gate.claim());    // one already queued: do not post again
    REQUIRE_FALSE(gate.claim());
    gate.release();                 // the handler took it
    REQUIRE(gate.claim());          // the next update posts again
}

TEST_CASE("PostCoalescer a failed post lets the next claim post", "[ui][post_coalescer]") {
    // PostMessageW can fail (window gone, queue full). Nothing is queued then, so
    // the gate must not stay closed -- or no update would ever be posted again.
    PostCoalescer gate;
    REQUIRE(gate.claim());
    gate.post_failed();
    REQUIRE(gate.claim());
}

TEST_CASE("PostCoalescer no update is lost between a release and the next claim",
          "[ui][post_coalescer]") {
    // Producers change shared state, then claim; a claim that wins "posts" a
    // message. The consumer handles each message by releasing FIRST and reading
    // the state after. Once every producer is done and the queue is drained, the
    // last read must have seen every change: an update whose claim lost must be
    // covered by a message handled after it.
    constexpr int kProducers = 4;
    constexpr int kPerProducer = 20000;
    PostCoalescer gate;
    std::atomic<int> changes{0};

    std::mutex m;
    std::condition_variable cv;
    std::size_t queued = 0;
    bool producers_done = false;

    int last_seen = -1;
    std::thread consumer([&] {
        for (;;) {
            {
                std::unique_lock<std::mutex> lk(m);
                cv.wait(lk, [&] { return queued > 0 || producers_done; });
                if (queued == 0) return;   // done and drained
                --queued;
            }
            gate.release();
            last_seen = changes.load(std::memory_order_acquire);
        }
    });

    std::vector<std::thread> producers;
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&] {
            for (int i = 0; i < kPerProducer; ++i) {
                changes.fetch_add(1, std::memory_order_release);
                if (gate.claim()) {
                    std::lock_guard<std::mutex> lk(m);
                    ++queued;
                    cv.notify_one();
                }
            }
        });
    }
    for (auto& t : producers) t.join();
    {
        std::lock_guard<std::mutex> lk(m);
        producers_done = true;
    }
    cv.notify_one();
    consumer.join();

    REQUIRE(last_seen == kProducers * kPerProducer);
}
