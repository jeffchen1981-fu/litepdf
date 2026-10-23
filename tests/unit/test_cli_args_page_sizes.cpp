// #55 PR 1 Task 10: litepdf-cli --bench-page-sizes, the page-sizing gate's
// harness (spec section 4.4). Kept apart from test_cli_args.cpp, which is not
// edited (spec section 9.2).

#include <catch2/catch_test_macros.hpp>

#include "cli/cli_args.hpp"

#include <initializer_list>
#include <optional>
#include <string>
#include <vector>

using litepdf::cli::CliOptions;
using litepdf::cli::parse_cli_args;

namespace {

// Parses `litepdf-cli file.pdf <args...>`.
std::optional<std::string> parse(std::initializer_list<const char*> args,
                                 CliOptions& out) {
    std::vector<const char*> argv{"litepdf-cli", "file.pdf"};
    argv.insert(argv.end(), args);
    return parse_cli_args(static_cast<int>(argv.size()), argv.data(), out);
}

constexpr const char* kExcludes =
    "--bench-page-sizes excludes --benchmark, --render, --json, --bench-selection and --page";

} // namespace

TEST_CASE("CliArgs: bench-page-sizes takes iterations", "[cli][args]") {
    CliOptions opt;
    const auto with = parse({"--bench-page-sizes", "--iterations", "3"}, opt);
    REQUIRE_FALSE(with.has_value());
    CHECK(opt.bench_page_sizes);
    CHECK(opt.iterations == 3);

    const auto without = parse({"--bench-page-sizes"}, opt);
    REQUIRE_FALSE(without.has_value());
    CHECK(opt.bench_page_sizes);
    CHECK(opt.iterations == 9);
}

TEST_CASE("CliArgs: bench-page-sizes excludes the other modes", "[cli][args]") {
    CliOptions opt;
    const std::vector<std::vector<const char*>> others{
        {"--benchmark"},
        {"--json"},
        {"--render", "0"},
        {"--bench-selection"},
        {"--page", "1"},
    };
    for (const auto& other : others) {
        CAPTURE(other.front());
        std::vector<const char*> argv{"litepdf-cli", "file.pdf", "--bench-page-sizes"};
        argv.insert(argv.end(), other.begin(), other.end());
        const auto err = parse_cli_args(static_cast<int>(argv.size()), argv.data(), opt);
        REQUIRE(err.has_value());
        CHECK(*err == kExcludes);
    }
}
