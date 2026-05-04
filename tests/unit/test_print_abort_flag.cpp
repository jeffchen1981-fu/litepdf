// tests/unit/test_print_abort_flag.cpp -- Phase 8.5 Task 3
#include "printing/PrintAbortFlag.hpp"

#include <catch2/catch_test_macros.hpp>
#include <thread>

using namespace litepdf::printing;

TEST_CASE("PrintAbortFlag default is not aborted", "[print-abort]") {
    PrintAbortFlag flag;
    REQUIRE(flag.is_aborted() == false);
}

TEST_CASE("set/read across threads is observable", "[print-abort]") {
    PrintAbortFlag flag;
    std::thread setter([&] { flag.request_abort(); });
    setter.join();
    REQUIRE(flag.is_aborted() == true);
}
