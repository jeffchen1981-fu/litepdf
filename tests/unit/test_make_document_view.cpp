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
#include "core/SearchSession.hpp"
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
// slower, never wrong. Since #229 an FB2-named view with no password opens its
// search document at the first search, so a view that builds no longer proves
// the password arrived: a search does. Without the password that open finds a
// document that needs one and fails; with it, the session opened at
// construction and finds the fixture's 8 lines (the render workers take the
// same argument from DocumentView, #126).
TEST_CASE("MakeDocumentView: the password reaches a one-worker view", "[ui][make_view]") {
    const TempCopy file{"tests/fixtures/encrypted-text.pdf", L"encrypted.fb2"};
    Document doc;
    const auto err = doc.open(file.path);
    REQUIRE(err.has_value());
    REQUIRE(*err == Document::OpenError::NeedsPassword);
    REQUIRE(doc.authenticate("test"));

    InlineDispatcher disp;
    auto view = make_document_view(std::move(doc), disp, std::string_view("test"));
    REQUIRE(view);
    CHECK(view->render_workers() == 1);
    view->search().set_query(L"ENCRYPTED", litepdf::core::SearchSession::Flags{});
    CHECK_FALSE(view->search().open_failed());
    CHECK(view->search().hit_count() == 8);
}

// #229: an FB2 view opens its search document at the first search, so a tab
// that is never searched parses the book once less. FB2 lets go of the file at
// open, so a copy moved away before the first search shows it: the search
// finds the file gone.
TEST_CASE("MakeDocumentView: an FB2 view opens its search document at the first search",
          "[ui][make_view][lazy]") {
    const TempCopy file{"tests/fixtures/strip-chapters.fb2", L"lazy.fb2"};
    InlineDispatcher disp;
    auto view = make_document_view(open_fixture(file.path), disp);
    REQUIRE(view);

    const auto moved = std::filesystem::path(file.path).concat(L".moved");
    std::filesystem::rename(file.path, moved);
    view->search().set_query(L"Lorem", litepdf::core::SearchSession::Flags{});
    CHECK(view->search().open_failed());
    CHECK(view->search().hit_count() == 0);

    std::filesystem::rename(moved, file.path);
    view->search().set_query(L"Lorem", litepdf::core::SearchSession::Flags{});
    CHECK_FALSE(view->search().open_failed());
    CHECK(view->search().hit_count() > 0);
}

// Every other format keeps its search document from the start. SVG, like FB2,
// reads the whole file at open and lets go of it, so it is the one that can
// show the difference: moved away after the view is built, its search still
// has its document and does not fail.
TEST_CASE("MakeDocumentView: other formats open their search document with the view",
          "[ui][make_view][lazy]") {
    const TempCopy file{"tests/fixtures/sample.svg", L"eager.svg"};
    InlineDispatcher disp;
    auto view = make_document_view(open_fixture(file.path), disp);
    REQUIRE(view);

    const auto moved = std::filesystem::path(file.path).concat(L".moved");
    std::filesystem::rename(file.path, moved);
    view->search().set_query(L"zzzz", litepdf::core::SearchSession::Flags{});
    CHECK_FALSE(view->search().open_failed());
    CHECK(view->search().scan_complete());
    std::filesystem::rename(moved, file.path);
}
