#include "core/Document.hpp"
#include "core/PageCache.hpp"

#include <catch2/catch_test_macros.hpp>

// Forward-declare the MuPDF C ABI bits we need. The unit-tests target does
// not get MuPDF's include path (litepdf_core links mupdf PRIVATE), so these
// extern-C declarations are sufficient to construct/drop display lists in
// tests.
//
// NOTE: per mupdf/fitz/display-list.h, fz_new_display_list takes fz_rect
// BY VALUE (not pointer). fz_rect is a plain 4-float struct; we mirror the
// layout here and pass a zero-initialised rect.
extern "C" {
struct fz_context;
struct fz_display_list;
typedef struct { float x0, y0, x1, y1; } fz_rect;
fz_display_list* fz_new_display_list(fz_context*, fz_rect mediabox);
void fz_drop_display_list(fz_context*, fz_display_list*);
void fz_drop_context(fz_context*);
}

static fz_display_list* make_list(fz_context* ctx) {
    fz_rect mb{0.0f, 0.0f, 0.0f, 0.0f};
    return fz_new_display_list(ctx, mb);
}

TEST_CASE("PageCache L2 stores and retrieves by page_num", "[core][cache][l2]") {
    litepdf::core::Document doc;
    REQUIRE_FALSE(doc.open("tests/fixtures/simple.pdf").has_value());
    fz_context* ctx = doc.clone_context();
    REQUIRE(ctx);
    {
        litepdf::core::PageCache cache(/*l1*/ 0, /*l2*/ 2, ctx);

        fz_display_list* d0 = make_list(ctx);
        cache.put_display_list(0, d0);  // cache takes the only ref

        fz_display_list* got = cache.get_display_list(0);  // caller gets new ref
        REQUIRE(got == d0);
        REQUIRE(cache.get_display_list(1) == nullptr);

        fz_drop_display_list(ctx, got);
    }
    fz_drop_context(ctx);
}

TEST_CASE("PageCache L2 evicts LRU on overflow", "[core][cache][l2]") {
    litepdf::core::Document doc;
    REQUIRE_FALSE(doc.open("tests/fixtures/simple.pdf").has_value());
    fz_context* ctx = doc.clone_context();
    REQUIRE(ctx);
    {
        litepdf::core::PageCache cache(0, 2, ctx);

        fz_display_list* a = make_list(ctx);
        fz_display_list* b = make_list(ctx);
        fz_display_list* c = make_list(ctx);

        cache.put_display_list(0, a);
        cache.put_display_list(1, b);
        REQUIRE(cache.l2_size() == 2);
        cache.put_display_list(2, c);  // evicts page-0 (oldest)
        REQUIRE(cache.l2_size() == 2);

        REQUIRE(cache.get_display_list(0) == nullptr);
        fz_display_list* got_b = cache.get_display_list(1);
        REQUIRE(got_b == b);
        fz_drop_display_list(ctx, got_b);
        fz_display_list* got_c = cache.get_display_list(2);
        REQUIRE(got_c == c);
        fz_drop_display_list(ctx, got_c);
    }
    fz_drop_context(ctx);
}

TEST_CASE("PageCache L2 get-hit promotes to most-recent", "[core][cache][l2]") {
    litepdf::core::Document doc;
    REQUIRE_FALSE(doc.open("tests/fixtures/simple.pdf").has_value());
    fz_context* ctx = doc.clone_context();
    REQUIRE(ctx);
    {
        litepdf::core::PageCache cache(0, 2, ctx);

        fz_display_list* a = make_list(ctx);
        fz_display_list* b = make_list(ctx);
        fz_display_list* c = make_list(ctx);

        cache.put_display_list(0, a);
        cache.put_display_list(1, b);
        // Touch page 0 — now page 1 is the LRU.
        fz_display_list* got_a = cache.get_display_list(0);
        REQUIRE(got_a);
        fz_drop_display_list(ctx, got_a);
        cache.put_display_list(2, c);  // should evict page 1, not page 0

        REQUIRE(cache.get_display_list(1) == nullptr);
        fz_display_list* still_a = cache.get_display_list(0);
        REQUIRE(still_a == a);
        fz_drop_display_list(ctx, still_a);
        fz_display_list* still_c = cache.get_display_list(2);
        REQUIRE(still_c == c);
        fz_drop_display_list(ctx, still_c);
    }
    fz_drop_context(ctx);
}

TEST_CASE("PageCache L2 put with null is a no-op", "[core][cache][l2]") {
    litepdf::core::Document doc;
    REQUIRE_FALSE(doc.open("tests/fixtures/simple.pdf").has_value());
    fz_context* ctx = doc.clone_context();
    REQUIRE(ctx);
    {
        litepdf::core::PageCache cache(0, 2, ctx);
        cache.put_display_list(0, nullptr);
        REQUIRE(cache.l2_size() == 0);
    }
    fz_drop_context(ctx);
}

TEST_CASE("PageCache clear drops both L1 and L2", "[core][cache][l2]") {
    litepdf::core::Document doc;
    REQUIRE_FALSE(doc.open("tests/fixtures/simple.pdf").has_value());
    fz_context* ctx = doc.clone_context();
    REQUIRE(ctx);
    {
        litepdf::core::PageCache cache(0, 2, ctx);
        fz_display_list* a = make_list(ctx);
        cache.put_display_list(0, a);
        REQUIRE(cache.l2_size() == 1);
        cache.clear();
        REQUIRE(cache.l2_size() == 0);
    }
    fz_drop_context(ctx);
}
