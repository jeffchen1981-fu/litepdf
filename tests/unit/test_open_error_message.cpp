// #184: the open dialog's text for OpenError::UnconvertiblePath names the part
// of the path that has no UTF-8 form -- the file name, a folder, or both.
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

#include "ui/detail/OpenErrorMessage.hpp"

using litepdf::ui::detail::unconvertible_path_message;

namespace {

// An unpaired UTF-16 surrogate: legal in an NTFS name, no UTF-8 form.
const std::wstring kBad(1, static_cast<wchar_t>(0xD800));

bool contains(const std::wstring& haystack, const std::wstring& needle) {
    return haystack.find(needle) != std::wstring::npos;
}

} // namespace

TEST_CASE("OpenErrorMessage names the file when only the file name is bad",
          "[ui][open_error]") {
    const std::wstring msg = unconvertible_path_message(
        std::filesystem::path(L"C:\\docs\\report" + kBad + L".pdf"));
    CHECK(msg == L"The file name contains an invalid Unicode character. "
                 L"Rename the file and try again.");
}

TEST_CASE("OpenErrorMessage names the folder when only a folder name is bad",
          "[ui][open_error]") {
    const std::wstring folder = L"comics" + kBad;
    const std::wstring msg = unconvertible_path_message(
        std::filesystem::path(L"C:\\") / folder / L"good.pdf");
    CHECK(contains(msg, L"\"" + folder + L"\""));
    CHECK(contains(msg, L"folder"));
    CHECK_FALSE(contains(msg, L"file name"));
    CHECK_FALSE(contains(msg, L"Rename the file and"));
}

TEST_CASE("OpenErrorMessage names the first bad folder, not a good one before it",
          "[ui][open_error]") {
    const std::wstring first_bad  = L"inner" + kBad;
    const std::wstring second_bad = L"deeper" + kBad;
    const std::wstring msg = unconvertible_path_message(
        std::filesystem::path(L"C:\\outer") / first_bad / second_bad / L"good.pdf");
    CHECK(contains(msg, L"\"" + first_bad + L"\""));
    CHECK_FALSE(contains(msg, L"\"outer\""));
    CHECK_FALSE(contains(msg, L"\"" + second_bad + L"\""));
}

TEST_CASE("OpenErrorMessage names both when the file and a folder are bad",
          "[ui][open_error]") {
    const std::wstring folder = L"comics" + kBad;
    const std::wstring msg = unconvertible_path_message(
        std::filesystem::path(L"C:\\") / folder / (L"bad" + kBad + L".pdf"));
    CHECK(contains(msg, L"file name"));
    CHECK(contains(msg, L"\"" + folder + L"\""));
}

TEST_CASE("OpenErrorMessage is neutral when the path does not say which part is bad",
          "[ui][open_error]") {
    // Defensive: the worker's lParam-0 posts all carry OpenError::Other, and a
    // path that converts cannot have produced this error. Neither may still
    // claim a culprit.
    for (const auto& p : {std::filesystem::path{},
                          std::filesystem::path(L"C:\\docs\\good.pdf")}) {
        const std::wstring msg = unconvertible_path_message(p);
        CHECK(contains(msg, L"path"));
        CHECK_FALSE(contains(msg, L"\""));
        CHECK_FALSE(contains(msg, L"Rename the file and"));
    }
}
