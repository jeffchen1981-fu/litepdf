#include <catch2/catch_test_macros.hpp>
#include "core/Document.hpp"
#include "core/RenderEngine.hpp"

// scaffold ignores the Document; Task 3 tests will use an opened doc when
// workers actually clone context.
TEST_CASE("RenderEngine scaffold constructs and destructs cleanly", "[core][render][lifecycle]") {
    litepdf::core::Document doc;  // unopened — scaffold doesn't use it
    {
        litepdf::core::RenderEngine engine(doc);
        REQUIRE(engine.num_workers() == 2);
        REQUIRE(engine.pending_count() == 0);
    }
    SUCCEED();
}

TEST_CASE("RenderEngine respects custom worker count", "[core][render][lifecycle]") {
    litepdf::core::Document doc;
    litepdf::core::RenderEngine engine(doc, 4);
    REQUIRE(engine.num_workers() == 4);
}

TEST_CASE("RenderEngine::submit returns a default token (scaffold)", "[core][render][lifecycle]") {
    litepdf::core::Document doc;
    litepdf::core::RenderEngine engine(doc, 1);
    auto tok = engine.submit({0, 0, 1.0f, nullptr});
    REQUIRE(tok.id == 0);
    REQUIRE(!tok.canceled);
}
