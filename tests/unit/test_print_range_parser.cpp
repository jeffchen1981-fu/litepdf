// tests/unit/test_print_range_parser.cpp -- Phase 8.5 Task 2
#include "printing/PrintRange.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace litepdf::printing;

TEST_CASE("single-page range converts 1-based to 0-based", "[print-range]") {
    PageRange r{ /*from*/3, /*to*/3 };
    auto v = parse_page_ranges(&r, 1, /*page_count*/10);
    REQUIRE(v == std::vector<std::size_t>{2});
}

TEST_CASE("contiguous range expands inclusive", "[print-range]") {
    PageRange r{1, 5};
    auto v = parse_page_ranges(&r, 1, 10);
    REQUIRE(v == std::vector<std::size_t>{0, 1, 2, 3, 4});
}

TEST_CASE("multiple disjoint ranges concatenate in input order", "[print-range]") {
    PageRange ranges[] = {
        {1, 3},
        {5, 5},
        {7, 9},
    };
    auto v = parse_page_ranges(ranges, 3, 10);
    REQUIRE(v == std::vector<std::size_t>{0, 1, 2, 4, 6, 7, 8});
}

TEST_CASE("range entries past page_count are clamped", "[print-range]") {
    PageRange r{8, 20};  // doc has 10 pages, expect 7..9
    auto v = parse_page_ranges(&r, 1, 10);
    REQUIRE(v == std::vector<std::size_t>{7, 8, 9});
}

TEST_CASE("inverted range (from > to) is dropped silently", "[print-range]") {
    PageRange ranges[] = {
        {5, 3},   // invalid -- guard
        {1, 2},   // valid: 0,1
    };
    auto v = parse_page_ranges(ranges, 2, 10);
    REQUIRE(v == std::vector<std::size_t>{0, 1});
}
