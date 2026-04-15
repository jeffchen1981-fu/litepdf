#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <stdexcept>
#include <thread>
#include "core/Document.hpp"
#include "core/RenderEngine.hpp"

// Task 2 scaffold ignored doc; Task 3 clones ctx so tests now open the fixture.
TEST_CASE("RenderEngine scaffold constructs and destructs cleanly", "[core][render][lifecycle]") {
    litepdf::core::Document doc;
    REQUIRE(!doc.open("tests/fixtures/simple.pdf").has_value());
    {
        litepdf::core::RenderEngine engine(doc);
        REQUIRE(engine.num_workers() == 2);
        REQUIRE(engine.pending_count() == 0);
    }
    SUCCEED();
}

TEST_CASE("RenderEngine respects custom worker count", "[core][render][lifecycle]") {
    litepdf::core::Document doc;
    REQUIRE(!doc.open("tests/fixtures/simple.pdf").has_value());
    litepdf::core::RenderEngine engine(doc, 4);
    REQUIRE(engine.num_workers() == 4);
}

TEST_CASE("RenderEngine::submit returns a default token (scaffold)", "[core][render][lifecycle]") {
    litepdf::core::Document doc;
    REQUIRE(!doc.open("tests/fixtures/simple.pdf").has_value());
    litepdf::core::RenderEngine engine(doc, 1);
    auto tok = engine.submit({0, 0, 1.0f, nullptr});
    REQUIRE(tok.id == 0);
    REQUIRE(!tok.canceled);
}

TEST_CASE("RenderEngine spawns and joins worker threads cleanly", "[core][render][pool]") {
    litepdf::core::Document doc;
    REQUIRE(!doc.open("tests/fixtures/simple.pdf").has_value());
    auto t0 = std::chrono::steady_clock::now();
    {
        litepdf::core::RenderEngine engine(doc, 2);
        // workers should be idle, not spinning
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    auto elapsed = std::chrono::steady_clock::now() - t0;
    // Ctor + 50ms sleep + dtor should all complete in well under 500ms.
    REQUIRE(elapsed < std::chrono::milliseconds(500));
}

TEST_CASE("RenderEngine with 4 workers starts and joins", "[core][render][pool]") {
    litepdf::core::Document doc;
    REQUIRE(!doc.open("tests/fixtures/simple.pdf").has_value());
    litepdf::core::RenderEngine engine(doc, 4);
    REQUIRE(engine.num_workers() == 4);
}

TEST_CASE("RenderEngine throws when Document is not open", "[core][render][pool]") {
    litepdf::core::Document doc;  // never opened
    REQUIRE_THROWS_AS(litepdf::core::RenderEngine(doc, 2), std::runtime_error);
}
