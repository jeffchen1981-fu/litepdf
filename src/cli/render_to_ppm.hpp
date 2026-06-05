#pragma once

// render_page_to_ppm — testable extraction of litepdf-cli's `--render` path.
//
// Renders one page via a fresh single-worker RenderEngine and writes the
// result as a binary PPM (P6) image to `out`. Pulled out of main() so a unit
// test can drive the bounded-wait + engine-teardown sequence directly (main()
// itself is not unit-testable), and to make the teardown-safety guarantee
// explicit and tested. See tests/unit/test_cli_render_teardown.cpp.

#include <chrono>
#include <cstdio>

namespace litepdf::core { class Document; }

namespace litepdf::cli {

// Render `page` of `doc` and write it to `out` as a binary PPM (P6). Blocks up
// to `timeout` for the render to complete.
//
// Returns: 0 = success, 2 = render produced no pixmap, 3 = timed out.
//          (Matches litepdf-cli's historical --render exit codes.)
//
// Teardown-safe by construction: the wait/result state is heap-allocated and
// co-owned (std::shared_ptr) by the worker callback, so a render that only
// finishes inside ~RenderEngine() (the timeout path drains the still-in-flight
// job) writes into live state regardless of stack-destruction order. Capturing
// stack locals by reference here would be a use-after-free, because the engine
// destructor outlives those locals and can still fire the callback.
int render_page_to_ppm(litepdf::core::Document& doc, int page, std::FILE* out,
                       std::chrono::milliseconds timeout);

} // namespace litepdf::cli
