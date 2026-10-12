// Tests for core::GenerationAbort — the per-generation abort token used by
// SearchSession to cancel a superseded search mid-page.
//
// Background: each search "generation" (one set_query) owns a distinct
// fz_cookie (#130). Starting the next generation must set the PREVIOUS
// generation's cookie `abort` so an in-flight scan holding it stops its text
// build at MuPDF's next poll instead of building the now-stale page to
// completion. A single shared cookie can't do this — MuPDF forbids resetting
// `abort` once set, and the next generation would need it at 0 (the bug this
// type fixes).
//
// The scan treats the cookie as: abort == 0 = keep scanning, non-zero = stop
// and return whatever hits were collected so far.

#include "core/GenerationAbort.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace litepdf::core;

TEST_CASE("GenerationAbort: a fresh generation is not aborted",
          "[search][abort]") {
    GenerationAbort g;
    auto tok = g.begin_generation();
    REQUIRE(tok != nullptr);
    REQUIRE_FALSE(detail::abort_requested(*tok));   // live generation keeps scanning
}

TEST_CASE("GenerationAbort: a fresh cookie has no progress",
          "[search][abort]") {
    // The cookie is value-initialised: MuPDF reads `progress`, `abort` and the
    // rest of its fields, so none may start as garbage.
    GenerationAbort g;
    auto tok = g.begin_generation();
    REQUIRE(tok != nullptr);
    REQUIRE((tok->progress == 0 && tok->abort == 0));
}

TEST_CASE("GenerationAbort: starting the next generation aborts the previous",
          "[search][abort]") {
    GenerationAbort g;
    auto first  = g.begin_generation();
    auto second = g.begin_generation();

    REQUIRE(detail::abort_requested(*first));         // previous generation told to bail mid-page
    REQUIRE_FALSE(detail::abort_requested(*second));  // new generation keeps scanning
}

TEST_CASE("GenerationAbort: each generation gets a distinct token, not a shared flag",
          "[search][abort]") {
    GenerationAbort g;
    auto first  = g.begin_generation();
    auto second = g.begin_generation();
    REQUIRE(first != second);
}

TEST_CASE("GenerationAbort: a superseded token stays alive and reads as aborted",
          "[search][abort]") {
    // Models an in-flight worker that captured generation N's cookie before
    // generation N+1 started: the worker's shared_ptr keeps the cookie alive,
    // and it observes the abort even though GenerationAbort moved on.
    GenerationAbort g;
    auto worker_token = g.begin_generation();
    g.begin_generation();          // worker_token is now superseded
    REQUIRE(detail::abort_requested(*worker_token));
}

TEST_CASE("GenerationAbort: abort_active signals the live generation (teardown)",
          "[search][abort]") {
    GenerationAbort g;
    auto tok = g.begin_generation();
    g.abort_active();
    REQUIRE(detail::abort_requested(*tok));
}

TEST_CASE("GenerationAbort: abort_active before any generation is a safe no-op",
          "[search][abort]") {
    GenerationAbort g;
    g.abort_active();              // no live generation yet
    SUCCEED("no crash with no active generation");
}
