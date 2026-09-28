#pragma once

// parse_cli_args -- litepdf-cli's argument parsing, pulled out of main() so a
// unit test can reach it (#100). See tests/unit/test_cli_args.cpp.

#include <optional>
#include <string>

namespace litepdf::cli {

struct CliOptions {
    const char* path = nullptr;
    int  render_page = -1;       // 0-based; -1 = not rendering
    bool benchmark = false;
    int  iterations = 5;         // 25 with --bench-selection unless given
    bool json = false;
    bool bench_selection = false;
    int  selection_page = -1;    // 0-based; -1 = every page
};

// Parses `argv[1]` as the file and `argv[2..]` as options into `out`. Returns
// nothing on success, or the one-line usage error main() prints before exiting
// 2. Needs argc >= 2; `--version` and the no-argument usage are main()'s.
std::optional<std::string> parse_cli_args(int argc, const char* const* argv,
                                          CliOptions& out);

} // namespace litepdf::cli
