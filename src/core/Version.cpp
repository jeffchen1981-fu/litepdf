// LitePDF -- core::Version (#60).
#include "core/Version.hpp"

#include "litepdf_build_facts.h"  // generated, see cmake/CollectBuildFacts.cmake

namespace litepdf::core {

BuildIdentity classify(const BuildFacts& facts, std::string_view version) {
    BuildIdentity id;
    if (facts.toplevel_match && !facts.exact_tag.empty() && !facts.dirty) {
        id.display_version = std::string(version);
        id.build_id = std::string(facts.exact_tag);
        id.is_release = true;
        return id;
    }
    id.display_version = std::string(version) + "-dev";
    // Facts about a repository that is not this source tree say nothing about
    // this build, so a foreign repo leaves the id empty.
    if (facts.toplevel_match) {
        if (!facts.describe.empty()) {
            id.build_id = std::string(facts.describe);
        } else if (!facts.short_sha.empty()) {
            id.build_id = "g" + std::string(facts.short_sha) + (facts.dirty ? "-dirty" : "");
        }
    }
    return id;
}

const BuildIdentity& build_identity() {
    static const BuildIdentity id = [] {
        BuildFacts f;
        f.toplevel_match = LITEPDF_FACT_TOPLEVEL_MATCH != 0;
        f.exact_tag = LITEPDF_FACT_EXACT_TAG;
        f.describe = LITEPDF_FACT_DESCRIBE;
        f.short_sha = LITEPDF_FACT_SHORT_SHA;
        f.dirty = LITEPDF_FACT_DIRTY != 0;
        return classify(f, LITEPDF_VERSION_TRIPLE);
    }();
    return id;
}

std::wstring ascii_to_wide(std::string_view ascii) {
    return std::wstring(ascii.begin(), ascii.end());
}

}  // namespace litepdf::core
