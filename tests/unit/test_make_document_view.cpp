// #229: both MainWindow construction sites build a tab's DocumentView through
// make_document_view. A document MuPDF opened as FB2, with no password, is
// shared: its render worker and search session borrow the Document's own
// document, so the tab keeps one copy. A shared view and anything named .fb2
// get one render worker; everything else gets RenderEngine::kDefaultWorkers.

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>

#include <windows.h>

#include "app/SearchDispatcher.hpp"
#include "core/Document.hpp"
#include "core/DocumentView.hpp"
#include "core/RenderEngine.hpp"
#include "core/SearchSession.hpp"
#include "core/detail/PriorityGate.hpp"
#include "core/detail/SharedDocumentAccess.hpp"
#include "gate_test_kit.hpp"
#include "search_test_kit.hpp"
#include "ui/detail/MakeDocumentView.hpp"

#include <mupdf/fitz.h>

using litepdf::app::InlineDispatcher;
using litepdf::app::ISearchDispatcher;
using litepdf::app::ThreadPoolDispatcher;
using litepdf::core::detail::GateClass;
using litepdf::core::detail::SharedDocumentAccess;
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
// slower, never wrong. It is not FB2 and has a password, so it keeps documents
// of its own (#229): the render worker and the search session each open and
// authenticate one with the password, and the search finds the fixture's 8
// lines.
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
    CHECK(view->render_documents_opened() == 1);
    CHECK(view->search().documents_opened() == 1);
    view->search().set_query(L"ENCRYPTED", litepdf::core::SearchSession::Flags{});
    CHECK(view->search().hit_count() == 8);
}

// S1: an FB2 view's engine and session open no document; a PDF view's open one
// per worker and one.
TEST_CASE("MakeDocumentView: an FB2 view opens no document of its own", "[ui][make_view]") {
    InlineDispatcher disp;
    {
        auto view = make_document_view(open_fixture("tests/fixtures/strip-chapters.fb2"), disp);
        REQUIRE(view);
        CHECK(view->render_documents_opened() == 0);
        CHECK(view->search().documents_opened() == 0);
        CHECK(view->render_workers() == 1);
    }
    {
        auto view = make_document_view(open_fixture("tests/fixtures/simple.pdf"), disp);
        REQUIRE(view);
        CHECK(view->render_documents_opened() == 2);
        CHECK(view->search().documents_opened() == 1);
    }
}

// S2: the format predicate, not the extension, decides sharing. An unencrypted
// PDF named .fb2 has no password to make it Own, so only the predicate keeps it
// from sharing: it keeps documents of its own, and one worker by name (#235).
TEST_CASE("MakeDocumentView: an unencrypted PDF named .fb2 keeps its own documents",
          "[ui][make_view]") {
    const TempCopy file{"tests/fixtures/simple.pdf", L"plain.fb2"};
    InlineDispatcher disp;
    auto view = make_document_view(open_fixture(file.path), disp);
    REQUIRE(view);
    CHECK(view->render_documents_opened() == 1);
    CHECK(view->search().documents_opened() == 1);
    CHECK(view->render_workers() == 1);
}

// S3: .svg has no magic check and MuPDF scores content above the extension, so
// FB2 content named .svg opens as FB2, is shared, and gets one worker.
TEST_CASE("MakeDocumentView: FB2 content named .svg is shared with one worker",
          "[ui][make_view]") {
    const TempCopy file{"tests/fixtures/strip-chapters.fb2", L"book.svg"};
    InlineDispatcher disp;
    auto view = make_document_view(open_fixture(file.path), disp);
    REQUIRE(view);
    CHECK(view->render_documents_opened() == 0);
    CHECK(view->search().documents_opened() == 0);
    CHECK(view->render_workers() == 1);
}

// Behaviour, not a sharing oracle (spec §6.1): an FB2 lets go of its file at
// open, so a view whose search borrows the tab's document finds every hit after
// the file is moved away. A session that opened its own copy with the view
// would pass too; only the counters above prove sharing.
TEST_CASE("MakeDocumentView: an FB2 view searches after its file is moved away",
          "[ui][make_view]") {
    const TempCopy file{"tests/fixtures/strip-chapters.fb2", L"moved.fb2"};
    std::size_t expected = 0;
    {
        const Document probe = open_fixture(file.path);
        for (std::size_t p = 0; p < probe.page_count(); ++p) {
            expected += probe.page_hits(p, "Lorem", Document::SearchFlags{}).size();
        }
    }
    REQUIRE(expected > 0);

    InlineDispatcher disp;
    auto view = make_document_view(open_fixture(file.path), disp);
    REQUIRE(view);
    const auto moved = std::filesystem::path(file.path).concat(L".moved");
    std::filesystem::rename(file.path, moved);
    view->search().set_query(L"Lorem", litepdf::core::SearchSession::Flags{});
    CHECK(view->search().scan_complete());
    CHECK(view->search().hit_count() == expected);
    std::error_code ec;
    std::filesystem::remove(moved, ec);
}

namespace {

// Forwards to a two-thread pool, and says when discard is first called:
// ~SearchSession calls it before its drain, so the flag tells the test that
// the view's destruction has begun.
class FlaggingDispatcher final : public ISearchDispatcher {
public:
    void submit(Task t) override { pool_.submit(std::move(t)); }
    std::size_t discard(const std::weak_ptr<void>& owner) override {
        discarded_.store(true);
        return pool_.discard(owner);
    }
    bool discarded() const { return discarded_.load(); }

private:
    ThreadPoolDispatcher pool_{2};
    std::atomic<bool>    discarded_{false};
};

}  // namespace

// Review Focus 1: closing an FB2 tab while its search scans and its worker
// renders. Both are queued on the gate a helper thread holds when destruction
// begins; ~SearchSession spins on the scan task and ~RenderEngine joins the
// worker, so the view must come down promptly once the hold ends.
TEST_CASE("MakeDocumentView: closing an FB2 view mid-search and mid-render returns",
          "[ui][make_view]") {
    // Heap-owned and leaked on a timeout: a destructor that never returns
    // keeps them in use on its detached thread.
    auto* disp = new FlaggingDispatcher();
    auto  view = make_document_view(open_fixture("tests/fixtures/strip-chapters.fb2"), *disp);
    REQUIRE(view);
    const Document& doc = view->document();

    // 1. Size pages first: set_viewport calls Document::page_size, a
    //    Foreground acquire on this thread, which would wait behind the hold.
    view->set_viewport(800.0f, 600.0f, 96.0f);

    // 2. A helper holds the gate in the Foreground class.
    litepdf::test_kit::Gate    latch;
    litepdf::test_kit::Events  progress;
    litepdf::test_kit::JoinAll threads;
    litepdf::test_kit::GateGuard guard(latch);
    threads.add(std::thread([&] {
        SharedDocumentAccess::with_document(doc, GateClass::Foreground, [&](fz_document*) {
            progress.record("held");
            latch.wait();
        });
    }));
    REQUIRE(progress.wait_for("held"));

    // 3. A scan and a render queue on the gate.
    view->search().set_query(L"a", litepdf::core::SearchSession::Flags{});
    const int pages = std::min(10, view->page_count());
    for (int p = 0; p < pages; ++p) {
        view->request_render(p, [](fz_pixmap* pix, fz_context* ctx) {
            if (pix) fz_drop_pixmap(ctx, pix);
        });
    }
    auto& gate = SharedDocumentAccess::gate(doc);
    REQUIRE(litepdf::test_kit::poll_until([&] {
        return gate.waiting(GateClass::Scan) == 1 && gate.waiting(GateClass::Render) == 1;
    }));

    // 4. Destroy the view on another thread; once destruction has begun, let
    //    the hold go.
    auto run = litepdf::test_kit::BoundedRun::start(
        [v = std::shared_ptr<litepdf::core::DocumentView>(std::move(view))]() mutable {
            v.reset();
        });
    REQUIRE(litepdf::test_kit::poll_until([&] { return disp->discarded(); }));
    latch.open();

    // 5. Destruction finished.
    CHECK(run->wait_for(std::chrono::seconds(10)));
}
