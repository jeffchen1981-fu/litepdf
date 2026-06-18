// Tests for core::GenerationAbort — the per-generation abort token used by
// SearchSession to cancel a superseded search mid-page.
//
// Background: each search "generation" (one set_query) owns a distinct abort
// token. Starting the next generation must flip the PREVIOUS generation's
// token to non-zero so an in-flight Document::page_hits() worker holding it
// bails mid-page instead of scanning the now-stale page to completion. A
// single shared flag can't do this — the next generation would reset it to 0
// and strand the old worker (the bug this type fixes).
//
// page_hits() treats the token as: 0 = keep scanning, non-zero = abort and
// return whatever hits were collected so far.

#include "core/GenerationAbort.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace litepdf::core;

TEST_CASE("GenerationAbort: a fresh generation is not aborted",
          "[search][abort]") {
    GenerationAbort g;
    auto tok = g.begin_generation();
    REQUIRE(tok != nullptr);
    REQUIRE(tok->load() == 0);   // live generation keeps scanning
}

TEST_CASE("GenerationAbort: starting the next generation aborts the previous",
          "[search][abort]") {
    GenerationAbort g;
    auto first  = g.begin_generation();
    auto second = g.begin_generation();

    REQUIRE(first->load() != 0);   // previous generation told to bail mid-page
    REQUIRE(second->load() == 0);  // new generation keeps scanning
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
    // Models an in-flight worker that captured generation N's token before
    // generation N+1 started: the worker's shared_ptr keeps the atomic alive,
    // and it observes the abort even though GenerationAbort moved on.
    GenerationAbort g;
    auto worker_token = g.begin_generation();
    g.begin_generation();          // worker_token is now superseded
    REQUIRE(worker_token->load() != 0);
}

TEST_CASE("GenerationAbort: abort_active signals the live generation (teardown)",
          "[search][abort]") {
    GenerationAbort g;
    auto tok = g.begin_generation();
    g.abort_active();
    REQUIRE(tok->load() != 0);
}

TEST_CASE("GenerationAbort: abort_active before any generation is a safe no-op",
          "[search][abort]") {
    GenerationAbort g;
    g.abort_active();              // no live generation yet
    SUCCEED("no crash with no active generation");
}
