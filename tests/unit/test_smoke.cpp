#include <catch2/catch_test_macros.hpp>

TEST_CASE("Catch2 smoke: arithmetic works", "[smoke]") {
    REQUIRE(2 + 2 == 4);
}
