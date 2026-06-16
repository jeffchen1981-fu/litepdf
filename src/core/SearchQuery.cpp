#include "core/SearchQuery.hpp"
#include <mupdf/fitz.h>
namespace litepdf::core {
int search_options_value(bool match_case, bool needs_regex) {
    static_assert(FZ_SEARCH_EXACT==0 && FZ_SEARCH_IGNORE_CASE==1 && FZ_SEARCH_REGEXP==4,
                  "fz_search_options drifted");
    int v = match_case ? FZ_SEARCH_EXACT : FZ_SEARCH_IGNORE_CASE;
    if (needs_regex) v |= FZ_SEARCH_REGEXP;
    return v;
}
std::string regex_escape(std::string_view s) {
    static const std::string_view meta = "\\^$.|?*+()[]{}";   // first char is a backslash
    std::string out; out.reserve(s.size()+8);
    for (char c : s) { if (meta.find(c)!=std::string_view::npos) out.push_back('\\'); out.push_back(c); }
    return out;
}
std::string build_search_needle(std::string_view raw, bool whole_word, bool regex) {
    if (!whole_word) return std::string(raw);
    std::string body = regex ? ("(?:" + std::string(raw) + ")") : regex_escape(raw);
    return "\\b" + body + "\\b";
}
}  // namespace litepdf::core
