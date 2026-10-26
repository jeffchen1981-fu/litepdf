// The #146 rule outside Document: a catch that swallows a MuPDF failure must
// also mark the error handled. MuPDF keeps a caught error's code on the context
// until a later catch overwrites it; a throw that comes first, or the context's
// fz_drop_context, reports the old error again as "UNHANDLED EXCEPTION!"
// (error.c, fz_vthrow; context.c, fz_drop_context).
//
// Each case installs a warning callback on the context the failed call ran on
// and throws on it straight after, with no MuPDF try or throw in between.
#include "core/Document.hpp"
#include "core/RenderEngine.hpp"
#include "core/SystemFonts.hpp"

#include <catch2/catch_test_macros.hpp>

#include <mupdf/fitz.h>

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <string>

using namespace litepdf::core;

namespace {

constexpr const char* kUnhandled = "UNHANDLED EXCEPTION!";

// Throws and catches a fresh error on ctx and returns the warnings MuPDF raised
// meanwhile: a stale code left by the call before it is reported as
// "UNHANDLED EXCEPTION!". Restores ctx's previous warning callback.
std::string warnings_from_probe_throw(fz_context* ctx) {
    std::string text;
    void* prev_user = nullptr;
    fz_warning_cb* prev = fz_warning_callback(ctx, &prev_user);
    fz_set_warning_callback(ctx, [](void* user, const char* message) noexcept {
        *static_cast<std::string*>(user) += std::string(message) + "\n";
    }, &text);
    fz_try(ctx) {
        fz_throw(ctx, FZ_ERROR_GENERIC, "error-state probe");
    }
    fz_catch(ctx) {
        fz_convert_error(ctx, nullptr);
    }
    fz_set_warning_callback(ctx, prev, prev_user);
    return text;
}

// Submits one request with no cache, and probes the worker's context from
// on_complete, the first callback to run after the worker's catch. Returns the
// probe's warnings; the request must fail (a null pixmap).
std::string probe_after_failed_render(int page, float scale) {
    // Declared before the engine: if the wait below fails, ~RenderEngine still
    // drains the queue and runs on_complete, which must find these alive.
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    bool got_pixmap = false;
    std::string warnings;

    Document doc;
    REQUIRE_FALSE(doc.open("tests/fixtures/simple.pdf").has_value());
    RenderEngine engine(doc, 1);

    RenderEngine::RenderRequest req{};
    req.page_num = page;
    req.scale = scale;
    req.on_complete = [&](fz_pixmap* pix, fz_context* ctx) {
        std::string text = warnings_from_probe_throw(ctx);
        if (pix) fz_drop_pixmap(ctx, pix);
        std::lock_guard<std::mutex> g(m);
        got_pixmap = pix != nullptr;
        warnings = std::move(text);
        done = true;
        cv.notify_all();
    };
    engine.submit(std::move(req));

    std::unique_lock<std::mutex> lk(m);
    REQUIRE(cv.wait_for(lk, std::chrono::seconds(10), [&] { return done; }));
    REQUIRE_FALSE(got_pixmap);
    return warnings;
}

// An allocator that fails on request, so the base14 last resort fails.
struct FailingAlloc {
    bool fail = false;
};

void* failing_malloc(void* user, size_t size) {
    return static_cast<FailingAlloc*>(user)->fail ? nullptr : std::malloc(size);
}
void* failing_realloc(void* user, void* old, size_t size) {
    return static_cast<FailingAlloc*>(user)->fail ? nullptr : std::realloc(old, size);
}
void failing_free(void*, void* ptr) { std::free(ptr); }

}  // namespace

TEST_CASE("RenderEngine error state: failed page load is marked handled",
          "[core][render][error-state]") {
    const std::string warnings = probe_after_failed_render(9999, 1.0f);

    INFO("warnings:\n" << warnings);
    REQUIRE(warnings.find(kUnhandled) == std::string::npos);
}

TEST_CASE("RenderEngine error state: failed rasterise is marked handled",
          "[core][render][error-state]") {
    // The page bbox clamps to MuPDF's 2^24 safe-int limit, so the pixmap
    // asks for petabytes and its allocation fails; the page load before it
    // succeeds.
    const std::string warnings = probe_after_failed_render(0, 1.0e5f);

    INFO("warnings:\n" << warnings);
    REQUIRE(warnings.find(kUnhandled) == std::string::npos);
}

TEST_CASE("SystemFonts error state: failed last-resort font is marked handled",
          "[core][fonts][cjk][error-state]") {
    FailingAlloc state;
    fz_alloc_context alloc = {&state, failing_malloc, failing_realloc, failing_free};
    fz_context* ctx = fz_new_context(&alloc, nullptr, FZ_STORE_UNLIMITED);
    REQUIRE(ctx != nullptr);

    // Ordering 99 matches no family, so the hook goes straight to the base14
    // last resort, whose allocation fails. Called directly, as a caller that
    // does not wrap the hook in its own fz_try would.
    state.fail = true;
    fz_font* font = resolve_cjk_system_font(ctx, "SourceHanSerif", 99, 1);
    state.fail = false;
    REQUIRE(font == nullptr);

    const std::string warnings = warnings_from_probe_throw(ctx);
    fz_drop_context(ctx);

    INFO("warnings:\n" << warnings);
    REQUIRE(warnings.find(kUnhandled) == std::string::npos);
}
