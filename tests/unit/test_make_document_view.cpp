// #229: both MainWindow construction sites build a tab's DocumentView through
// make_document_view, which gives an FB2 one render worker and everything else
// RenderEngine::kDefaultWorkers.

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>

#include <windows.h>

#include "app/SearchDispatcher.hpp"
#include "core/Document.hpp"
#include "core/DocumentView.hpp"
#include "core/RenderEngine.hpp"
#include "ui/detail/MakeDocumentView.hpp"

using litepdf::app::InlineDispatcher;
using litepdf::core::Document;
using litepdf::core::RenderEngine;
using litepdf::ui::make_document_view;

namespace {

Document open_fixture(const std::filesystem::path& path) {
    Document doc;
    REQUIRE_FALSE(doc.open(path).has_value());
    return doc;
}

// A copy of `source` under a new name in the temp directory, removed on exit.
struct TempCopy {
    std::filesystem::path path;
    TempCopy(const std::filesystem::path& source, const wchar_t* name) {
        static std::atomic<unsigned> seq{0};
        path = std::filesystem::temp_directory_path() /
               (L"litepdf_make_view_" + std::to_wstring(GetCurrentProcessId()) + L"_" +
                std::to_wstring(seq.fetch_add(1)) + L"_" + name);
        std::filesystem::copy_file(source, path,
                                   std::filesystem::copy_options::overwrite_existing);
    }
    ~TempCopy() { std::error_code ec; std::filesystem::remove(path, ec); }
};

}  // namespace

TEST_CASE("MakeDocumentView: an FB2 gets one render worker", "[ui][make_view]") {
    InlineDispatcher disp;
    auto view = make_document_view(open_fixture("tests/fixtures/strip-chapters.fb2"), disp);
    REQUIRE(view);
    CHECK(view->render_workers() == 1);
    CHECK(view->page_count() > 0);
}

TEST_CASE("MakeDocumentView: other formats keep the default worker count", "[ui][make_view]") {
    InlineDispatcher disp;
    for (const char* fixture : {"tests/fixtures/simple.pdf", "tests/fixtures/sample.epub",
                                "tests/fixtures/sample.cbz", "tests/fixtures/sample.svg"}) {
        CAPTURE(fixture);
        auto view = make_document_view(open_fixture(fixture), disp);
        REQUIRE(view);
        CHECK(view->render_workers() == RenderEngine::kDefaultWorkers);
    }
    CHECK(RenderEngine::kDefaultWorkers == 2);
}

// The password-prompt site passes the password through. A PDF named .fb2 opens
// as a PDF (MuPDF scores content above extension) and gets one worker by name:
// slower, never wrong. Its worker must still authenticate (#126).
TEST_CASE("MakeDocumentView: the password reaches the workers of a one-worker view",
          "[ui][make_view]") {
    const TempCopy file{"tests/fixtures/encrypted.pdf", L"encrypted.fb2"};
    Document doc;
    const auto err = doc.open(file.path);
    REQUIRE(err.has_value());
    REQUIRE(*err == Document::OpenError::NeedsPassword);
    REQUIRE(doc.authenticate("test"));

    InlineDispatcher disp;
    auto view = make_document_view(std::move(doc), disp, std::string_view("test"));
    REQUIRE(view);
    CHECK(view->render_workers() == 1);
}
