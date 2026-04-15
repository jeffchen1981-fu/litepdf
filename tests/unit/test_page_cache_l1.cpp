#include "core/Document.hpp"
#include "core/PageCache.hpp"

#include <catch2/catch_test_macros.hpp>

// Forward-declare the MuPDF C ABI bits we need. The unit-tests target does
// not get MuPDF's include path (litepdf_core links mupdf PRIVATE), so these
// extern-C declarations are sufficient to construct/drop pixmaps in tests.
extern "C" {
struct fz_context;
struct fz_pixmap;
struct fz_colorspace;
fz_pixmap* fz_new_pixmap(fz_context*, fz_colorspace*, int w, int h, void* seps, int alpha);
fz_colorspace* fz_device_rgb(fz_context*);
void fz_drop_pixmap(fz_context*, fz_pixmap*);
void fz_drop_context(fz_context*);
}

TEST_CASE("PageCache L1 stores and retrieves by (page, scale)", "[core][cache][l1]") {
    litepdf::core::Document doc;
    REQUIRE_FALSE(doc.open("tests/fixtures/simple.pdf").has_value());
    fz_context* ctx = doc.clone_context();
    REQUIRE(ctx);
    {
        litepdf::core::PageCache cache(/*l1*/ 2, /*l2*/ 0, ctx);

        fz_pixmap* p0 = fz_new_pixmap(ctx, fz_device_rgb(ctx), 10, 10, nullptr, 0);
        cache.put_pixmap(0, 1.0f, p0);  // cache takes the only ref

        fz_pixmap* got = cache.get_pixmap(0, 1.0f);    // caller gets a new ref
        REQUIRE(got == p0);                            // same underlying pointer
        REQUIRE(cache.get_pixmap(0, 2.0f) == nullptr); // different scale = miss
        REQUIRE(cache.get_pixmap(1, 1.0f) == nullptr); // different page = miss

        fz_drop_pixmap(ctx, got);  // caller drops its ref
        // cache destructor drops its own ref
    }
    fz_drop_context(ctx);
}

TEST_CASE("PageCache L1 evicts LRU on overflow", "[core][cache][l1]") {
    litepdf::core::Document doc;
    REQUIRE_FALSE(doc.open("tests/fixtures/simple.pdf").has_value());
    fz_context* ctx = doc.clone_context();
    REQUIRE(ctx);
    {
        litepdf::core::PageCache cache(2, 0, ctx);

        fz_pixmap* a = fz_new_pixmap(ctx, fz_device_rgb(ctx), 10, 10, nullptr, 0);
        fz_pixmap* b = fz_new_pixmap(ctx, fz_device_rgb(ctx), 10, 10, nullptr, 0);
        fz_pixmap* c = fz_new_pixmap(ctx, fz_device_rgb(ctx), 10, 10, nullptr, 0);

        cache.put_pixmap(0, 1.0f, a);
        cache.put_pixmap(1, 1.0f, b);
        REQUIRE(cache.l1_size() == 2);
        cache.put_pixmap(2, 1.0f, c);  // evicts page-0 (oldest)
        REQUIRE(cache.l1_size() == 2);

        REQUIRE(cache.get_pixmap(0, 1.0f) == nullptr);  // evicted
        fz_pixmap* got_b = cache.get_pixmap(1, 1.0f);
        REQUIRE(got_b == b);
        fz_drop_pixmap(ctx, got_b);
        fz_pixmap* got_c = cache.get_pixmap(2, 1.0f);
        REQUIRE(got_c == c);
        fz_drop_pixmap(ctx, got_c);
    }
    fz_drop_context(ctx);
}

TEST_CASE("PageCache L1 get-hit promotes to most-recent", "[core][cache][l1]") {
    litepdf::core::Document doc;
    REQUIRE_FALSE(doc.open("tests/fixtures/simple.pdf").has_value());
    fz_context* ctx = doc.clone_context();
    REQUIRE(ctx);
    {
        litepdf::core::PageCache cache(2, 0, ctx);
        fz_pixmap* a = fz_new_pixmap(ctx, fz_device_rgb(ctx), 10, 10, nullptr, 0);
        fz_pixmap* b = fz_new_pixmap(ctx, fz_device_rgb(ctx), 10, 10, nullptr, 0);
        fz_pixmap* c = fz_new_pixmap(ctx, fz_device_rgb(ctx), 10, 10, nullptr, 0);

        cache.put_pixmap(0, 1.0f, a);
        cache.put_pixmap(1, 1.0f, b);
        // Touch page 0 — now page 1 is the LRU.
        fz_pixmap* got_a = cache.get_pixmap(0, 1.0f);
        REQUIRE(got_a);
        fz_drop_pixmap(ctx, got_a);
        cache.put_pixmap(2, 1.0f, c);  // should evict page 1, not page 0

        REQUIRE(cache.get_pixmap(1, 1.0f) == nullptr);  // evicted
        fz_pixmap* still_a = cache.get_pixmap(0, 1.0f);
        REQUIRE(still_a == a);
        fz_drop_pixmap(ctx, still_a);
        fz_pixmap* still_c = cache.get_pixmap(2, 1.0f);
        REQUIRE(still_c == c);
        fz_drop_pixmap(ctx, still_c);
    }
    fz_drop_context(ctx);
}

TEST_CASE("PageCache L1 put with null pixmap is a no-op", "[core][cache][l1]") {
    litepdf::core::Document doc;
    REQUIRE_FALSE(doc.open("tests/fixtures/simple.pdf").has_value());
    fz_context* ctx = doc.clone_context();
    REQUIRE(ctx);
    {
        litepdf::core::PageCache cache(2, 0, ctx);
        cache.put_pixmap(0, 1.0f, nullptr);
        REQUIRE(cache.l1_size() == 0);
    }
    fz_drop_context(ctx);
}

TEST_CASE("PageCache L1 clear drops everything", "[core][cache][l1]") {
    litepdf::core::Document doc;
    REQUIRE_FALSE(doc.open("tests/fixtures/simple.pdf").has_value());
    fz_context* ctx = doc.clone_context();
    REQUIRE(ctx);
    {
        litepdf::core::PageCache cache(4, 0, ctx);
        for (int i = 0; i < 4; ++i) {
            fz_pixmap* p = fz_new_pixmap(ctx, fz_device_rgb(ctx), 10, 10, nullptr, 0);
            cache.put_pixmap(i, 1.0f, p);
        }
        REQUIRE(cache.l1_size() == 4);
        cache.clear();
        REQUIRE(cache.l1_size() == 0);
    }
    fz_drop_context(ctx);
}
