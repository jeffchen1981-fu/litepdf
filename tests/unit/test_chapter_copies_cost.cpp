// #232 cost oracles: every per-page load of a many-chapter ePub goes by its
// chapter location, so a loop over every page costs what loading the pages in
// chapter order costs, not what fz_load_page's walk from the first chapter
// costs. Each test compares against a MuPDF-only loop of the test's own, so the
// bound scales with the machine.
//
// The bound is measured <= kCostK * reference + kCostC. The constants are
// #230's. Planning probe P3 (1,500 chapters, best of 3, pinned to P-cores),
// every page, load + stext:
//   by location   28.76 ms     by flat number  796.63 ms
// load + dehyphenated stext (a scan):
//   by location   29.13 ms     by flat number  808.35 ms
// so the bound is about 140 ms and the flat route fails it by at least 5x,
// while the location route keeps 3x to spare (140.04 / 43.76 = 3.2x, with
// ~15 ms added by the product). The page_hits reference also runs
// fz_search_stext_page for the needle on every page, which the probe's scan row
// does not, so its bound is larger: about 340 ms on this machine.
//
// The fresh-copy oracles (a new RenderEngine's one worker, the last page):
// load + display list + scale-1 render:
//   by location    1.29 ms     by flat number  158.23 ms
// text of the last page after that render:
//   by location    0.04 ms     by flat number  157.65 ms
// so the bound is about 30 ms and the flat route fails it by at least 5x, while
// the location route keeps 2.5x to spare while the product adds under ~10 ms.
//
// A location-route reading with less than 2x headroom means k and c need
// re-deriving, not loosening here.
#include "chapter_test_kit.hpp"
#include "core/Document.hpp"
#include "core/RenderEngine.hpp"

#include <catch2/catch_test_macros.hpp>

#include <mupdf/fitz.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <utility>

using namespace litepdf::core;
using namespace litepdf::test_kit;

namespace {

constexpr double kCostK = 4.0;
constexpr double kCostC = 25.0;   // ms

// The reference loops' per-page work: the stext build the Document's path runs
// (default options for page_text and text_page, dehyphenated for page_hits).
void stext_with(fz_context* ctx, fz_page* page, int flags) {
    fz_stext_options so = {};
    so.flags = flags;
    fz_drop_stext_page(ctx, fz_new_stext_page_from_page(ctx, page, &so));
}
void stext_default(fz_context* ctx, fz_page* page) { stext_with(ctx, page, 0); }
// The reference for page_hits does the same work it does: the dehyphenated
// stext, then the search for the needle on it. The quad array is plain data and
// nothing here has a destructor, so it is safe inside the caller's fz_try (#179).
void stext_dehyphenate(fz_context* ctx, fz_page* page) {
    fz_stext_options so = {};
    so.flags = FZ_STEXT_DEHYPHENATE;
    fz_stext_page* stext = fz_new_stext_page_from_page(ctx, page, &so);
    fz_quad quads[256];
    (void)fz_search_stext_page(ctx, stext, "Lorem", nullptr, quads, 256);
    fz_drop_stext_page(ctx, stext);
}

}  // namespace

TEST_CASE("Document chapters cost: page_text of every page of a 1500-chapter ePub costs what loading by chapter costs",
          "[document][chapters][cost]") {
    const TempDir dir;   // first: destroyed after both documents
    const std::filesystem::path file = dir.path / "chapters-1500.epub";
    REQUIRE(write_many_chapter_epub(file, 1500));
    Document doc;
    REQUIRE_FALSE(doc.open(file).has_value());
    REQUIRE(doc.page_count() == 1500);
    const std::string file_utf8 = utf8_of(file);
    OwnDocument own(file_utf8.c_str());
    REQUIRE(own.page_count() == 1500);
    const std::size_t n = doc.page_count();

    bool own_ok = true;
    const double ref_ms = best_of_3_ms([&] {
        own_ok = for_each_page_by_location(own, stext_default) && own_ok;
    });
    REQUIRE(own_ok);
    const double ms = best_of_3_ms([&] {
        for (std::size_t i = 0; i < n; ++i) (void)doc.page_text(i);
    });

    INFO("page_text " << ms << " ms against chapter-order stext " << ref_ms << " ms");
    CHECK(ms <= kCostK * ref_ms + kCostC);
    WARN("page_text " << ms << " / " << ref_ms << " ms");
}

TEST_CASE("Document chapters cost: page_hits of every page of a 1500-chapter ePub costs what loading by chapter costs",
          "[document][chapters][cost]") {
    const TempDir dir;
    const std::filesystem::path file = dir.path / "chapters-1500.epub";
    REQUIRE(write_many_chapter_epub(file, 1500));
    Document doc;
    REQUIRE_FALSE(doc.open(file).has_value());
    REQUIRE(doc.page_count() == 1500);
    const std::string file_utf8 = utf8_of(file);
    OwnDocument own(file_utf8.c_str());
    REQUIRE(own.page_count() == 1500);
    const std::size_t n = doc.page_count();

    bool own_ok = true;
    const double ref_ms = best_of_3_ms([&] {
        own_ok = for_each_page_by_location(own, stext_dehyphenate) && own_ok;
    });
    REQUIRE(own_ok);
    const double ms = best_of_3_ms([&] {
        for (std::size_t i = 0; i < n; ++i) (void)doc.page_hits(i, "Lorem", {});
    });

    INFO("page_hits " << ms << " ms against chapter-order dehyphenated stext " << ref_ms << " ms");
    CHECK(ms <= kCostK * ref_ms + kCostC);
    WARN("page_hits " << ms << " / " << ref_ms << " ms");
}

TEST_CASE("Document chapters cost: text_page of every page of a 1500-chapter ePub costs what loading by chapter costs",
          "[document][chapters][cost]") {
    const TempDir dir;
    const std::filesystem::path file = dir.path / "chapters-1500.epub";
    REQUIRE(write_many_chapter_epub(file, 1500));
    Document doc;
    REQUIRE_FALSE(doc.open(file).has_value());
    REQUIRE(doc.page_count() == 1500);
    const std::string file_utf8 = utf8_of(file);
    OwnDocument own(file_utf8.c_str());
    REQUIRE(own.page_count() == 1500);
    const std::size_t n = doc.page_count();

    bool own_ok = true;
    const double ref_ms = best_of_3_ms([&] {
        own_ok = for_each_page_by_location(own, stext_default) && own_ok;
    });
    REQUIRE(own_ok);
    bool valid_ok = true;
    const double ms = best_of_3_ms([&] {
        for (std::size_t i = 0; i < n; ++i) valid_ok = doc.text_page(i).valid() && valid_ok;
    });
    REQUIRE(valid_ok);

    INFO("text_page " << ms << " ms against chapter-order stext " << ref_ms << " ms");
    CHECK(ms <= kCostK * ref_ms + kCostC);
    WARN("text_page " << ms << " / " << ref_ms << " ms");
}

// --- the render workers' fresh copies (Task 5) --------------------------------

namespace {

using Clock = std::chrono::steady_clock;

double ms_between(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}

// The best of three readings, each from `run`, which times only its own measured
// part and returns false when the run failed.
bool best_of_3_readings(const std::function<bool(double&)>& run, double& best) {
    best = 1e300;
    for (int r = 0; r < 3; ++r) {
        double ms = 0.0;
        if (!run(ms)) return false;
        best = std::min(best, ms);
    }
    return true;
}

// The stamps of one request. Shared with its callbacks, so the worker can still
// be inside a promise's set_value after the test has seen the future.
struct Stamps {
    Clock::time_point completed;
    Clock::time_point texted;
    bool got_pixmap = false;
    std::promise<void> on_complete;
    std::promise<void> on_text;
};

// A NEW RenderEngine(doc, 1, nullptr) -- built and destroyed untimed -- and one
// request for `page` at scale 1, with on_text and text_wanted set when
// `with_text`. render_ms: submit to on_complete; text_ms: on_complete (stamped
// inside it) to on_text. False when a callback does not arrive in 10 s or the
// pixmap is null.
bool fresh_engine_request(Document& doc, int page, bool with_text, double& render_ms,
                          double& text_ms) {
    constexpr auto kWait = std::chrono::seconds(10);
    auto st = std::make_shared<Stamps>();
    auto completed = st->on_complete.get_future();
    auto texted = st->on_text.get_future();
    RenderEngine engine(doc, 1, nullptr);
    RenderEngine::RenderRequest r{page, 0, 1.0f, [st](fz_pixmap* pix, fz_context* ctx) {
        st->completed = Clock::now();
        st->got_pixmap = pix != nullptr;
        if (pix) fz_drop_pixmap(ctx, pix);
        st->on_complete.set_value();
    }};
    r.bypass_cache = true;
    if (with_text) {
        r.on_text = [st](Document::TextPage) {
            st->texted = Clock::now();
            st->on_text.set_value();
        };
        r.text_wanted = std::make_shared<const std::atomic<int>>(page);
    }
    const Clock::time_point submitted = Clock::now();
    engine.submit(std::move(r));
    if (completed.wait_for(kWait) != std::future_status::ready || !st->got_pixmap) return false;
    render_ms = ms_between(submitted, st->completed);
    if (with_text) {
        if (texted.wait_for(kWait) != std::future_status::ready) return false;
        text_ms = ms_between(st->completed, st->texted);
    }
    return true;
}

// The reference on a NEW OwnDocument of `path`: fz_count_chapters untimed (the
// engine's per-copy check reads it at construction), then, when `render_first`,
// an untimed load + render of chapter `chapter` page 0, then the timed load of
// that page by chapter location and `use` on it. False when MuPDF throws.
bool fresh_reference(const char* path, int chapter, bool render_first,
                     void (*use)(fz_context*, fz_page*), double& ms) {
    OwnDocument own(path);
    fz_context* const ctx = own.ctx;
    fz_document* const doc = own.doc;
    fz_page* page = nullptr;
    int nc = -1;
    bool ok = true;
    fz_var(page);
    fz_var(nc);
    fz_try(ctx) {
        nc = fz_count_chapters(ctx, doc);
        if (render_first) {
            page = fz_load_chapter_page(ctx, doc, chapter, 0);
            render_scale1_via_list(ctx, page);
            fz_drop_page(ctx, page);
            page = nullptr;
        }
    }
    fz_always(ctx) { fz_drop_page(ctx, page); }
    fz_catch(ctx) {
        fz_report_error(ctx);
        ok = false;
    }
    if (!ok || nc <= chapter) return false;

    page = nullptr;
    const Clock::time_point t0 = Clock::now();
    fz_try(ctx) {
        page = fz_load_chapter_page(ctx, doc, chapter, 0);
        use(ctx, page);
    }
    fz_always(ctx) { fz_drop_page(ctx, page); }
    fz_catch(ctx) {
        fz_report_error(ctx);
        ok = false;
    }
    ms = ms_between(t0, Clock::now());
    return ok;
}

}  // namespace

TEST_CASE("RenderEngine chapters cost: a fresh worker renders the last page of a 1500-chapter ePub as cheaply as loading by chapter",
          "[core][render][chapters][cost]") {
    const TempDir dir;   // first: destroyed after every document that opens the file
    const std::filesystem::path file = dir.path / "chapters-1500.epub";
    REQUIRE(write_many_chapter_epub(file, 1500));
    Document doc;
    REQUIRE_FALSE(doc.open(file).has_value());
    REQUIRE(doc.page_count() == 1500);
    const std::string file_utf8 = utf8_of(file);

    double ref_ms = 0.0;
    REQUIRE(best_of_3_readings(
        [&](double& ms) {
            return fresh_reference(file_utf8.c_str(), 1499, false, render_scale1_via_list, ms);
        },
        ref_ms));
    double ms = 0.0;
    REQUIRE(best_of_3_readings(
        [&](double& reading) {
            double unused = 0.0;
            return fresh_engine_request(doc, 1499, false, reading, unused);
        },
        ms));

    INFO("fresh worker render " << ms << " ms against load by chapter + render " << ref_ms << " ms");
    CHECK(ms <= kCostK * ref_ms + kCostC);
    WARN("fresh worker render " << ms << " / " << ref_ms << " ms");
}

TEST_CASE("RenderEngine chapters cost: a fresh worker prefetches the last page's text as cheaply as loading by chapter",
          "[core][render][chapters][cost]") {
    const TempDir dir;
    const std::filesystem::path file = dir.path / "chapters-1500.epub";
    REQUIRE(write_many_chapter_epub(file, 1500));
    Document doc;
    REQUIRE_FALSE(doc.open(file).has_value());
    REQUIRE(doc.page_count() == 1500);
    const std::string file_utf8 = utf8_of(file);

    double ref_ms = 0.0;
    REQUIRE(best_of_3_readings(
        [&](double& ms) {
            return fresh_reference(file_utf8.c_str(), 1499, true, stext_default, ms);
        },
        ref_ms));
    double ms = 0.0;
    REQUIRE(best_of_3_readings(
        [&](double& reading) {
            double unused = 0.0;
            return fresh_engine_request(doc, 1499, true, unused, reading);
        },
        ms));

    INFO("fresh worker text prefetch " << ms << " ms against load by chapter + stext " << ref_ms << " ms");
    CHECK(ms <= kCostK * ref_ms + kCostC);
    WARN("fresh worker text prefetch " << ms << " / " << ref_ms << " ms");
}
