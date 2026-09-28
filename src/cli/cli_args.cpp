#include "cli/cli_args.hpp"

#include <charconv>
#include <cstring>
#include <system_error>

namespace litepdf::cli {

namespace {

// Reads the value after the flag at argv[i] into `value`, advancing i past it.
// The value must be a whole decimal integer in int range and nothing else: no
// '+', no spaces, no trailing characters. atoi read anything else as a number
// (garbage as 0), and a flag with no value used to be skipped silently (#100).
std::optional<std::string> take_int(int argc, const char* const* argv, int& i,
                                    const char* what, int& value) {
    const std::string flag = argv[i];
    if (i + 1 >= argc) return flag + " needs " + what;
    const char* s = argv[++i];
    const char* end = s + std::strlen(s);
    const auto [ptr, ec] = std::from_chars(s, end, value);
    if (ec != std::errc{} || ptr != end) {
        return flag + " needs " + what + ", got '" + s + "'";
    }
    return std::nullopt;
}

} // namespace

std::optional<std::string> parse_cli_args(int argc, const char* const* argv,
                                          CliOptions& out) {
    out = CliOptions{};
    out.path = argv[1];
    bool iterations_set = false;
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--render") == 0) {
            if (auto err = take_int(argc, argv, i, "a page number", out.render_page)) return err;
            if (out.render_page < 0) return "--render must be >= 0 (pages count from 0)";
        } else if (std::strcmp(argv[i], "--benchmark") == 0) {
            out.benchmark = true;
        } else if (std::strcmp(argv[i], "--iterations") == 0) {
            if (auto err = take_int(argc, argv, i, "a count", out.iterations)) return err;
            if (out.iterations < 1) return "--iterations must be >= 1";
            iterations_set = true;
        } else if (std::strcmp(argv[i], "--json") == 0) {
            out.json = true;
        } else if (std::strcmp(argv[i], "--bench-selection") == 0) {
            out.bench_selection = true;
        } else if (std::strcmp(argv[i], "--page") == 0) {
            // A missing value is an error, not "every page": that would turn a
            // typo into a whole-document run, minutes long on a big file.
            if (auto err = take_int(argc, argv, i, "a page number", out.selection_page)) return err;
            if (out.selection_page < 1) return "--page must be >= 1";
            out.selection_page -= 1;   // 1-based on the command line
        } else {
            return std::string("unknown argument '") + argv[i] + "'";
        }
    }

    if (out.bench_selection) {
        if (out.benchmark || out.render_page >= 0 || out.json) {
            return "--bench-selection excludes --benchmark, --render and --json";
        }
        // Most samples are one sub-millisecond call, so take more than --benchmark's 5.
        if (!iterations_set) out.iterations = 25;
        return std::nullopt;
    }
    if (out.selection_page >= 0) return "--page is only valid with --bench-selection";

    if (out.benchmark && out.render_page >= 0) {
        return "--benchmark and --render are mutually exclusive";
    }
    // --iterations / --json are only meaningful with --benchmark (spec §3.1).
    if (!out.benchmark && (out.json || iterations_set)) {
        return "--iterations/--json are only valid with --benchmark "
               "(--iterations also with --bench-selection)";
    }
    return std::nullopt;
}

} // namespace litepdf::cli
