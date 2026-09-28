// LitePDF -- core::Version: which build is running (#60).
#pragma once

#include <string>
#include <string_view>

namespace litepdf::core {

// Raw git facts, collected at build time by cmake/CollectBuildFacts.cmake.
// Every string is either empty or matches [A-Za-z0-9._-]+.
struct BuildFacts {
    bool toplevel_match = false;  // git's top-level is this source tree
    std::string_view exact_tag;   // `git describe --tags --exact-match`
    std::string_view describe;    // `git describe --tags --long --dirty`
    std::string_view short_sha;   // `git rev-parse --short HEAD`
    bool dirty = false;           // tracked files modified
};

struct BuildIdentity {
    std::string display_version;  // "1.3.0" (release) or "1.3.0-dev"
    std::string build_id;         // "v1.3.0", "v1.3.0-18-gf7b1ed6", "gf7b1ed6", or ""
    bool is_release = false;
};

// A build is a release only when git proves it: this tree, exactly on a tag,
// clean. Everything else is dev -- a false "dev" costs a title suffix, a false
// "release" hides a dev build (the #60 failure). `version` is the numeric
// triple from VERSION.
BuildIdentity classify(const BuildFacts& facts, std::string_view version);

// This build's identity, from the facts collected when it was built.
// Computed once; safe to call from any thread.
const BuildIdentity& build_identity();

// Widens a string known to be ASCII (every BuildIdentity field is).
std::wstring ascii_to_wide(std::string_view ascii);

}  // namespace litepdf::core
