// #60: pure-logic tests for the build-identity classification. The git facts
// are injected, so no repository is involved here; the fact collector itself
// is exercised by the version_script_selftest ctest.
#include "core/Version.hpp"

#include <catch2/catch_test_macros.hpp>

using litepdf::core::BuildFacts;
using litepdf::core::ascii_to_wide;
using litepdf::core::classify;

namespace {

BuildFacts facts(bool toplevel, const char* tag, const char* describe,
                 const char* sha, bool dirty) {
    BuildFacts f;
    f.toplevel_match = toplevel;
    f.exact_tag = tag;
    f.describe = describe;
    f.short_sha = sha;
    f.dirty = dirty;
    return f;
}

}  // namespace

TEST_CASE("Version: exact tag on a clean tree is a release", "[version]") {
    auto id = classify(facts(true, "v1.3.0", "v1.3.0-0-g88513f2", "88513f2", false), "1.3.0");
    REQUIRE(id.is_release);
    REQUIRE(id.display_version == "1.3.0");
    // The tag itself, never the long describe form: at a tag,
    // `git describe --tags --long` prints v1.3.0-0-g88513f2.
    REQUIRE(id.build_id == "v1.3.0");
}

TEST_CASE("Version: exact tag with a dirty tree is dev", "[version]") {
    auto id = classify(facts(true, "v1.3.0", "v1.3.0-0-g88513f2-dirty", "88513f2", true), "1.3.0");
    REQUIRE_FALSE(id.is_release);
    REQUIRE(id.display_version == "1.3.0-dev");
    REQUIRE(id.build_id == "v1.3.0-0-g88513f2-dirty");
}

TEST_CASE("Version: commits past a tag are dev with the describe id", "[version]") {
    auto id = classify(facts(true, "", "v1.3.0-18-gf7b1ed6", "f7b1ed6", false), "1.3.0");
    REQUIRE_FALSE(id.is_release);
    REQUIRE(id.display_version == "1.3.0-dev");
    REQUIRE(id.build_id == "v1.3.0-18-gf7b1ed6");
}

TEST_CASE("Version: no reachable tag falls back to the short sha", "[version]") {
    auto id = classify(facts(true, "", "", "f7b1ed6", false), "1.3.0");
    REQUIRE_FALSE(id.is_release);
    REQUIRE(id.display_version == "1.3.0-dev");
    REQUIRE(id.build_id == "gf7b1ed6");
}

TEST_CASE("Version: no reachable tag and a dirty tree marks the sha dirty", "[version]") {
    auto id = classify(facts(true, "", "", "f7b1ed6", true), "1.3.0");
    REQUIRE(id.build_id == "gf7b1ed6-dirty");
}

TEST_CASE("Version: git giving no answer is dev with an empty id", "[version]") {
    auto id = classify(facts(false, "", "", "", false), "1.3.0");
    REQUIRE_FALSE(id.is_release);
    REQUIRE(id.display_version == "1.3.0-dev");
    REQUIRE(id.build_id.empty());
}

TEST_CASE("Version: facts from a foreign repository are ignored", "[version]") {
    // release.yml's tarball build-back extracts inside the checkout, so git
    // run there answers for the parent repository. toplevel_match is false.
    auto id = classify(facts(false, "v1.3.0", "v1.3.0-0-g88513f2", "88513f2", false), "1.3.0");
    REQUIRE_FALSE(id.is_release);
    REQUIRE(id.display_version == "1.3.0-dev");
    REQUIRE(id.build_id.empty());
}

TEST_CASE("Version: a tag blanked by sanitizing is not a release", "[version]") {
    // A tag such as v1.3.0+meta fails the [A-Za-z0-9._-] rule, so the
    // collector records exact_tag and describe as empty.
    auto id = classify(facts(true, "", "", "88513f2", false), "1.3.0");
    REQUIRE_FALSE(id.is_release);
    REQUIRE(id.build_id == "g88513f2");
}

TEST_CASE("Version: ascii_to_wide widens byte for byte", "[version]") {
    REQUIRE(ascii_to_wide("1.3.0-dev") == L"1.3.0-dev");
    REQUIRE(ascii_to_wide("").empty());
}
