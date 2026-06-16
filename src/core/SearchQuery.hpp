#pragma once
// Pure, MuPDF-free helpers translating litepdf SearchFlags into the matcher
// needle string and options integer. The int equals fz_search_options
// (static_assert'd in the .cpp).
#include <string>
#include <string_view>
namespace litepdf::core {
int         search_options_value(bool match_case, bool needs_regex);
std::string regex_escape(std::string_view literal);
std::string build_search_needle(std::string_view raw, bool whole_word, bool regex);
}  // namespace litepdf::core
