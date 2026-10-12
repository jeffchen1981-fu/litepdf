// #130: the lock-free page-hits helper builds its text with a cookie, and the
// escrow accessor a search session clones its own context from (spec §3.3,
// §3.4, §6 T3).
//
// Each cookie case opens a second fz_document on an escrow cloned from a tab
// Document -- the shape a SearchSession scan runs on -- and drops it on that
// escrow before the escrow goes.
#include "core/Document.hpp"
#include "core/EscrowContext.hpp"
#include "core/detail/PageHitsAccess.hpp"

#include <catch2/catch_test_macros.hpp>

#include <mupdf/fitz.h>

#include <filesystem>

using namespace litepdf::core;

namespace {

constexpr const char* kSearchFixture = "tests/fixtures/search.pdf";

// fz_open_document on `ctx`, or null when MuPDF throws.
fz_document* open_on(fz_context* ctx, const char* path) {
    fz_document* doc = nullptr;
    fz_var(doc);
    fz_try(ctx) {
        doc = fz_open_document(ctx, path);
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
    }
    return doc;
}

// Drops the document on the escrow it was opened on, even when a REQUIRE fails.
struct DocOnEscrow {
    const EscrowContext& esc;
    fz_document*         doc;
    ~DocOnEscrow() {
        if (doc) fz_drop_document(esc.get(), doc);
    }
};

}  // namespace

TEST_CASE("Document: page_hits_on passes the cookie to the text build",
          "[document][search]") {
    Document tab;
    REQUIRE_FALSE(tab.open(std::filesystem::path(kSearchFixture)).has_value());
    auto esc = tab.clone_escrow_context();
    REQUIRE(esc.valid());
    DocOnEscrow d{esc, open_on(esc.get(), kSearchFixture)};
    REQUIRE(d.doc != nullptr);

    fz_cookie c{};
    REQUIRE(detail::page_hits_on(esc.get(), d.doc, 0, "Lorem", {}, &c).size() == 12);
    REQUIRE(c.progress > 0);
}

TEST_CASE("Document: page_hits_on stops before the build when the cookie is aborted",
          "[document][search]") {
    Document tab;
    REQUIRE_FALSE(tab.open(std::filesystem::path(kSearchFixture)).has_value());
    auto esc = tab.clone_escrow_context();
    REQUIRE(esc.valid());
    DocOnEscrow d{esc, open_on(esc.get(), kSearchFixture)};
    REQUIRE(d.doc != nullptr);

    fz_cookie c{};
    detail::request_abort(c);
    REQUIRE(detail::abort_requested(c));
    REQUIRE(detail::page_hits_on(esc.get(), d.doc, 0, "Lorem", {}, &c).empty());
    // MuPDF checks abort before it counts a token (pdf-interpret.c:1558-1561).
    REQUIRE(c.progress == 0);
}

TEST_CASE("Document: clone_escrow_context is empty on an unopened document",
          "[document][search]") {
    REQUIRE_FALSE(Document{}.clone_escrow_context().valid());
}

TEST_CASE("Document: page_hits on an unopened document is empty",
          "[document][search]") {
    REQUIRE(Document{}.page_hits(0, "x", {}).empty());
}
