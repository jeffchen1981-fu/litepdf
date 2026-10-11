// #98: the render worker builds a page's text on its own document after it has
// delivered a non-null pixmap, unless a skip rule says the build could delay newer
// work or is no longer wanted (spec §3.3, §5.1-1, §5.1-5).
//
// Harness. A skip-rule case parks the one worker inside the request's on_complete
// (a latch, no timing), acts as the UI thread while it is parked, then releases
// it. A negative case knows the parked request's text check has already run when
// a priority-1 sentinel, submitted while parked, completes: with one worker it
// runs only after that check has returned, and a priority-1 request does not
// trigger the queued-P0 rule. Destroying the engine is never that marker, because
// ~RenderEngine sets `stopping`, which would mask the rule under test. Every wait
// is bounded, and a scope guard releases the latch before the engine is destroyed
// so a failed REQUIRE cannot deadlock the suite on join().
#include "core/Document.hpp"
#include "core/PageCache.hpp"
#include "core/RenderEngine.hpp"

#include <catch2/catch_test_macros.hpp>

#include <windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The few C-ABI entry points the callbacks need (see test_render_engine_cache.cpp).
extern "C" {
    struct fz_context;
    struct fz_pixmap;
    void fz_drop_pixmap(fz_context*, fz_pixmap*);
    void fz_drop_context(fz_context*);
}

using litepdf::core::Document;
using litepdf::core::PageCache;
using litepdf::core::Quad;
using litepdf::core::RenderEngine;
using litepdf::core::SelectMode;
using litepdf::core::SelPoint;

namespace {

constexpr auto kWait = std::chrono::seconds(10);
constexpr int  kWordsPage = 3;   // selection.pdf's pinned-word page

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

// Centre of the single whole-word hit for `word` on `page`.
SelPoint center_of_word(const Document& doc, std::size_t page, const char* word) {
    Document::SearchFlags flags;
    flags.match_case = true;
    flags.whole_word = true;
    const auto hits = doc.page_hits(page, word, flags);
    REQUIRE(hits.size() == 1);
    const auto& h = hits[0];
    return SelPoint{ (h.ul_x + h.ur_x + h.ll_x + h.lr_x) * 0.25f,
                     (h.ul_y + h.ur_y + h.ll_y + h.lr_y) * 0.25f };
}

// --- copied from test_document_page_count.cpp ------------------------------

// Copies `fixture` to a temp file with its page tree's "/Count 1 " rewritten
// to `count`, which must be the same length so the xref offsets stay valid and
// MuPDF opens the file without repairing it. Both fixtures keep /Pages outside
// any stream, so the edit also works on the encrypted one (AES covers strings
// and streams, not the dictionary's integers).
std::filesystem::path with_page_count(const char* fixture, std::string_view count) {
    constexpr std::string_view original = "/Count 1 ";
    REQUIRE(count.size() == original.size());
    std::ifstream in(fixture, std::ios::binary);
    REQUIRE(in);
    std::string bytes((std::istreambuf_iterator<char>(in)),
                      std::istreambuf_iterator<char>());
    const auto at = bytes.find(original);
    REQUIRE(at != std::string::npos);
    REQUIRE(bytes.find(original, at + 1) == std::string::npos);
    bytes.replace(at, original.size(), count);

    static std::atomic<unsigned> seq{0};
    const auto path = std::filesystem::temp_directory_path()
        / (L"litepdf_test_bad_count_" + std::to_wstring(GetCurrentProcessId())
           + L"_" + std::to_wstring(seq.fetch_add(1)) + L".pdf");
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    REQUIRE(out);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    REQUIRE(out);
    return path;
}

struct RemoveOnExit {
    std::filesystem::path path;
    ~RemoveOnExit() { std::error_code ec; std::filesystem::remove(path, ec); }
};

// --- harness ----------------------------------------------------------------

Document open_fixture(const std::filesystem::path& path) {
    Document doc;
    REQUIRE_FALSE(doc.open(path).has_value());
    return doc;
}

// Stops the worker inside on_complete until the test releases it.
struct ParkLatch {
    std::mutex              m;
    std::condition_variable cv;
    bool                    entered = false;
    bool                    release = false;

    // Worker side, from on_complete.
    void park() {
        std::unique_lock<std::mutex> lk(m);
        entered = true;
        cv.notify_all();
        cv.wait(lk, [this] { return release; });
    }
    // Test side.
    [[nodiscard]] bool wait_entered() {
        std::unique_lock<std::mutex> lk(m);
        return cv.wait_for(lk, kWait, [this] { return entered; });
    }
    void open() {
        {
            std::lock_guard<std::mutex> g(m);
            release = true;
        }
        cv.notify_all();
    }
};

// Declared AFTER the engine, so it runs BEFORE ~RenderEngine joins: a failed
// REQUIRE while the worker is parked still lets the worker go.
struct ReleaseOnExit {
    ParkLatch& latch;
    ~ReleaseOnExit() { latch.open(); }
};

// One request's observations. Declared before the engine so the callbacks that
// write them never outlive it.
struct Probe {
    std::atomic<int>  completions{0};
    std::atomic<bool> got_pixmap{false};
    std::atomic<int>  text_calls{0};
    std::promise<void>               completed;
    std::promise<Document::TextPage> text;

    // on_complete: records whether the pixmap was non-null and drops it on the
    // worker's context (valid for the callback's duration).
    auto on_complete(ParkLatch* latch = nullptr) {
        return [this, latch](fz_pixmap* p, fz_context* ctx) {
            got_pixmap.store(p != nullptr);
            if (p) fz_drop_pixmap(ctx, p);
            if (latch) latch->park();
            if (completions.fetch_add(1) == 0) completed.set_value();
        };
    }
    std::function<void(Document::TextPage)> on_text() {
        return [this](Document::TextPage t) {
            if (text_calls.fetch_add(1) == 0) text.set_value(std::move(t));
        };
    }
};

RenderEngine::RenderRequest request(int page, int priority, Probe& probe,
                                    ParkLatch* latch, bool with_text,
                                    const std::shared_ptr<std::atomic<int>>& wanted) {
    RenderEngine::RenderRequest r;
    r.page_num    = page;
    r.priority    = priority;
    r.scale       = 1.0f;
    r.on_complete = probe.on_complete(latch);
    if (with_text) {
        r.on_text     = probe.on_text();
        r.text_wanted = wanted;
    }
    return r;
}

template <class T>
[[nodiscard]] bool ready(std::future<T>& f) {
    return f.wait_for(kWait) == std::future_status::ready;
}

bool quads_equal(const std::vector<Quad>& a, const std::vector<Quad>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const Quad& x = a[i];
        const Quad& y = b[i];
        if (x.ul_x != y.ul_x || x.ul_y != y.ul_y || x.ur_x != y.ur_x || x.ur_y != y.ur_y ||
            x.ll_x != y.ll_x || x.ll_y != y.ll_y || x.lr_x != y.lr_x || x.lr_y != y.lr_y) {
            return false;
        }
    }
    return true;
}

// Probes a worker may still be touching after a test has seen their result (a
// promise is still inside set_value when its future wakes). Declared before the
// engine, so they are destroyed only after ~RenderEngine has joined.
using Probes = std::vector<std::unique_ptr<Probe>>;

// The worker-built handle for `page` from a one-worker engine: a fresh `wanted`
// per page, no park, so nothing skips the build.
Document::TextPage worker_text(RenderEngine& engine, Probes& probes, int page) {
    auto wanted = std::make_shared<std::atomic<int>>(page);
    Probe& probe = *probes.emplace_back(std::make_unique<Probe>());
    auto text = probe.text.get_future();
    engine.submit(request(page, 0, probe, nullptr, true, wanted));
    REQUIRE(ready(text));
    return text.get();
}

void require_same_text(const Document::TextPage& w, const Document::TextPage& d) {
    REQUIRE(w.valid());
    REQUIRE(d.valid());
    REQUIRE(w.page() == d.page());

    SelPoint wf, wl, df, dl;
    const bool wr = w.full_range(wf, wl);
    const bool dr = d.full_range(df, dl);
    REQUIRE(wr == dr);
    if (!dr) return;
    CHECK(wf.x == df.x);
    CHECK(wf.y == df.y);
    CHECK(wl.x == dl.x);
    CHECK(wl.y == dl.y);
    CHECK(quads_equal(w.highlight(wf, wl), d.highlight(df, dl)));
    CHECK(w.copy(wf, wl) == d.copy(df, dl));
}

// Every listed page (all of them when `pages` is empty) of the document at
// `path`, worker handle against Document::text_page.
void compare_document(const std::filesystem::path& path, std::vector<int> pages = {},
                      std::optional<std::string_view> password = std::nullopt) {
    INFO("fixture " << path.string());
    Document doc;
    const auto err = doc.open(path);
    if (password) {
        REQUIRE(err.has_value());
        REQUIRE(*err == Document::OpenError::NeedsPassword);
        REQUIRE(doc.authenticate(*password));
    } else {
        REQUIRE_FALSE(err.has_value());
    }
    if (pages.empty()) {
        for (std::size_t p = 0; p < doc.page_count(); ++p) pages.push_back(static_cast<int>(p));
    }
    REQUIRE_FALSE(pages.empty());

    Probes probes;
    RenderEngine engine(doc, 1, nullptr, password);
    for (int p : pages) {
        INFO("page " << p);
        const auto w = worker_text(engine, probes, p);
        const auto d = doc.text_page(static_cast<std::size_t>(p));
        require_same_text(w, d);
    }
}

} // namespace

TEST_CASE("RenderEngine delivers text after a non-null pixmap",
          "[core][render][text_prefetch]") {
    Document doc = open_fixture("tests/fixtures/selection.pdf");
    auto wanted = std::make_shared<std::atomic<int>>(kWordsPage);
    Probe a;
    auto text = a.text.get_future();

    RenderEngine engine(doc, 1);
    engine.submit(request(kWordsPage, 0, a, nullptr, true, wanted));

    REQUIRE(ready(text));
    const auto t = text.get();
    CHECK(a.got_pixmap.load());
    CHECK(a.text_calls.load() == 1);
    CHECK(engine.text_builds_started() == 1);
    REQUIRE(t.valid());
    CHECK(t.page() == kWordsPage);
    SelPoint first, last;
    CHECK(t.full_range(first, last));
}

TEST_CASE("RenderEngine skips text when the page is no longer wanted",
          "[core][render][text_prefetch]") {
    Document doc = open_fixture("tests/fixtures/selection.pdf");
    auto wanted = std::make_shared<std::atomic<int>>(kWordsPage);
    ParkLatch latch;
    Probe a, s;
    auto s_done = s.completed.get_future();

    RenderEngine engine(doc, 1);
    const ReleaseOnExit guard{latch};
    engine.submit(request(kWordsPage, 0, a, &latch, true, wanted));
    REQUIRE(latch.wait_entered());

    engine.submit(request(kWordsPage, 1, s, nullptr, false, nullptr));
    wanted->store(-1, std::memory_order_relaxed);
    latch.open();
    REQUIRE(ready(s_done));

    CHECK(a.got_pixmap.load());
    CHECK(a.text_calls.load() == 0);
    CHECK(engine.text_builds_started() == 0);
}

TEST_CASE("RenderEngine skips text when a newer P0 is queued",
          "[core][render][text_prefetch]") {
    Document doc = open_fixture("tests/fixtures/selection.pdf");
    auto wanted = std::make_shared<std::atomic<int>>(kWordsPage);
    ParkLatch latch;
    Probe a, b;
    auto b_text = b.text.get_future();

    RenderEngine engine(doc, 1);
    const ReleaseOnExit guard{latch};
    engine.submit(request(kWordsPage, 0, a, &latch, true, wanted));
    REQUIRE(latch.wait_entered());

    engine.submit(request(kWordsPage, 0, b, nullptr, true, wanted));
    latch.open();
    REQUIRE(ready(b_text));

    CHECK(a.got_pixmap.load());
    CHECK(a.text_calls.load() == 0);
    CHECK(b.text_calls.load() == 1);
    CHECK(b_text.get().page() == kWordsPage);
    CHECK(engine.text_builds_started() == 1);
}

TEST_CASE("RenderEngine skips text when the request is cancelled after its pixmap",
          "[core][render][text_prefetch]") {
    Document doc = open_fixture("tests/fixtures/selection.pdf");
    auto wanted = std::make_shared<std::atomic<int>>(kWordsPage);
    auto cancel_flag = std::make_shared<std::atomic<bool>>(false);
    ParkLatch latch;
    Probe a, s;
    auto s_done = s.completed.get_future();

    RenderEngine engine(doc, 1);
    const ReleaseOnExit guard{latch};
    auto req = request(kWordsPage, 0, a, &latch, true, wanted);
    req.cancel_flag = cancel_flag;
    engine.submit(std::move(req));
    REQUIRE(latch.wait_entered());

    engine.submit(request(kWordsPage, 1, s, nullptr, false, nullptr));
    cancel_flag->store(true);
    latch.open();
    REQUIRE(ready(s_done));

    CHECK(a.got_pixmap.load());
    CHECK(a.text_calls.load() == 0);
    CHECK(engine.text_builds_started() == 0);
}

TEST_CASE("RenderEngine skips text once shutdown has begun",
          "[core][render][text_prefetch]") {
    Document doc = open_fixture("tests/fixtures/selection.pdf");
    auto wanted = std::make_shared<std::atomic<int>>(kWordsPage);
    ParkLatch latch;
    Probe a, b, s;
    auto s_done = s.completed.get_future();

    RenderEngine engine(doc, 1);
    const ReleaseOnExit guard{latch};
    engine.submit(request(kWordsPage, 0, a, &latch, true, wanted));
    REQUIRE(latch.wait_entered());

    // B is still wanted, S is not a P0 and nothing is cancelled: only the
    // stopping rule can keep B from building.
    engine.submit(request(kWordsPage, 0, b, nullptr, true, wanted));
    engine.submit(request(kWordsPage, 1, s, nullptr, false, nullptr));
    engine.begin_shutdown();
    engine.begin_shutdown();  // idempotent
    latch.open();
    REQUIRE(ready(s_done));   // the worker drains the queue after begin_shutdown

    CHECK(a.got_pixmap.load());
    CHECK(b.completions.load() == 1);
    CHECK(b.got_pixmap.load());
    CHECK(a.text_calls.load() == 0);
    CHECK(b.text_calls.load() == 0);
    CHECK(engine.text_builds_started() == 0);
}

TEST_CASE("RenderEngine delivers text on a pixmap cache hit",
          "[core][render][text_prefetch]") {
    Document doc = open_fixture("tests/fixtures/selection.pdf");
    fz_context* cache_ctx = doc.clone_context();
    REQUIRE(cache_ctx);
    constexpr float scale = 1.0f;
    {
        PageCache cache(5, 10, cache_ctx);
        auto wanted = std::make_shared<std::atomic<int>>(kWordsPage);
        Probe warm, a;
        auto warm_done = warm.completed.get_future();
        auto text = a.text.get_future();

        RenderEngine engine(doc, 1, &cache);
        engine.submit(request(kWordsPage, 0, warm, nullptr, false, nullptr));
        REQUIRE(ready(warm_done));
        REQUIRE(warm.got_pixmap.load());

        // Non-vacuity: the next request is an L1 hit.
        fz_pixmap* hit = cache.get_pixmap(kWordsPage, scale);
        REQUIRE(hit != nullptr);
        fz_drop_pixmap(cache_ctx, hit);

        engine.submit(request(kWordsPage, 0, a, nullptr, true, wanted));
        REQUIRE(ready(text));
        const auto t = text.get();
        CHECK(a.got_pixmap.load());
        CHECK(a.text_calls.load() == 1);
        REQUIRE(t.valid());
        CHECK(t.page() == kWordsPage);
    }
    fz_drop_context(cache_ctx);
}

TEST_CASE("RenderEngine sends no text for a request cancelled before submit",
          "[core][render][text_prefetch]") {
    Document doc = open_fixture("tests/fixtures/selection.pdf");
    auto wanted = std::make_shared<std::atomic<int>>(kWordsPage);
    auto cancel_flag = std::make_shared<std::atomic<bool>>(true);
    Probe a, s;
    auto a_done = a.completed.get_future();
    auto s_done = s.completed.get_future();

    RenderEngine engine(doc, 1);
    auto req = request(kWordsPage, 0, a, nullptr, true, wanted);
    req.cancel_flag = cancel_flag;
    engine.submit(std::move(req));
    // The P1 sentinel runs after A's whole iteration, text check included.
    engine.submit(request(kWordsPage, 1, s, nullptr, false, nullptr));
    REQUIRE(ready(a_done));
    REQUIRE(ready(s_done));

    CHECK_FALSE(a.got_pixmap.load());
    CHECK(a.text_calls.load() == 0);
    CHECK(engine.text_builds_started() == 0);
}

TEST_CASE("TextPrefetch worker text equals Document text",
          "[core][render][text_prefetch]") {
    for (const char* path : {
             "tests/fixtures/selection.pdf",
             "tests/fixtures/search.pdf",
             "tests/fixtures/simple.pdf",
             "tests/fixtures/bookmarks.pdf",
             "tests/fixtures/cjk-ja.pdf",
             "tests/fixtures/cjk-ko.pdf",
             "tests/fixtures/cjk-zh-hant.pdf",
             "tests/fixtures/type3.pdf",
             "tests/fixtures/annot.pdf",
             "tests/fixtures/sample.epub",
             "tests/fixtures/sample.cbz",
         }) {
        compare_document(path);
    }

    {
        Document large = open_fixture("tests/fixtures/large.pdf");
        const int n = static_cast<int>(large.page_count());
        REQUIRE(n > 2);
        compare_document("tests/fixtures/large.pdf", {0, n / 2, n - 1});
    }

    // #119's /Count-inflated copy of simple.pdf.
    {
        const RemoveOnExit file{with_page_count("tests/fixtures/simple.pdf", "/Count 2 ")};
        compare_document(file.path);
    }

    // Ruling R1: worker documents authenticate (#126), so a user-password PDF's
    // worker text must equal the authenticated Document's.
    compare_document("tests/fixtures/encrypted-text.pdf", {}, "test");
    compare_document("tests/fixtures/encrypted.pdf", {}, "test");

    // An interior selection on selection.pdf page 3: the whole-page range alone
    // would not notice a difference in word or line segmentation.
    Document doc = open_fixture("tests/fixtures/selection.pdf");
    Probes probes;
    RenderEngine engine(doc, 1);
    const auto w = worker_text(engine, probes, kWordsPage);
    const auto d = doc.text_page(kWordsPage);
    REQUIRE(w.valid());
    REQUIRE(d.valid());
    const SelPoint alpha = center_of_word(doc, kWordsPage, "alpha");
    const SelPoint beta  = center_of_word(doc, kWordsPage, "beta");
    const SelPoint gamma = center_of_word(doc, kWordsPage, "gamma");
    const std::pair<SelPoint, SelPoint> pairs[] = { {alpha, gamma}, {beta, beta} };
    for (const auto& [a, b] : pairs) {
        for (SelectMode mode : {SelectMode::Chars, SelectMode::Words, SelectMode::Lines}) {
            INFO("mode " << static_cast<int>(mode));
            const auto ws = w.snap(a, b, mode);
            const auto ds = d.snap(a, b, mode);
            CHECK(ws.a.x == ds.a.x);
            CHECK(ws.a.y == ds.a.y);
            CHECK(ws.b.x == ds.b.x);
            CHECK(ws.b.y == ds.b.y);
            CHECK(quads_equal(w.highlight(ws.a, ws.b), d.highlight(ds.a, ds.b)));
            const auto wc = w.copy(ws.a, ws.b);
            CHECK(wc == d.copy(ds.a, ds.b));
            // Non-vacuity: a Words or Lines snap of either pair selects at least
            // the word under the point (a Chars pair (beta, beta) selects nothing).
            if (mode != SelectMode::Chars) CHECK_FALSE(without_whitespace(wc).empty());
        }
    }
}
