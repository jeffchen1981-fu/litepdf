#include <catch2/catch_test_macros.hpp>

#include "cli/render_to_ppm.hpp"
#include "core/Document.hpp"

#include <chrono>
#include <cstdio>

// Regression guard for the litepdf-cli `--render` teardown use-after-free.
//
// The bug (pre-fix): the `--render` block declared `RenderEngine engine` BEFORE
// the m/cv/done sync primitives its submitted callback captured by reference.
// On the 10 s timeout path main returned, those stack locals were destroyed
// first, and then ~RenderEngine() joined the worker — which drains the still
// in-flight render and fires the callback into the already-destroyed locals.
//
// render_page_to_ppm fixes this by construction: the wait state is
// shared_ptr-co-owned by the callback, so the drained render writes into live
// state no matter the stack-destruction order. This test forces the dangerous
// path deterministically with a 0 ms wait and proves teardown is clean: no
// crash, the timeout code is reported, and the render that completes during
// ~RenderEngine() still produces a well-formed PPM in the sink.
TEST_CASE("render_page_to_ppm: 0 ms wait hits the timeout-teardown path safely",
          "[cli][render][teardown]") {
    litepdf::core::Document doc;
    REQUIRE_FALSE(doc.open("tests/fixtures/simple.pdf").has_value());

    // tmpfile(): process-private handle, auto-removed on close (no fixture
    // litter). The drained render writes its PPM here during teardown.
    std::FILE* sink = std::tmpfile();
    REQUIRE(sink != nullptr);

    const int rc = litepdf::cli::render_page_to_ppm(
        doc, 0, sink, std::chrono::milliseconds(0));

    // A real page render cannot finish in ~0 ms, so we almost always take the
    // timeout path (rc == 3). A freakishly fast machine could let the render
    // win the race (rc == 0); both outcomes are teardown-safe and acceptable.
    // The load-bearing fact is that we got here at all: ~RenderEngine() drained
    // the in-flight callback without touching freed memory.
    REQUIRE((rc == 3 || rc == 0));

    // The drained callback wrote the PPM during ~RenderEngine() (which runs
    // before render_page_to_ppm returns), so the sink now holds a complete P6
    // image. Asserting the header is intentional and load-bearing: it is the
    // positive evidence that the drained callback executed safely against live
    // shared state rather than freed stack. (The raw use-after-free is
    // sanitizer-invisible, so "didn't crash" alone would be near-vacuous.)
    // This also matches the CLI's behavior — a render that finishes during
    // teardown emits its PPM even on the timeout return.
    std::rewind(sink);
    char magic[3] = {0, 0, 0};
    int w = 0, h = 0, maxval = 0;
    const int matched =
        std::fscanf(sink, "%2s %d %d %d", magic, &w, &h, &maxval);
    std::fclose(sink);

    REQUIRE(matched == 4);
    REQUIRE(magic[0] == 'P');
    REQUIRE(magic[1] == '6');
    REQUIRE(w > 0);
    REQUIRE(h > 0);
    REQUIRE(maxval == 255);
}

// Locks the rc == 2 (no-pixmap) contract at the function level. The engine
// delivers a null pixmap for an out-of-range page (fz_load_page throws,
// fz_catch nulls it); render_page_to_ppm must report 2 and write nothing.
// Guards against got_pixmap ever being miscomputed into a false success (rc 0).
TEST_CASE("render_page_to_ppm: out-of-range page returns 2 and writes no PPM",
          "[cli][render][no-pixmap]") {
    litepdf::core::Document doc;
    REQUIRE_FALSE(doc.open("tests/fixtures/simple.pdf").has_value());

    std::FILE* sink = std::tmpfile();
    REQUIRE(sink != nullptr);

    const int bad_page = static_cast<int>(doc.page_count()) + 100;
    const int rc = litepdf::cli::render_page_to_ppm(
        doc, bad_page, sink, std::chrono::seconds(10));

    REQUIRE(rc == 2);

    // Nothing is written on the no-pixmap path.
    std::fseek(sink, 0, SEEK_END);
    REQUIRE(std::ftell(sink) == 0);
    std::fclose(sink);
}

// Locks the rc == 0 success contract deterministically. The teardown test
// above only reaches rc == 0 incidentally (on a lost 0 ms race); a generous
// timeout makes the render complete before the wait, so the success path and
// the BGRA->RGB PPM serialization are always exercised here.
TEST_CASE("render_page_to_ppm: success path returns 0 with a well-formed PPM",
          "[cli][render][success]") {
    litepdf::core::Document doc;
    REQUIRE_FALSE(doc.open("tests/fixtures/simple.pdf").has_value());

    std::FILE* sink = std::tmpfile();
    REQUIRE(sink != nullptr);

    const int rc = litepdf::cli::render_page_to_ppm(
        doc, 0, sink, std::chrono::seconds(10));

    REQUIRE(rc == 0);

    std::rewind(sink);
    char magic[3] = {0, 0, 0};
    int w = 0, h = 0, maxval = 0;
    const int matched =
        std::fscanf(sink, "%2s %d %d %d", magic, &w, &h, &maxval);
    std::fclose(sink);

    REQUIRE(matched == 4);
    REQUIRE(magic[0] == 'P');
    REQUIRE(magic[1] == '6');
    REQUIRE(w > 0);
    REQUIRE(h > 0);
    REQUIRE(maxval == 255);
}
