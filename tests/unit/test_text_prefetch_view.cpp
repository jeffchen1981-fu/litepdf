// #98: DocumentView keeps at most one text page per tab -- the current page's --
// in a slot, asks its P0 render request for the page's text while the slot does
// not hold it, and serves text-selection presses from the slot (spec §3.5,
// §5.1-2, -3, -6).
//
// Every view gets a viewport right after it is built: without one the fit
// percentage, and so the render scale, is 0. Every wait is bounded. The text
// callback's promise lives in a shared_ptr the callback owns, so a worker still
// inside set_value when the future wakes never touches freed state.
#include "app/SearchDispatcher.hpp"
#include "core/Document.hpp"
#include "core/DocumentView.hpp"
#include "core/EscrowContext.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <future>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

// The few C-ABI entry points the callbacks and the probe need; keeps this TU
// MuPDF-header-free (see test_document_view.cpp).
extern "C" {
    struct fz_context;
    struct fz_pixmap;
    void fz_drop_pixmap(fz_context*, fz_pixmap*);
    void fz_drop_context(fz_context*);
}

using litepdf::app::InlineDispatcher;
using litepdf::core::Document;
using litepdf::core::DocumentView;
using litepdf::core::EscrowContext;
using litepdf::core::SelPoint;

namespace {

constexpr auto kWait = std::chrono::seconds(10);
constexpr const char* kSelection = "tests/fixtures/selection.pdf";   // 8 pages

// --- copied from test_document_selection.cpp -------------------------------

// A selection copy and page_text() lay out line breaks differently -- CRLF
// versus LF, and page_text adds a blank line after every block -- but must agree
// on every character. Compare with whitespace removed.
std::string without_whitespace(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c != ' ' && c != '\r' && c != '\n' && c != '\t') out.push_back(c);
    }
    return out;
}

// --- harness ----------------------------------------------------------------

Document open_fixture(const char* path) {
    Document doc;
    REQUIRE_FALSE(doc.open(path).has_value());
    return doc;
}

void set_test_viewport(DocumentView& view) {
    view.set_viewport(800.0f, 600.0f, 96.0f);
}

void drop_pixmap(fz_pixmap* p, fz_context* ctx) {
    if (p) fz_drop_pixmap(ctx, p);
}

// The whole page's text, or "" on a page without any.
std::string full_text(const Document::TextPage& t) {
    SelPoint first, last;
    if (!t.full_range(first, last)) return {};
    return t.copy(first, last);
}

// Makes `page` current, renders it with a text callback, and returns the handle
// the worker built -- WITHOUT offering it to the view.
Document::TextPage get_worker_text(DocumentView& view, int page) {
    (void)view.set_current_page(page);
    auto promise = std::make_shared<std::promise<Document::TextPage>>();
    auto text = promise->get_future();
    view.request_render_with_prefetch(page, drop_pixmap,
        [promise](Document::TextPage t) { promise->set_value(std::move(t)); });
    REQUIRE(text.wait_for(kWait) == std::future_status::ready);
    return text.get();
}

// The EscrowContext probe (constraints): counts the view's MuPDF context family,
// and keeps counting after the Document dies.
EscrowContext family_probe(const DocumentView& view) {
    fz_context* tmp = view.document().clone_context();
    REQUIRE(tmp != nullptr);
    EscrowContext probe = EscrowContext::clone_from(tmp);
    fz_drop_context(tmp);
    REQUIRE(probe.valid());
    return probe;
}

std::size_t contexts(const EscrowContext& probe) {
    return litepdf::core::detail::family_context_count(probe.get());
}

}  // namespace

TEST_CASE("DocumentView prefetch gate", "[core][view][text_prefetch]") {
    SECTION("selection.pdf") {
        InlineDispatcher disp;
        DocumentView view(open_fixture(kSelection), disp);
        set_test_viewport(view);

        REQUIRE(view.wants_text_prefetch(0));
        view.set_dual_page(true);
        REQUIRE_FALSE(view.wants_text_prefetch(0));
        view.set_dual_page(false);
        REQUIRE(view.wants_text_prefetch(0));

        view.offer_text(get_worker_text(view, 0));
        REQUIRE(view.has_text_for(0));
        REQUIRE_FALSE(view.wants_text_prefetch(0));
    }

    SECTION("a user-password PDF whose view has the password (#126)") {
        // The workers authenticate with the password the view was built with,
        // so the gate needs no password condition -- and the text they build
        // must be the real page's, not a wrong-key garble.
        InlineDispatcher disp;
        Document doc;
        const auto err = doc.open("tests/fixtures/encrypted-text.pdf");
        REQUIRE(err.has_value());
        REQUIRE(*err == Document::OpenError::NeedsPassword);
        REQUIRE(doc.authenticate("test"));
        DocumentView view(std::move(doc), disp, "test");
        set_test_viewport(view);

        REQUIRE(view.wants_text_prefetch(0));
        const Document::TextPage worker = get_worker_text(view, 0);
        REQUIRE(worker.valid());
        const std::string expected = full_text(view.document().text_page(0));
        REQUIRE_FALSE(expected.empty());
        REQUIRE(full_text(worker) == expected);
    }
}

TEST_CASE("DocumentView offer_text keeps only the current page and never replaces",
          "[core][view][text_prefetch]") {
    InlineDispatcher disp;
    DocumentView view(open_fixture(kSelection), disp);
    set_test_viewport(view);
    REQUIRE(view.current_page() == 0);

    // A handle for a page other than the current one is dropped.
    Document::TextPage other = view.document().text_page(1);
    REQUIRE(other.valid());
    view.offer_text(std::move(other));
    REQUIRE_FALSE(view.has_text_for(1));
    REQUIRE_FALSE(view.has_text_for(0));

    // Never replace: with the slot full, a second handle for the same page is
    // dropped. A replacing implementation would keep both alive (the first
    // through h_first), and the count would not fall.
    view.offer_text(get_worker_text(view, 0));
    REQUIRE(view.has_text_for(0));
    const Document::TextPage h_first = view.text_for(0);
    REQUIRE(h_first.valid());
    Document::TextPage second = view.document().text_page(0);
    REQUIRE(second.valid());

    const EscrowContext probe = family_probe(view);
    const std::size_t c = contexts(probe);
    view.offer_text(std::move(second));
    REQUIRE(contexts(probe) == c - 1);
    REQUIRE(view.has_text_for(0));
}

TEST_CASE("DocumentView a page change empties the slot", "[core][view][text_prefetch]") {
    InlineDispatcher disp;
    DocumentView view(open_fixture(kSelection), disp);
    set_test_viewport(view);

    view.offer_text(get_worker_text(view, 0));
    REQUIRE(view.has_text_for(0));
    REQUIRE(view.set_current_page(1));
    REQUIRE_FALSE(view.has_text_for(0));
    REQUIRE_FALSE(view.has_text_for(1));

    view.offer_text(get_worker_text(view, 1));
    REQUIRE(view.has_text_for(1));
    REQUIRE_FALSE(view.set_current_page(1));   // same page: no change
    REQUIRE(view.has_text_for(1));
}

TEST_CASE("DocumentView text_for reuses the slot", "[core][view][text_prefetch]") {
    InlineDispatcher disp;
    DocumentView view(open_fixture(kSelection), disp);
    set_test_viewport(view);
    const EscrowContext probe = family_probe(view);

    const Document::TextPage h1 = view.text_for(0);   // builds; fills the slot
    REQUIRE(h1.valid());
    REQUIRE(view.has_text_for(0));

    // A rebuild clones a new escrow and would raise the count by one.
    const std::size_t c = contexts(probe);
    const Document::TextPage h2 = view.text_for(0);
    REQUIRE(h2.valid());
    REQUIRE(contexts(probe) == c);
    REQUIRE(full_text(h2) == full_text(h1));

    // Non-vacuity: another page does build.
    REQUIRE(view.set_current_page(1));
    const Document::TextPage h3 = view.text_for(1);
    REQUIRE(h3.valid());
    REQUIRE(contexts(probe) == c + 1);
}

TEST_CASE("DocumentView keeps text_wanted in step with the page",
          "[core][view][text_prefetch]") {
    InlineDispatcher disp;
    DocumentView view(open_fixture(kSelection), disp);
    set_test_viewport(view);
    // Drops the worker's handle: only the view's own writes are observed here.
    const DocumentView::TextCb ignore_text = [](Document::TextPage) {};

    REQUIRE(view.current_page() == 0);
    REQUIRE(view.pending_text_page() == -1);
    view.request_render_with_prefetch(0, drop_pixmap, ignore_text);
    REQUIRE(view.pending_text_page() == 0);

    REQUIRE(view.set_current_page(1));
    REQUIRE(view.pending_text_page() == -1);
    view.request_render_with_prefetch(1, drop_pixmap, ignore_text);
    REQUIRE(view.pending_text_page() == 1);
    view.offer_text(view.document().text_page(1));
    REQUIRE(view.has_text_for(1));
    REQUIRE(view.pending_text_page() == -1);

    // The slot holds page 1: no text is requested.
    view.request_render_with_prefetch(1, drop_pixmap, ignore_text);
    REQUIRE(view.pending_text_page() == -1);

    REQUIRE(view.set_current_page(2));
    view.request_render_with_prefetch(2, drop_pixmap, ignore_text);
    REQUIRE(view.pending_text_page() == 2);
    view.set_dual_page(true);
    REQUIRE(view.pending_text_page() == -1);
}

namespace {

// Spec §5.1-6. `expected` is page 0's whole text with whitespace removed, or
// nullptr to take it from Document::text_page while the view is alive.
void require_worker_text_outlives_view(const char* path, const char* expected) {
    INFO("fixture " << path);
    // Read before the Document is opened: opening one adds a root.
    const std::size_t r0 = litepdf::core::detail::live_mupdf_roots();
    Document::TextPage h;
    EscrowContext probe;
    std::string want = expected ? expected : "";
    {
        InlineDispatcher disp;
        auto view = std::make_unique<DocumentView>(open_fixture(path), disp);
        set_test_viewport(*view);
        probe = family_probe(*view);
        if (!expected) want = without_whitespace(full_text(view->document().text_page(0)));
        REQUIRE_FALSE(want.empty());
        h = get_worker_text(*view, 0);
        REQUIRE(h.valid());
        view.reset();   // the engine, its worker documents and the Document die
    }
    REQUIRE(without_whitespace(full_text(h)) == want);

    const std::size_t before = contexts(probe);
    h = Document::TextPage{};
    REQUIRE(contexts(probe) == before - 1);

    probe = EscrowContext{};
    REQUIRE(litepdf::core::detail::live_mupdf_roots() == r0);
}

}  // namespace

TEST_CASE("DocumentView a worker-built text page outlives its view",
          "[core][view][text_prefetch]") {
    SECTION("selection.pdf") { require_worker_text_outlives_view(kSelection, nullptr); }
    SECTION("type3.pdf")     { require_worker_text_outlives_view("tests/fixtures/type3.pdf", "ABBA"); }
}
