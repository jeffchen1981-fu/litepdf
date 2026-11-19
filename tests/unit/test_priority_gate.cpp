// #229: the priority gate that serialises a Document's users (spec §3.3,
// tests §6.2). Deterministic: every waiter is seen waiting (through
// PriorityGate::waiting) before the next one starts or the holder lets go, and
// no sleep is an oracle. A case whose defect could deadlock runs the blocking
// part through finishes_within on a heap-owned gate.

#include <catch2/catch_test_macros.hpp>

#include "core/detail/PriorityGate.hpp"
#include "gate_test_kit.hpp"
#include "search_test_kit.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

using litepdf::core::detail::admissible;
using litepdf::core::detail::GateClass;
using litepdf::core::detail::GateHold;
using litepdf::core::detail::GateState;
using litepdf::core::detail::PriorityGate;
using litepdf::test_kit::BoundedRun;
using litepdf::test_kit::Events;
using litepdf::test_kit::finishes_within;
using litepdf::test_kit::Gate;
using litepdf::test_kit::GateGuard;
using litepdf::test_kit::JoinAll;
using litepdf::test_kit::poll_until;

namespace {

constexpr auto kDeadline = std::chrono::seconds(5);

constexpr std::size_t idx(GateClass c) { return static_cast<std::size_t>(c); }

// Takes the gate in `cls`, says so, and keeps it until `latch` opens.
std::thread start_holder(PriorityGate& g, GateClass cls, Gate& latch, Events& progress) {
    return std::thread([&g, cls, &latch, &progress] {
        g.acquire(cls);
        progress.record("held");
        latch.wait();
        g.release();
    });
}

// Takes the gate in `cls`, records `tag` in `order` while holding it, releases.
std::thread start_waiter(PriorityGate& g, GateClass cls, std::string tag, Events& order) {
    return std::thread([&g, cls, tag = std::move(tag), &order] {
        g.acquire(cls);
        order.record(tag);
        g.release();
    });
}

// Starts each waiter only after the previous one is seen waiting, with a
// holder in `holder_cls` keeping the gate, then lets the holder go and returns
// the order in which the waiters held the gate.
struct Arrival {
    GateClass   cls;
    std::string tag;
};

std::vector<std::string> run_arrivals(GateClass holder_cls, const std::vector<Arrival>& arrivals) {
    PriorityGate g;
    Events progress;
    Events order;
    Gate latch;
    JoinAll threads;
    GateGuard guard(latch);

    threads.add(start_holder(g, holder_cls, latch, progress));
    REQUIRE(progress.wait_for("held"));

    std::size_t seen[3] = {};
    for (const Arrival& a : arrivals) {
        threads.add(start_waiter(g, a.cls, a.tag, order));
        const std::size_t want = ++seen[idx(a.cls)];
        REQUIRE(poll_until([&] { return g.waiting(a.cls) == want; }));
    }

    latch.open();
    threads.join();
    return order.snapshot();
}

}  // namespace

TEST_CASE("PriorityGate: a foreground waiter goes before waiting render and scan",
          "[core][gate]") {
    const auto order = run_arrivals(GateClass::Foreground, {
        {GateClass::Scan, "S"}, {GateClass::Render, "R"}, {GateClass::Foreground, "F"}});
    CHECK(order == std::vector<std::string>{"F", "R", "S"});
}

TEST_CASE("PriorityGate: a foreground arriving after a waiting scan goes first",
          "[core][gate]") {
    const auto order = run_arrivals(GateClass::Render, {
        {GateClass::Scan, "S"}, {GateClass::Foreground, "F"}});
    CHECK(order == std::vector<std::string>{"F", "S"});
}

TEST_CASE("PriorityGate: render goes before scan", "[core][gate]") {
    const auto order = run_arrivals(GateClass::Foreground, {
        {GateClass::Scan, "S"}, {GateClass::Render, "R"}});
    CHECK(order == std::vector<std::string>{"R", "S"});
}

TEST_CASE("PriorityGate: waiters of one class are served in arrival order", "[core][gate]") {
    SECTION("the admission rule admits only the oldest ticket of a class") {
        // Pure: no threads, so a rule that lets any waiter of a class go fails
        // here whatever order the waiters would wake in.
        GateState s{};
        s.next_ticket[idx(GateClass::Foreground)] = 2;
        s.serving[idx(GateClass::Foreground)]     = 0;
        CHECK(admissible(s, GateClass::Foreground, 0));
        CHECK_FALSE(admissible(s, GateClass::Foreground, 1));

        GateState r{};
        r.next_ticket[idx(GateClass::Render)] = 2;
        r.serving[idx(GateClass::Render)]     = 0;
        CHECK(admissible(r, GateClass::Render, 0));
        CHECK_FALSE(admissible(r, GateClass::Render, 1));
    }

    const auto five = [](GateClass cls, const char* prefix) {
        std::vector<Arrival> a;
        for (int i = 1; i <= 5; ++i) a.push_back({cls, prefix + std::to_string(i)});
        return a;
    };
    const auto tags = [](const std::vector<Arrival>& a) {
        std::vector<std::string> t;
        for (const Arrival& x : a) t.push_back(x.tag);
        return t;
    };

    SECTION("foreground waiters") {
        const auto arrivals = five(GateClass::Foreground, "F");
        for (int round = 0; round < 50; ++round) {
            CAPTURE(round);
            REQUIRE(run_arrivals(GateClass::Foreground, arrivals) == tags(arrivals));
        }
    }

    SECTION("render waiters") {
        const auto arrivals = five(GateClass::Render, "R");
        for (int round = 0; round < 50; ++round) {
            CAPTURE(round);
            REQUIRE(run_arrivals(GateClass::Foreground, arrivals) == tags(arrivals));
        }
    }
}

TEST_CASE("PriorityGate: a waiting scan is admitted after four render holds", "[core][gate]") {
    PriorityGate g;
    Events progress;   // "held", "R<k>-in"
    Events order;      // the tags, in the order their holds ran
    Gate holder_latch;
    Gate r_latch[5];
    JoinAll threads;
    GateGuard guard(holder_latch, r_latch[0], r_latch[1], r_latch[2], r_latch[3], r_latch[4]);

    // R<k> takes the gate, says so, waits for its own latch, then records and
    // lets go.
    const auto start_render = [&](int k) {
        threads.add(std::thread([&g, &progress, &order, &r_latch, k] {
            g.acquire(GateClass::Render);
            progress.record("R" + std::to_string(k) + "-in");
            r_latch[k - 1].wait();
            order.record("R" + std::to_string(k));
            g.release();
        }));
    };

    threads.add(start_holder(g, GateClass::Foreground, holder_latch, progress));
    REQUIRE(progress.wait_for("held"));

    threads.add(start_waiter(g, GateClass::Scan, "S", order));
    REQUIRE(poll_until([&] { return g.waiting(GateClass::Scan) == 1; }));
    start_render(1);
    REQUIRE(poll_until([&] { return g.waiting(GateClass::Render) == 1; }));

    holder_latch.open();
    for (int k = 1; k <= 4; ++k) {
        REQUIRE(progress.wait_for("R" + std::to_string(k) + "-in"));
        // The next render arrives while R<k> holds the gate.
        start_render(k + 1);
        REQUIRE(poll_until([&] { return g.waiting(GateClass::Render) == 1; }));
        r_latch[k - 1].open();
    }
    REQUIRE(progress.wait_for("R5-in"));
    r_latch[4].open();
    threads.join();

    CHECK(order.snapshot() == std::vector<std::string>{"R1", "R2", "R3", "R4", "S", "R5"});
}

TEST_CASE("PriorityGate: re-entry on the owning thread throws", "[core][gate]") {
    // Heap-owned and leaked on a timeout: without the re-entry check the second
    // acquire waits for itself forever.
    auto g = std::make_shared<PriorityGate>();
    auto threw = std::make_shared<std::atomic<bool>>(false);
    const bool returned = finishes_within(kDeadline, [g, threw] {
        g->acquire(GateClass::Foreground);
        try {
            g->acquire(GateClass::Render);
            g->release();   // not reached when the check is in place
        } catch (const std::system_error& e) {
            threw->store(e.code() == std::errc::resource_deadlock_would_occur);
        }
        g->release();
    });
    REQUIRE(returned);
    CHECK(threw->load());
}

TEST_CASE("PriorityGate: a failed re-entry leaves the gate usable", "[core][gate]") {
    auto g = std::make_shared<PriorityGate>();
    auto threw = std::make_shared<std::atomic<bool>>(false);
    REQUIRE(finishes_within(kDeadline, [g, threw] {
        g->acquire(GateClass::Foreground);
        try {
            g->acquire(GateClass::Foreground);
            g->release();
        } catch (const std::system_error&) {
            threw->store(true);
        }
        g->release();
    }));
    REQUIRE(threw->load());

    // A ticket left behind by the failed acquire would count as a Foreground
    // waiter that is never served, and nothing of a lower class would ever be
    // admitted again.
    CHECK(finishes_within(kDeadline, [g] {
        g->acquire(GateClass::Render);
        g->release();
        g->acquire(GateClass::Scan);
        g->release();
    }));
    CHECK(g->waiting(GateClass::Foreground) == 0);
}

TEST_CASE("PriorityGate: a throw under a hold releases it", "[core][gate]") {
    auto g = std::make_shared<PriorityGate>();
    try {
        GateHold h(*g, GateClass::Foreground);
        throw std::runtime_error("x");
    } catch (...) {
    }
    CHECK(finishes_within(kDeadline, [g] { GateHold h(*g, GateClass::Render); }));
}
