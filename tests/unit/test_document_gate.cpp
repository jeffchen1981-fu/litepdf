// #229: the Document's priority gate, the borrow seam and is_fb2 (spec §3.1-3.3).
//
// A case whose defect could deadlock (a re-entry the gate no longer refuses, a
// print that calls back under its hold) runs the blocking part through
// finishes_within on a heap-owned Document, and the helper thread records what
// it saw for the main thread to assert.

#include <catch2/catch_test_macros.hpp>

#include "core/Document.hpp"
#include "core/detail/PriorityGate.hpp"
#include "core/detail/SharedDocumentAccess.hpp"
#include "gate_test_kit.hpp"

#include <mupdf/fitz.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include <windows.h>

using litepdf::core::Document;
using litepdf::core::detail::GateClass;
using litepdf::core::detail::SharedDocumentAccess;
using litepdf::test_kit::BoundedRun;
using litepdf::test_kit::finishes_within;

namespace {

constexpr const char* kFb2 = "tests/fixtures/strip-chapters.fb2";

Document open_fixture(const std::filesystem::path& path) {
    Document doc;
    REQUIRE_FALSE(doc.open(path).has_value());
    return doc;
}

std::shared_ptr<Document> open_shared(const std::filesystem::path& path) {
    auto doc = std::make_shared<Document>();
    REQUIRE_FALSE(doc->open(path).has_value());
    return doc;
}

// A copy of `source` under a new name in the temp directory, removed on exit.
struct TempCopy {
    std::filesystem::path path;
    TempCopy(const std::filesystem::path& source, const wchar_t* name) {
        static std::atomic<unsigned> seq{0};
        path = std::filesystem::temp_directory_path() /
               (L"litepdf_doc_gate_" + std::to_wstring(GetCurrentProcessId()) + L"_" +
                std::to_wstring(seq.fetch_add(1)) + L"_" + name);
        std::filesystem::copy_file(source, path,
                                   std::filesystem::copy_options::overwrite_existing);
    }
    ~TempCopy() { std::error_code ec; std::filesystem::remove(path, ec); }
    TempCopy(const TempCopy&)            = delete;
    TempCopy& operator=(const TempCopy&) = delete;
};

}  // namespace

// The format predicate reads what MuPDF opened, so FB2 content under another
// allowed name (.svg has no magic check, and MuPDF scores content above the
// extension) is FB2, and a PDF named .fb2 is not.
TEST_CASE("Document: is_fb2 follows the content, not the name", "[core][gate][document]") {
    CHECK(open_fixture(kFb2).is_fb2());
    {
        // Spec §3.1's premise: if this copy does not open as FB2, the shared
        // view's worker count rule for it is moot.
        const TempCopy svg{kFb2, L"book.svg"};
        CHECK(open_fixture(svg.path).is_fb2());
    }
    CHECK_FALSE(open_fixture("tests/fixtures/simple.pdf").is_fb2());
    {
        const TempCopy pdf{"tests/fixtures/simple.pdf", L"plain.fb2"};
        CHECK_FALSE(open_fixture(pdf.path).is_fb2());
    }
    CHECK_FALSE(open_fixture("tests/fixtures/sample.svg").is_fb2());
    CHECK_FALSE(open_fixture("tests/fixtures/sample.epub").is_fb2());

    Document closed;
    CHECK_FALSE(closed.is_fb2());
    Document reopened = open_fixture(kFb2);
    reopened.close();
    CHECK_FALSE(reopened.is_fb2());
}

TEST_CASE("SharedDocument: with_document runs on an open document only",
          "[core][gate][document]") {
    Document doc = open_fixture(kFb2);
    fz_context* ctx = doc.clone_context();
    REQUIRE(ctx != nullptr);

    bool got_doc = false;
    int  count   = -1;
    const bool ran = SharedDocumentAccess::with_document(
        doc, GateClass::Foreground, [&](fz_document* d) {
            got_doc = d != nullptr;
            if (!d) return;
            int n = -1;
            fz_var(n);
            fz_try(ctx) { n = fz_count_pages(ctx, d); }
            fz_catch(ctx) { fz_report_error(ctx); }
            count = n;
        });
    fz_drop_context(ctx);
    CHECK(ran);
    CHECK(got_doc);
    CHECK(count == static_cast<int>(doc.page_count()));

    Document closed;
    bool called = false;
    CHECK_FALSE(SharedDocumentAccess::with_document(closed, GateClass::Foreground,
                                                    [&](fz_document*) { called = true; }));
    CHECK_FALSE(called);
}

// G4, second half: the #70 contract under the gate. text_page on a thread that
// already holds the gate gets an empty handle, not a deadlock and not the
// std::terminate a throw out of a noexcept function would be.
TEST_CASE("Document: text_page under a held gate is empty", "[core][gate][document]") {
    auto doc     = open_shared(kFb2);
    auto inside  = std::make_shared<std::atomic<int>>(-1);
    auto outside = std::make_shared<std::atomic<int>>(-1);
    REQUIRE(finishes_within(std::chrono::seconds(5), [doc, inside, outside] {
        SharedDocumentAccess::with_document(*doc, GateClass::Foreground, [&](fz_document*) {
            inside->store(doc->text_page(0).valid() ? 1 : 0);
        });
        outside->store(doc->text_page(0).valid() ? 1 : 0);
    }));
    CHECK(inside->load() == 0);
    CHECK(outside->load() == 1);
}

// G9: print calls back with the gate released. A dispatched message can then
// call into the Document on the same thread, and a borrower on another thread
// is not held up behind the callback (spec §3.3).
TEST_CASE("Document: with_rendered_page calls back with the gate released",
          "[core][gate][document]") {
    auto doc      = open_shared(kFb2);
    auto rendered = std::make_shared<std::atomic<bool>>(false);
    auto sized    = std::make_shared<std::atomic<bool>>(false);
    auto borrowed = std::make_shared<std::atomic<bool>>(false);
    REQUIRE(finishes_within(std::chrono::seconds(10), [doc, rendered, sized, borrowed] {
        const bool ok = doc->with_rendered_page(
            0, 1.0f, 1.0f, false, [&](int, int, const std::uint8_t*) {
                try {
                    (void)doc->page_size(0);
                    sized->store(true);
                } catch (...) {
                }
                auto run = BoundedRun::start([doc] {
                    SharedDocumentAccess::with_document(*doc, GateClass::Render,
                                                        [](fz_document*) {});
                });
                borrowed->store(run->wait_for(std::chrono::seconds(5)));
            });
        rendered->store(ok);
    }));
    CHECK(rendered->load());
    CHECK(sized->load());
    CHECK(borrowed->load());
}

// The callback can destroy the Document (PrintJob's abort_proc dispatches a tab
// close), so nothing after it may touch the Document. Nothing else of the
// family is alive, so the Document's death frees the root context and the lock
// table too. In a Release build this is a smoke test; a read of the freed Impl,
// or a drop through the freed root context, is caught by the ASan build (plan
// Task 8 Step 1b).
TEST_CASE("Document: with_rendered_page touches nothing of its Document after the callback",
          "[core][gate][document]") {
    auto owner = std::make_unique<Document>();
    REQUIRE_FALSE(owner->open(kFb2).has_value());
    Document& doc = *owner;

    std::vector<std::uint8_t> row;
    const bool ok = doc.with_rendered_page(
        0, 1.0f, 1.0f, false, [&](int w, int, const std::uint8_t* bgra) {
            row.assign(bgra, bgra + static_cast<std::size_t>(w) * 4);
            owner.reset();
        });
    CHECK(ok);
    CHECK(owner == nullptr);
    REQUIRE_FALSE(row.empty());
    CHECK(std::any_of(row.begin(), row.end(), [](std::uint8_t b) { return b != 0; }));
}
