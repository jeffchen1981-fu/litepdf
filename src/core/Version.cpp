// LitePDF -- core::Version (#60).
#include "core/Version.hpp"

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

std::wstring ascii_to_wide(std::string_view ascii) {
    return std::wstring(ascii.begin(), ascii.end());
}

}  // namespace litepdf::core
