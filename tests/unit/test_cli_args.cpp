#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "cli/cli_args.hpp"

#include <initializer_list>
#include <optional>
#include <string>
#include <vector>

// litepdf-cli's argument parsing (#100). Before it, a value-taking flag with no
// value, and an unknown flag, fell through the parse loop unreported, and
// atoi read garbage as 0: the CLI then ran something other than what was asked
// for and exited 0. The cases that used to parse are marked; the rest were
// already usage errors and are pinned so the move out of main() cannot lose them.

using Catch::Matchers::ContainsSubstring;
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

std::optional<std::string> parse(std::initializer_list<const char*> args) {
    CliOptions ignored;
    return parse(args, ignored);
}

} // namespace

TEST_CASE("CliArgs: a value-taking flag with no value is a usage error",
          "[cli][args]") {
    const auto render = parse({"--render"});   // used to parse
    REQUIRE(render);
    CHECK_THAT(*render, ContainsSubstring("--render"));

    const auto bench = parse({"--benchmark", "--iterations"});   // used to parse
    REQUIRE(bench);
    CHECK_THAT(*bench, ContainsSubstring("--iterations"));

    const auto selection = parse({"--bench-selection", "--iterations"});   // used to parse
    REQUIRE(selection);
    CHECK_THAT(*selection, ContainsSubstring("--iterations"));

    const auto page = parse({"--bench-selection", "--page"});   // already an error (#99)
    REQUIRE(page);
    CHECK_THAT(*page, ContainsSubstring("--page"));
}

TEST_CASE("CliArgs: an unknown argument is a usage error", "[cli][args]") {
    // All three used to parse.
    const auto flag = parse({"--bogus"});
    REQUIRE(flag);
    CHECK_THAT(*flag, ContainsSubstring("--bogus"));

    const auto after_valid = parse({"--benchmark", "--jsn"});
    REQUIRE(after_valid);
    CHECK_THAT(*after_valid, ContainsSubstring("--jsn"));

    const auto second_file = parse({"other.pdf"});
    REQUIRE(second_file);
    CHECK_THAT(*second_file, ContainsSubstring("other.pdf"));
}

TEST_CASE("CliArgs: a value that is not a whole integer is a usage error",
          "[cli][args]") {
    // Most used to parse: atoi read "abc" and "" as 0 and "1x" as 1. Only those
    // that came out as an iteration count or page below 1 were already errors.
    for (const char* bad : {"abc", "1x", "", "2.5", " 1", "+1", "99999999999"}) {
        INFO("value: '" << bad << "'");
        const auto render = parse({"--render", bad});
        REQUIRE(render);
        CHECK_THAT(*render, ContainsSubstring("--render"));

        const auto iterations = parse({"--benchmark", "--iterations", bad});
        REQUIRE(iterations);
        CHECK_THAT(*iterations, ContainsSubstring("--iterations"));

        const auto page = parse({"--bench-selection", "--page", bad});
        REQUIRE(page);
        CHECK_THAT(*page, ContainsSubstring("--page"));
    }
}

TEST_CASE("CliArgs: a flag is never taken as the previous flag's value",
          "[cli][args]") {
    // Used to read "--json" as an iteration count of 0 and report that count,
    // not the flag it had swallowed.
    const auto swallowed = parse({"--benchmark", "--iterations", "--json"});
    REQUIRE(swallowed);
    CHECK_THAT(*swallowed, ContainsSubstring("--iterations"));
    CHECK_THAT(*swallowed, ContainsSubstring("--json"));
}

TEST_CASE("CliArgs: out-of-range values are usage errors", "[cli][args]") {
    CHECK(parse({"--render", "-1"}));   // used to print the summary instead
    CHECK(parse({"--benchmark", "--iterations", "0"}));   // the rest were already errors
    CHECK(parse({"--bench-selection", "--iterations", "-3"}));
    CHECK(parse({"--bench-selection", "--page", "0"}));
}

TEST_CASE("CliArgs: well-formed commands parse", "[cli][args]") {
    CliOptions o;

    REQUIRE_FALSE(parse({}, o));
    CHECK(std::string(o.path) == "file.pdf");
    CHECK(o.render_page == -1);
    CHECK_FALSE(o.benchmark);
    CHECK_FALSE(o.bench_selection);

    REQUIRE_FALSE(parse({"--render", "0"}, o));
    CHECK(o.render_page == 0);

    REQUIRE_FALSE(parse({"--benchmark"}, o));
    CHECK(o.benchmark);
    CHECK(o.iterations == 5);
    CHECK_FALSE(o.json);

    // The form scripts/benchmark.ps1 and the CI benchmark gate use.
    REQUIRE_FALSE(parse({"--benchmark", "--iterations", "7", "--json"}, o));
    CHECK(o.benchmark);
    CHECK(o.iterations == 7);
    CHECK(o.json);

    REQUIRE_FALSE(parse({"--bench-selection"}, o));
    CHECK(o.bench_selection);
    CHECK(o.iterations == 25);
    CHECK(o.selection_page == -1);

    REQUIRE_FALSE(parse({"--bench-selection", "--page", "3", "--iterations", "4"}, o));
    CHECK(o.selection_page == 2);   // 1-based on the command line
    CHECK(o.iterations == 4);
}

TEST_CASE("CliArgs: flag combinations that make no sense stay usage errors",
          "[cli][args]") {
    // All already errors; pinned so the move out of main() cannot lose them.
    CHECK(parse({"--bench-selection", "--json"}));
    CHECK(parse({"--bench-selection", "--benchmark"}));
    CHECK(parse({"--bench-selection", "--render", "0"}));
    CHECK(parse({"--benchmark", "--render", "0"}));
    CHECK(parse({"--iterations", "3"}));
    CHECK(parse({"--json"}));
    CHECK(parse({"--page", "1"}));
}
