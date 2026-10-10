// #119: a PDF whose /Pages /Count MuPDF rejects (negative, or not smaller than
// the xref) opens fine, but every fz_count_pages on it throws. page_count() is
// called from window procedures with no catch, so opening such a file crashed
// the app. The count is now read once while opening; a document whose count
// cannot be read is refused there instead.
#include "core/Document.hpp"

#include <catch2/catch_test_macros.hpp>

#include <windows.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

using namespace litepdf::core;

namespace {

// Copies `fixture` to a temp file with its page tree's "/Count 1 " rewritten
// to `count`, which must be the same length so the xref offsets stay valid and
// MuPDF opens the file without repairing it. Both fixtures keep /Pages outside
// any stream, so the edit also works on the encrypted one (AES covers strings
// and streams, not the dictionary's integers).
std::filesystem::path with_page_count(const char* fixture, std::string_view count) {
    constexpr std::string_view original = "/Count 1 ";
    REQUIRE(count.size() == original.size());
    std::ifstream in(fixture, std::ios::binary);
    REQUIRE(in);
    std::string bytes((std::istreambuf_iterator<char>(in)),
                      std::istreambuf_iterator<char>());
    const auto at = bytes.find(original);
    REQUIRE(at != std::string::npos);
    REQUIRE(bytes.find(original, at + 1) == std::string::npos);
    bytes.replace(at, original.size(), count);

    static std::atomic<unsigned> seq{0};
    const auto path = std::filesystem::temp_directory_path()
        / (L"litepdf_test_bad_count_" + std::to_wstring(GetCurrentProcessId())
           + L"_" + std::to_wstring(seq.fetch_add(1)) + L".pdf");
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    REQUIRE(out);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    REQUIRE(out);
    return path;
}

struct RemoveOnExit {
    std::filesystem::path path;
    ~RemoveOnExit() { std::error_code ec; std::filesystem::remove(path, ec); }
};

} // namespace

TEST_CASE("Document page count: an unreadable count is refused at open as Corrupted",
          "[document][pages]") {
    const RemoveOnExit file{with_page_count("tests/fixtures/simple.pdf", "/Count -1")};

    Document doc;
    const auto err = doc.open(file.path);
    REQUIRE(err.has_value());
    REQUIRE(*err == Document::OpenError::Corrupted);
    REQUIRE_FALSE(doc.is_open());
}

TEST_CASE("Document page count: an unreadable count after authentication closes the document",
          "[document][pages][password]") {
    const RemoveOnExit file{with_page_count("tests/fixtures/encrypted.pdf", "/Count -1")};

    Document doc;
    const auto err = doc.open(file.path);
    REQUIRE(err.has_value());
    REQUIRE(*err == Document::OpenError::NeedsPassword);

    // The password itself is right, so the retry loop must not report it as
    // wrong. The document is closed instead; DocumentView refuses a closed
    // document, which surfaces the "failed after authentication" error.
    REQUIRE(doc.authenticate("test"));
    REQUIRE_FALSE(doc.is_open());
}

TEST_CASE("Document page count: an open document answers without throwing",
          "[document][pages]") {
    Document doc;
    REQUIRE_FALSE(doc.open("tests/fixtures/simple.pdf").has_value());
    std::size_t n = 0;
    REQUIRE_NOTHROW(n = doc.page_count());
    REQUIRE(n == 1);
}

// A /Count larger than the page tree is accepted by fz_count_pages, and MuPDF
// corrects it only when it first builds its page-tree map. Cached before that,
// the count kept a page that does not exist, and going to it threw out of a
// window procedure.
TEST_CASE("Document page count: a count larger than the page tree is corrected at open",
          "[document][pages]") {
    const RemoveOnExit file{with_page_count("tests/fixtures/simple.pdf", "/Count 2 ")};

    Document doc;
    REQUIRE_FALSE(doc.open(file.path).has_value());
    REQUIRE(doc.page_count() == 1);
}
