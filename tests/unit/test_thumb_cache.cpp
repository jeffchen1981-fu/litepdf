#include "core/ThumbCache.hpp"

#include <catch2/catch_test_macros.hpp>
#include <windows.h>

using litepdf::core::ThumbCache;

namespace {
HBITMAP make_dummy_bitmap() {
    // 2x2 monochrome - minimum viable HBITMAP.
    return CreateBitmap(2, 2, 1, 1, nullptr);
}
}

TEST_CASE("ThumbCache: empty cache returns null on miss", "[thumb_cache]") {
    ThumbCache c(4);
    REQUIRE(c.get(0) == nullptr);
}

TEST_CASE("ThumbCache: put then get returns same handle", "[thumb_cache]") {
    ThumbCache c(4);
    HBITMAP b = make_dummy_bitmap();
    c.put(7, b);
    REQUIRE(c.get(7) == b);
    REQUIRE(c.size() == 1);
}

TEST_CASE("ThumbCache: replacing same key drops old bitmap", "[thumb_cache]") {
    ThumbCache c(4);
    HBITMAP a = make_dummy_bitmap();
    HBITMAP b = make_dummy_bitmap();
    c.put(3, a);
    c.put(3, b);
    REQUIRE(c.get(3) == b);
    REQUIRE(c.size() == 1);
    // a was DeleteObject'd by put(3, b). We can't verify Win32-side cleanly
    // without a leak detector, but logical state must show b only.
}

TEST_CASE("ThumbCache: put with same page+same handle is a no-op",
          "[thumb_cache]") {
    // Added per plan-eng-review 2B: defensive test against accidental
    // double-DeleteObject on idempotent puts.
    ThumbCache c(4);
    HBITMAP a = make_dummy_bitmap();
    c.put(3, a);
    c.put(3, a);  // same key + same handle - must NOT DeleteObject(a)
    REQUIRE(c.get(3) == a);
    REQUIRE(c.size() == 1);
    // No way to assert DeleteObject wasn't called from this side, but the
    // implementation's same-handle short-circuit means logical state is
    // intact. If put() ever drops same-handle, get(3) would still return
    // a stale pointer that BitBlt would crash on - surfaces under the
    // smoke-test handle-count check in T9.
}

TEST_CASE("ThumbCache: capacity overflow evicts least-recently-used", "[thumb_cache]") {
    ThumbCache c(2);
    c.put(1, make_dummy_bitmap());
    c.put(2, make_dummy_bitmap());
    (void)c.get(1);  // touch 1 - 2 is now LRU
    c.put(3, make_dummy_bitmap());
    REQUIRE(c.get(2) == nullptr);  // evicted
    REQUIRE(c.get(1) != nullptr);
    REQUIRE(c.get(3) != nullptr);
    REQUIRE(c.size() == 2);
}

TEST_CASE("ThumbCache: clear empties cache", "[thumb_cache]") {
    ThumbCache c(4);
    c.put(1, make_dummy_bitmap());
    c.put(2, make_dummy_bitmap());
    c.clear();
    REQUIRE(c.size() == 0);
    REQUIRE(c.get(1) == nullptr);
}
