#include <catch2/catch_test_macros.hpp>
#include "core/Document.hpp"
#include "core/RenderEngine.hpp"

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
