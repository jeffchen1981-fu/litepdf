#include "core/Document.hpp"
#include "core/PageCache.hpp"

#include <catch2/catch_test_macros.hpp>

// Forward-declare the MuPDF C ABI bits we need. The unit-tests target does
// not get MuPDF's include path (litepdf_core links mupdf PRIVATE), so these
// extern-C declarations are sufficient to construct/drop display lists in
// tests — mirrors the pattern used in test_page_cache_l2.cpp.
extern "C" {
struct fz_context;
struct fz_display_list;
typedef struct { float x0, y0, x1, y1; } fz_rect;
fz_display_list* fz_new_display_list(fz_context*, fz_rect mediabox);
void fz_drop_display_list(fz_context*, fz_display_list*);
void fz_drop_context(fz_context*);
}

namespace {

fz_display_list* make_list(fz_context* ctx) {
    fz_rect mb{0.0f, 0.0f, 100.0f, 100.0f};
    return fz_new_display_list(ctx, mb);
}

}  // namespace

TEST_CASE("peek_display_list returns nullptr for cold page", "[pagecache][peek]") {
    litepdf::core::Document doc;
    REQUIRE_FALSE(doc.open("tests/fixtures/simple.pdf").has_value());
    fz_context* ctx = doc.clone_context();
    REQUIRE(ctx);
    {
        litepdf::core::PageCache cache(/*l1*/ 5, /*l2*/ 10, ctx);

        REQUIRE(cache.peek_display_list(0) == nullptr);
        REQUIRE(cache.peek_display_list(42) == nullptr);
    }
    fz_drop_context(ctx);
}

TEST_CASE("peek_display_list returns borrowed non-owning pointer", "[pagecache][peek]") {
    litepdf::core::Document doc;
    REQUIRE_FALSE(doc.open("tests/fixtures/simple.pdf").has_value());
    fz_context* ctx = doc.clone_context();
    REQUIRE(ctx);
    {
        litepdf::core::PageCache cache(/*l1*/ 5, /*l2*/ 10, ctx);

        fz_display_list* list = make_list(ctx);
        REQUIRE(list);
        cache.put_display_list(3, list);  // cache takes caller's ref

        fz_display_list* borrowed = cache.peek_display_list(3);
        REQUIRE(borrowed == list);
        // Borrowed reference must NOT be dropped by caller — cache still
        // owns it. (The test simply not dropping is the assertion; cache's
        // destructor will drop the single ref it holds.)
    }
    fz_drop_context(ctx);
}

TEST_CASE("peek_display_list does NOT alter LRU order", "[pagecache][peek]") {
    // L2 capacity 2. Put pages A then B; B is MRU, A is LRU.
    // peek(A) and then put(C): if peek touched LRU, A would survive and
    // B would be evicted. If peek is read-only, A is still LRU and gets
    // evicted.
    litepdf::core::Document doc;
    REQUIRE_FALSE(doc.open("tests/fixtures/simple.pdf").has_value());
    fz_context* ctx = doc.clone_context();
    REQUIRE(ctx);
    {
        litepdf::core::PageCache cache(/*l1*/ 5, /*l2*/ 2, ctx);

        cache.put_display_list(1, make_list(ctx));  // A
        cache.put_display_list(2, make_list(ctx));  // B (A is now LRU)

        (void)cache.peek_display_list(1);  // peek A — must NOT touch LRU

        cache.put_display_list(3, make_list(ctx));  // C evicts LRU (= A)

        REQUIRE(cache.peek_display_list(1) == nullptr);    // A evicted
        REQUIRE(cache.peek_display_list(2) != nullptr);    // B survives
        REQUIRE(cache.peek_display_list(3) != nullptr);    // C is newest
    }
    fz_drop_context(ctx);
}
