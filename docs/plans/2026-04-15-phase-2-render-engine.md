# Phase 2: RenderEngine + PageCache — Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use `superpowers:subagent-driven-development` to execute this plan task-by-task, same way Phase 1 was executed. This phase is TDD-heavy — every worker primitive, cache method, and cancellation path starts with a failing Catch2 test.

**Goal:** Build a UI-agnostic `core::RenderEngine` (fixed-size thread pool + priority queue + cancellation) and `core::PageCache` (two-tier LRU over MuPDF's `fz_display_list` and `fz_pixmap`) that together rasterize pages off the UI thread with a per-tab memory budget. No Direct2D, no Win32 — exit criterion is a headless stress test of 1000 sequential renders under 25 MB RAM cap.

**Architecture:** `RenderEngine` owns N cloned `fz_context` pointers (one per worker thread — MuPDF contexts are **not** thread-safe). Callers `submit(RenderRequest)` and get back a `RenderToken` they can `cancel()`. Workers pull from a priority-ordered queue, consult `PageCache` for a cached display list or pixmap, and fall through to `fz_load_page`/`fz_new_display_list_from_page`/`fz_new_pixmap_from_display_list` on miss. Completion fires a caller-supplied `std::function<void(fz_pixmap*)>` **on the worker thread** — Phase 3 UI code will `PostMessage` from inside that callback to marshal back to the UI thread. `PageCache` is two independent LRUs keyed by page number (L2: display list) and `(page, scale)` (L1: pixmap), with explicit refcount discipline.

**Tech Stack:** C++17 (std::thread, std::condition_variable, std::atomic, std::priority_queue), MuPDF 1.24.11 (same submodule from Phase 1), Catch2 v3 (same FetchContent), CMake 3.25+. No UI libraries.

**Prerequisites (from Phase 1):**

- Tag `v0.0.2-phase1.1` on `main`; CI green.
- `litepdf_core` static library exports `core::Document` with public `fz_context*` accessor (or we add one in Task 1).
- `tests/fixtures/simple.pdf` usable for render smoke tests.
- MuPDF CRT unification (`/MT`) patched in at CMake configure time.

**Done when:**

1. `cmake --build build --config Release` produces `litepdf.exe`, `litepdf-cli.exe`, `litepdf_unit_tests.exe` clean.
2. `ctest --test-dir build --config Release` passes all Phase 1 + new Phase 2 tests (~25+ new).
3. Stress test `test_render_stress` renders 1000 pages from `simple.pdf` (looping over 8 pages × 125 iterations) and asserts peak-RSS stays under 25 MB **over and above** the baseline measured at the start of the test.
4. CLI gains a `--render N` flag: `litepdf-cli tests/fixtures/simple.pdf --render 0` writes a PPM to stdout as a manual smoke check.
5. CI runs all of the above green on `windows-latest`.
6. Tag `v0.0.3-phase2` pushed.

**Learnings carried from Phase 1:**

- `fz_context` is **not thread-safe**; must clone per worker via `fz_clone_context`.
- MuPDF objects (`fz_page`, `fz_display_list`, `fz_pixmap`) are refcounted; `fz_keep_X`/`fz_drop_X` must balance. Tests should sanity-check refcount leaks where feasible.
- `std::swap` move idiom from `Document` works well when a type holds a PIMPL; reuse for non-copyable engine types.
- Subagent-driven workflow caught 5 plan deviations in Phase 1 — keep using it, with two-stage review per task.
- `RuntimeLibrary=MultiThreaded` propagation to FetchContent still required (Catch2 will re-use Phase 1's `CMAKE_MSVC_RUNTIME_LIBRARY`).

---

## Architectural Decisions

Before tasks, pin the decisions that govern the whole phase:

**D1. Context cloning lifecycle.** `RenderEngine` is constructed with a reference to a `core::Document`. At construction, it calls `Document::clone_context()` N times (once per worker). Workers use their own context for all MuPDF calls. Destructor joins workers, then `fz_drop_context` each clone. Document's context is **never** touched from worker threads. Rationale: Phase 1 deferred item; documented in MuPDF `docs/thread.html`.

**D2. Public callback fires on worker thread, with worker ctx.** `RenderRequest::on_complete(fz_pixmap*, fz_context*)` runs in the worker that completed the render. The ctx passed in is the **worker's clone** (safe to use *only* for the duration of the callback, on this thread). Caller **takes ownership** of the pixmap and is expected to either (a) `fz_keep_pixmap(ctx, p)` to extend lifetime, or (b) `fz_drop_pixmap(ctx, p)` to release. Rationale: avoids an extra queue + extra lock; Phase 3 UI code also needs a ctx for Direct2D upload / colorspace / keep, so we pay this API cost once here rather than changing signatures in Phase 3. MuPDF's D1 invariant is preserved — the ctx never escapes the worker thread.

**D3. Cancellation is cooperative and coarse-grained.** Worker checks `token.canceled->load()` at three points: (a) before dequeue commits, (b) after `fz_load_page` returns, (c) after display list built but before rasterize. MuPDF ops themselves are not interrupted mid-call. Rationale: typical single-page operations are < 200 ms on the HDD target; a three-checkpoint cancel is responsive enough for `PgDn`-held-down scrubbing. No custom `fz_try` long-jmp gymnastics.

**D4. Priority queue is stable.** Tasks with equal priority run FIFO. Implemented by tagging every submission with a monotonically-increasing sequence number and comparing `(priority, seq)`. Rationale: avoids starvation of older P2 tasks when new P2 tasks arrive.

**D5. PageCache is owned per-`Document`/per-tab.** Phase 2 creates `RenderEngine` and `PageCache` as separate objects; Phase 3's `DocumentView` (per tab) will own one of each. `PageCache` is constructed with explicit capacities: L1 = 5 pixmaps, L2 = 10 display lists (per design §5.2). Cache methods are mutex-protected — multiple workers may race on the same cache.

**D6. L1 key is `(page_num, scale)`.** Same page rasterized at two zooms is two L1 entries (bounded by capacity). L2 is keyed by `page_num` only because display lists are zoom-independent.

**D7. Memory cap enforcement is reactive, not predictive.** Eviction happens when inserting a new entry pushes the LRU past capacity — same number of entries regardless of pixmap size. Peak RSS is bounded because A4 @ 150 dpi ≈ 4 MB and capacity × avg size = 5 × 4 + 10 × 0.3 ≈ 23 MB, headroom within the 25 MB target.

**D8. No Direct2D in Phase 2.** L1 stores `fz_pixmap*`. Phase 3 will either (a) swap L1 to `ID2D1Bitmap*`, or (b) keep pixmap cache and add a sibling `D2DBitmapCache`. Decision deferred to Phase 3 kickoff.

**D9. Headless stress-test peak-RSS measurement.** Use `GetProcessMemoryInfo(GetCurrentProcess(), ...)::PeakWorkingSetSize`. Take baseline before the loop, take final after, assert `final - baseline < 25 MB`. On Windows, `psapi.h` + `psapi.lib`. Rationale: Phase 2 is headless and Windows-only, so no portable-abstraction layer needed.

---

## Tasks

### Task 1: Add `Document::clone_context()` accessor + allocator-shared base

**Why this first:** RenderEngine can't start until Document can hand out context clones.

**Files:**
- Modify: `src/core/Document.hpp` — add public method
- Modify: `src/core/Document.cpp` — implement, protect against null
- Test: `tests/unit/test_document_clone_context.cpp` — new

**Step 1: Write the failing test**

```cpp
TEST_CASE("Document::clone_context returns distinct fz_context per call", "[core][document][clone]") {
    litepdf::core::Document doc;
    REQUIRE(doc.open("tests/fixtures/simple.pdf"));
    fz_context* c1 = doc.clone_context();
    fz_context* c2 = doc.clone_context();
    REQUIRE(c1 != nullptr);
    REQUIRE(c2 != nullptr);
    REQUIRE(c1 != c2);
    fz_drop_context(c1);
    fz_drop_context(c2);
}

TEST_CASE("Document::clone_context on un-opened doc returns nullptr", "[core][document][clone]") {
    litepdf::core::Document doc;
    REQUIRE(doc.clone_context() == nullptr);
}
```

**Step 2: Run, expect link error / method missing**

`ctest -R test_document_clone_context`

**Step 3: Implement**

```cpp
// Document.hpp (public section)
// Opaque to callers — requires fitz.h to use. Only RenderEngine cares.
struct fz_context_s;
typedef struct fz_context_s fz_context;
fz_context* clone_context() const;
```

```cpp
// Document.cpp
fz_context* Document::clone_context() const {
    if (!impl_->ctx) return nullptr;
    return fz_clone_context(impl_->ctx);
}
```

**Step 4: Run, expect pass**

**Step 5: Commit**

```
feat(core): Document::clone_context() for thread-pool workers (TDD)
```

---

### Task 2: Scaffold `RenderEngine` class (non-functional skeleton)

**Files:**
- Create: `src/core/RenderEngine.hpp`
- Create: `src/core/RenderEngine.cpp`
- Modify: `CMakeLists.txt` — add sources to `litepdf_core`
- Test: `tests/unit/test_render_engine_lifecycle.cpp`

**Step 1: Write header with the full public API (no implementation)**

```cpp
// RenderEngine.hpp
#pragma once
#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>

struct fz_pixmap_s;
typedef struct fz_pixmap_s fz_pixmap;

namespace litepdf::core {
class Document;

class RenderEngine {
public:
    struct RenderToken {
        std::size_t id = 0;
        std::shared_ptr<std::atomic<bool>> canceled;
    };

    struct RenderRequest {
        int page_num;
        int priority;       // 0 = highest (current page); 1 = adjacent; 2 = prefetch
        float scale;        // 1.0 = 72 dpi; 2.08 ≈ 150 dpi
        std::function<void(fz_pixmap*, fz_context*)> on_complete;  // on worker thread; ctx valid only during this call
    };

    RenderEngine(Document& doc, std::size_t num_workers = 2);
    ~RenderEngine();

    RenderEngine(const RenderEngine&) = delete;
    RenderEngine& operator=(const RenderEngine&) = delete;
    RenderEngine(RenderEngine&&) = delete;
    RenderEngine& operator=(RenderEngine&&) = delete;

    RenderToken submit(RenderRequest req);
    void cancel(const RenderToken& token);
    void cancel_all_below_priority(int priority);

    std::size_t num_workers() const;
    std::size_t pending_count() const;  // for tests

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
```

**Step 2: Write the failing lifecycle test**

```cpp
TEST_CASE("RenderEngine starts and stops cleanly with default workers", "[core][render][lifecycle]") {
    litepdf::core::Document doc;
    REQUIRE(doc.open("tests/fixtures/simple.pdf"));
    {
        litepdf::core::RenderEngine engine(doc);
        REQUIRE(engine.num_workers() == 2);
    }  // destructor joins threads
    SUCCEED();
}

TEST_CASE("RenderEngine respects custom worker count", "[core][render][lifecycle]") {
    litepdf::core::Document doc;
    REQUIRE(doc.open("tests/fixtures/simple.pdf"));
    litepdf::core::RenderEngine engine(doc, 4);
    REQUIRE(engine.num_workers() == 4);
}
```

**Step 3: Stub `.cpp` so the class links but does nothing**

```cpp
struct RenderEngine::Impl { std::size_t n; };
RenderEngine::RenderEngine(Document&, std::size_t n) : impl_(std::make_unique<Impl>()) { impl_->n = n; }
RenderEngine::~RenderEngine() = default;
std::size_t RenderEngine::num_workers() const { return impl_->n; }
std::size_t RenderEngine::pending_count() const { return 0; }
RenderEngine::RenderToken RenderEngine::submit(RenderRequest) { return {}; }
void RenderEngine::cancel(const RenderToken&) {}
void RenderEngine::cancel_all_below_priority(int) {}
```

**Step 4: Run, expect pass**

**Step 5: Commit** — `feat(core): scaffold RenderEngine (PIMPL, API only)`

---

### Task 3: Thread pool start/stop

**Files:**
- Modify: `src/core/RenderEngine.cpp`
- Test: extend `test_render_engine_lifecycle.cpp`

**Step 1: Test — workers actually start and join**

```cpp
TEST_CASE("RenderEngine workers are joinable and shut down on destruction", "[core][render][pool]") {
    litepdf::core::Document doc;
    REQUIRE(doc.open("tests/fixtures/simple.pdf"));
    std::atomic<int> ticks{0};
    {
        litepdf::core::RenderEngine engine(doc, 2);
        // Workers should be idle, not burning CPU; let them run briefly.
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    SUCCEED();  // test passes if destructor returns within test timeout
}
```

**Step 2: Implement**

```cpp
struct RenderEngine::Impl {
    Document* doc;
    std::size_t n;
    std::vector<std::thread> workers;
    std::vector<fz_context*> worker_ctxs;
    std::mutex mtx;
    std::condition_variable cv;
    bool stopping = false;
    // (queue added in Task 4)
    void worker_loop(std::size_t worker_idx);
};
```

Constructor: clone N contexts from `doc.clone_context()`, spawn N threads running `worker_loop`. Destructor: `stopping = true; cv.notify_all(); join all; fz_drop_context` each.

`worker_loop` stub: `while (true) { unique_lock lk(mtx); cv.wait(lk, [&]{ return stopping; }); if (stopping) return; }`

**Step 3: Run, pass. Commit** — `feat(core): RenderEngine thread pool start/stop`

---

### Task 4: `submit()` enqueues, worker dequeues (no rendering yet)

**Files:**
- Modify: `src/core/RenderEngine.cpp`
- Test: `tests/unit/test_render_engine_submit.cpp`

**Step 1: Test — submission executes a no-op callback**

```cpp
TEST_CASE("RenderEngine::submit runs the callback exactly once", "[core][render][submit]") {
    litepdf::core::Document doc;
    REQUIRE(doc.open("tests/fixtures/simple.pdf"));
    litepdf::core::RenderEngine engine(doc, 1);
    std::atomic<int> calls{0};
    engine.submit({0, 0, 1.0f, [&](fz_pixmap* p, fz_context* ctx){
        ++calls;
        if (p) fz_drop_pixmap(ctx, p);
    }});
    // Wait up to 2s for async completion
    for (int i = 0; i < 200 && calls.load() == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    REQUIRE(calls.load() == 1);
}
```

**Step 2: Implement a minimal queue + pop loop; callback passed nullptr for now**

Add `std::queue<RenderRequest>` (simple FIFO, not priority yet) to `Impl`. `submit` pushes, notifies. `worker_loop` pops, calls `req.on_complete(nullptr)`.

**Step 3: Run, pass. Commit** — `feat(core): RenderEngine submit + worker dequeue (callback wired)`

---

### Task 5: Priority queue with FIFO tiebreaker

**Files:**
- Modify: `src/core/RenderEngine.cpp`
- Test: `tests/unit/test_render_engine_priority.cpp`

**Step 1: Test — P0 submitted after P2 still completes first**

```cpp
TEST_CASE("RenderEngine runs higher-priority tasks before lower", "[core][render][priority]") {
    litepdf::core::Document doc;
    REQUIRE(doc.open("tests/fixtures/simple.pdf"));
    litepdf::core::RenderEngine engine(doc, 1);  // single worker to force serialization
    std::vector<int> order;
    std::mutex m;
    auto push = [&](int tag, int prio) {
        engine.submit({tag, prio, 1.0f, [&, tag](fz_pixmap* p, fz_context* ctx){
            { std::lock_guard g(m); order.push_back(tag); }
            if (p) fz_drop_pixmap(ctx, p);
        }});
    };
    // Block the worker briefly with a first task so we can stack P2 then P0.
    push(99, 2);  // gate
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    push(2, 2);
    push(0, 0);
    push(1, 1);
    // Wait for drain
    for (int i = 0; i < 200 && order.size() < 4; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    REQUIRE(order.size() == 4);
    // After the gate, order should be P0 before P1 before P2.
    REQUIRE(order == std::vector<int>{99, 0, 1, 2});
}
```

**Step 2: Replace `std::queue` with `std::priority_queue` over `(priority, seq, request)`**

Comparator: lower priority wins; smaller seq wins on tie. Add `std::atomic<std::size_t> next_seq_` to `Impl`.

**Step 3: Run, pass. Commit** — `feat(core): RenderEngine priority queue (stable FIFO on ties)`

---

### Task 6: Cooperative cancellation

**Files:**
- Modify: `src/core/RenderEngine.cpp`
- Test: `tests/unit/test_render_engine_cancel.cpp`

**Step 1: Test — canceled token's callback is never called**

```cpp
TEST_CASE("RenderEngine::cancel before worker picks up task skips callback", "[core][render][cancel]") {
    litepdf::core::Document doc;
    REQUIRE(doc.open("tests/fixtures/simple.pdf"));
    litepdf::core::RenderEngine engine(doc, 1);
    std::atomic<int> calls{0};
    // Gate the worker so we can cancel before pickup.
    auto gate = engine.submit({0, 0, 1.0f, [](fz_pixmap* p, fz_context* ctx){
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (p) fz_drop_pixmap(ctx, p);
    }});
    (void)gate;
    auto tok = engine.submit({0, 2, 1.0f, [&](fz_pixmap* p, fz_context* ctx){
        ++calls;
        if (p) fz_drop_pixmap(ctx, p);
    }});
    engine.cancel(tok);
    // Wait for gate to drain
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    REQUIRE(calls.load() == 0);
}

TEST_CASE("RenderEngine::cancel_all_below_priority drops pending prefetches", "[core][render][cancel]") {
    // Submit P0 gate + three P2 tasks; cancel_all_below_priority(1); assert only gate completes.
    // ...
}
```

**Step 2: Implement**

`submit` allocates `shared_ptr<atomic<bool>>` for the token. Worker checks `canceled->load()` after dequeue; if true, skip callback. `cancel(tok)` sets the flag. `cancel_all_below_priority(p)` walks the priority queue and sets every token with `priority > p`.

Note: walking `std::priority_queue` requires extracting to a vector and rebuilding — acceptable at our queue sizes. Alternative: switch underlying container to `std::vector` + `std::make_heap`. Implementer chooses, flags trade-off in commit message.

**Step 3: Run, pass. Commit** — `feat(core): RenderEngine cooperative cancellation (pre-dequeue + bulk)`

---

### Task 7: Actually render — worker produces `fz_pixmap`

**Files:**
- Modify: `src/core/RenderEngine.cpp`
- Test: `tests/unit/test_render_engine_output.cpp`

**Step 1: Test — submitted page-0 render returns a non-null pixmap of expected dimensions**

```cpp
TEST_CASE("RenderEngine renders page 0 to a correctly-sized pixmap", "[core][render][output]") {
    litepdf::core::Document doc;
    REQUIRE(doc.open("tests/fixtures/simple.pdf"));
    auto size = doc.page_size(0);
    litepdf::core::RenderEngine engine(doc, 1);
    std::mutex m;
    std::condition_variable cv;
    fz_pixmap* got = nullptr;
    bool done = false;
    engine.submit({0, 0, 1.0f, [&](fz_pixmap* p, fz_context* ctx){
        // Keep for assertions outside the callback — caller becomes owner.
        if (p) fz_keep_pixmap(ctx, p);
        std::lock_guard g(m); got = p; done = true; cv.notify_all();
    }});
    {
        std::unique_lock lk(m);
        REQUIRE(cv.wait_for(lk, std::chrono::seconds(5), [&]{ return done; }));
    }
    REQUIRE(got != nullptr);
    // Cleanup on test thread via its own clone. refcount ops (keep/drop) are
    // atomic across clones of the same root ctx; the D1 rule concerns stateful
    // MuPDF ops, not refcount bookkeeping.
    fz_context* test_ctx = doc.clone_context();
    fz_drop_pixmap(test_ctx, got);
    fz_drop_context(test_ctx);
}
```

**Step 2: Implement real render inside `worker_loop`**

```cpp
// Pseudo:
fz_context* ctx = worker_ctxs[worker_idx];
fz_page* page = nullptr;
fz_pixmap* pix = nullptr;
fz_try(ctx) {
    page = fz_load_page(ctx, doc_handle_for_this_worker, req.page_num);
    // check cancel here
    fz_matrix m = fz_scale(req.scale, req.scale);
    pix = fz_new_pixmap_from_page(ctx, page, m, fz_device_rgb(ctx), 0);
}
fz_always(ctx) {
    if (page) fz_drop_page(ctx, page);
}
fz_catch(ctx) {
    if (pix) { fz_drop_pixmap(ctx, pix); pix = nullptr; }
    // log via common error path; fall through with nullptr
}
req.on_complete(pix);  // transfers ownership
```

**Complication:** each worker context needs its own `fz_document` handle (re-opened per worker) because `fz_document` is tied to the context that opened it. Options:
- (a) `fz_document` also cloned: MuPDF doesn't expose a clone for documents. Must `fz_open_document` on each worker's ctx.
- (b) Document caches its path; RenderEngine workers each open their own `fz_document` with their ctx.

Choose **(b)**. Add `Document::source_path()` getter (returns `std::string`). RenderEngine Impl opens one `fz_document` per worker context at construction, drops at destruction.

**Subagent flag:** if MuPDF exposes a document-sharing primitive the implementer discovers, they MAY switch strategies and justify in the commit message.

**Step 3: Run, pass. Commit** — `feat(core): RenderEngine produces fz_pixmap via per-worker fz_document`

---

### Task 8: `PageCache` scaffold + L1 (pixmap) cache

**Files:**
- Create: `src/core/PageCache.hpp`
- Create: `src/core/PageCache.cpp`
- Modify: `CMakeLists.txt`
- Test: `tests/unit/test_page_cache_l1.cpp`

**Step 1: Test — put then get hits; capacity enforces LRU eviction**

```cpp
TEST_CASE("PageCache L1: put + get returns same pixmap; miss returns null", "[core][cache][l1]") {
    litepdf::core::Document doc;
    REQUIRE(doc.open("tests/fixtures/simple.pdf"));
    fz_context* ctx = doc.clone_context();
    litepdf::core::PageCache cache(/*l1*/2, /*l2*/0, ctx);

    fz_pixmap* p0 = fz_new_pixmap(ctx, fz_device_rgb(ctx), 10, 10, nullptr, 0);
    cache.put_pixmap(0, 1.0f, p0);  // cache takes ownership

    fz_pixmap* got = cache.get_pixmap(0, 1.0f);  // caller owns returned reference
    REQUIRE(got == p0);
    REQUIRE(cache.get_pixmap(0, 2.0f) == nullptr);  // different scale = miss
    fz_drop_pixmap(ctx, got);
    // cache still holds its own reference; drop cache at scope end
}

TEST_CASE("PageCache L1 evicts least-recently-used on overflow", "[core][cache][l1]") {
    // Fill to capacity + 1; assert the oldest entry is dropped.
    // Implementer: track eviction via a mock fz_drop or by counting cache.size()
}
```

**Step 2: Implement**

Keep it simple: `std::list<Key>` for LRU order + `std::unordered_map<Key, Entry>` where Entry holds iterator back to list. Key for L1 = `std::pair<int, float>`.

Refcount rules (document in header):
- `put_pixmap(page, scale, pix)`: cache takes the caller's reference (does NOT call keep). If eviction fires for a different entry, cache `fz_drop_pixmap`s the evicted one.
- `get_pixmap(page, scale)`: returns `nullptr` on miss. On hit, cache calls `fz_keep_pixmap` and returns — caller must `fz_drop_pixmap` when done. Cache still holds its own reference.
- Destructor: drops all held references.

Cache stores the `fz_context*` it was constructed with and uses that for keeps/drops. **The context must outlive the cache.** Documented as a precondition.

**Step 3: Run, pass. Commit** — `feat(core): PageCache L1 (pixmap LRU with explicit refcounts)`

---

### Task 9: PageCache L2 (display list) + `get_or_create` fast path

**Files:**
- Modify: `src/core/PageCache.{hpp,cpp}`
- Test: `tests/unit/test_page_cache_l2.cpp`

**Step 1: Tests**

```cpp
TEST_CASE("PageCache L2: display list put/get with LRU eviction", "[core][cache][l2]") {
    // Analogous to L1 tests; key is just page_num (zoom-independent).
}
```

**Step 2: Implement** — copy the L1 structure with key = `int`.

**Step 3: Commit** — `feat(core): PageCache L2 display-list LRU`

---

### Task 10: Wire `RenderEngine` through `PageCache`

**Files:**
- Modify: `src/core/RenderEngine.{hpp,cpp}` — optional `PageCache*` parameter
- Test: `tests/unit/test_render_engine_cache.cpp`

**Step 1: Test — second render of same page hits L1, never reaches MuPDF**

```cpp
TEST_CASE("RenderEngine reuses L1 pixmap on repeat submit", "[core][render][cache]") {
    litepdf::core::Document doc;
    REQUIRE(doc.open("tests/fixtures/simple.pdf"));
    fz_context* cache_ctx = doc.clone_context();
    litepdf::core::PageCache cache(5, 10, cache_ctx);
    litepdf::core::RenderEngine engine(doc, 1, &cache);

    // Render page 0 twice at scale 1.0; second call should satisfy from cache.
    auto render_once = [&]() -> fz_pixmap* {
        std::mutex m; std::condition_variable cv; fz_pixmap* out=nullptr; bool done=false;
        engine.submit({0, 0, 1.0f, [&](fz_pixmap* p, fz_context* ctx){
            if (p) fz_keep_pixmap(ctx, p);  // extend lifetime past callback
            std::lock_guard g(m); out=p; done=true; cv.notify_all();
        }});
        std::unique_lock lk(m); cv.wait_for(lk, std::chrono::seconds(5), [&]{ return done; });
        return out;
    };
    fz_pixmap* first = render_once();
    fz_pixmap* second = render_once();
    REQUIRE(first != nullptr);
    REQUIRE(second != nullptr);
    // Same underlying buffer pointer — cache returned the kept reference.
    REQUIRE(first == second);
    fz_drop_pixmap(cache_ctx, first);
    fz_drop_pixmap(cache_ctx, second);
    fz_drop_context(cache_ctx);
}
```

**Step 2: Implement**

Worker logic becomes:
1. Check `cache.get_pixmap(page, scale)` → if hit, return via callback.
2. Check `cache.get_display_list(page)` → if miss, `fz_new_display_list_from_page`, `cache.put_display_list`.
3. Rasterize display list → pixmap, `cache.put_pixmap` (cache takes ref), then `fz_keep_pixmap` to hand caller its own ref, return via callback.

**Refcount invariant:** the pixmap pointer returned to the caller has **one reference owned by the caller** + **one reference owned by the cache**. Two drops needed in total.

**Step 3: Commit** — `feat(core): RenderEngine consults PageCache for L1/L2 hits`

---

### Task 11: Stress test — 1000 renders, 25 MB RSS cap

**Files:**
- Test: `tests/unit/test_render_stress.cpp`

**Step 1: Implement stress test**

```cpp
#include <psapi.h>
#pragma comment(lib, "psapi.lib")

TEST_CASE("Stress: 1000 sequential renders stay under 25 MB RSS growth", "[core][render][stress]") {
    litepdf::core::Document doc;
    REQUIRE(doc.open("tests/fixtures/simple.pdf"));
    fz_context* cache_ctx = doc.clone_context();
    litepdf::core::PageCache cache(5, 10, cache_ctx);
    litepdf::core::RenderEngine engine(doc, 2, &cache);
    int page_count = doc.page_count();
    REQUIRE(page_count > 0);

    PROCESS_MEMORY_COUNTERS_EX before{}; before.cb = sizeof(before);
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&before), sizeof(before));

    std::atomic<int> done{0};
    for (int i = 0; i < 1000; ++i) {
        engine.submit({i % page_count, 0, 1.0f, [&](fz_pixmap* p, fz_context* ctx){
            // D2: caller owns the passed pixmap reference. Cache (if wired) still holds its own.
            if (p) fz_drop_pixmap(ctx, p);
            ++done;
        }});
    }
    while (done.load() < 1000) std::this_thread::sleep_for(std::chrono::milliseconds(5));

    PROCESS_MEMORY_COUNTERS_EX after{}; after.cb = sizeof(after);
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&after), sizeof(after));
    SIZE_T growth = after.PeakWorkingSetSize - before.PeakWorkingSetSize;
    INFO("RSS growth: " << (growth / (1024*1024)) << " MB");
    REQUIRE(growth < 25 * 1024 * 1024);
    fz_drop_context(cache_ctx);
}
```

**Note:** Per D2, the callback already receives a `fz_context*` — no extra plumbing needed here. Just drop the pixmap inside the callback; the cache (once Task 10 wires it in) keeps its own ref for future hits.

**Step 2: Run, pass (expected: first run may reveal a leak; fix it). Commit** — `test: render stress 1000 pages under 25 MB RSS cap`

---

### Task 12: `litepdf-cli --render N` flag (manual smoke check)

**Files:**
- Modify: `src/cli/main.cpp`

**Step 1: Parse `--render N` arg; if set, render page N and write PPM to stdout**

```cpp
// If argv contains --render, take the next arg as page index.
// RenderEngine engine(doc, 1);
// engine.submit({page, 0, 1.0f, [](fz_pixmap* p, fz_context* ctx){
//     if (!p) return;
//     // fz_pixmap_width/height/stride/samples -> write PPM header + RGB bytes to stdout.
//     fz_drop_pixmap(ctx, p);
// }});
// Wait for completion (e.g. an atomic<bool> + busy wait, or std::promise).
```

**Step 2: Manual smoke** (no automated test; document in commit that `litepdf-cli simple.pdf --render 0 > out.ppm` produces a valid PPM that opens in any viewer).

**Step 3: Commit** — `feat(cli): --render N writes page PPM to stdout`

---

### Task 13: CI sanity sweep

**Files:**
- Modify: `.github/workflows/ci.yml` — no functional change expected; confirm stress test is picked up by `ctest`. If runtime blows past CI timeout, tag the stress test with `[.stress]` and run it only on tag builds.

**Step 1: Push; observe CI green. If stress test runs > 2 min on CI, split it into its own workflow job with `continue-on-error: false` and keep the default `ctest` for fast tests.**

**Step 2: Commit (if any changes)** — `ci: run render stress on windows-latest`

---

### Task 14: Tag `v0.0.3-phase2`

**Step 1: Final review via `feature-dev:code-reviewer`** — same pattern as Phase 1.

**Step 2: Address Important issues only; Minor polish goes to a follow-up `v0.0.3-phase2.1` tag if needed.**

**Step 3:**
```bash
git tag -a v0.0.3-phase2 -m "Phase 2: RenderEngine + PageCache"
git push origin v0.0.3-phase2
```

---

## Deferred to Phase 3 (do not do in Phase 2)

- `ID2D1Bitmap` upload path (Phase 3 adds, possibly replacing L1 key type).
- `PostMessage` marshaling from worker callback to UI thread.
- Async document open (Phase 3 wraps `Document::open` + first-page render behind a spinner).
- Render-time telemetry (log per-page render times to `%LOCALAPPDATA%` log).
- Prefetch policy (who submits P1/P2 requests — Phase 3's DocumentView).

## Deferred Phase 1 items that surface here

Addressed in Phase 2:
- ✅ `fz_context` cloning for thread pool (Task 1 + Task 3).
- ✅ `fz_page` caching hook (L2 display list in Task 9).

Still deferred:
- `flatten_outline` recursion bound (hardening, not Phase 2).
- Unicode path handling (Phase 3 file-dialog work).
- MuPDF 1.24.11 CVE audit (pre-v1.0 release gate; Phase 12).
- `vcxproj` copy-to-build-dir (ditch in-source patch — pre-v1.0 cleanup).
- C++20 upgrade decision (pre-Phase 4).
- CI `actions/cache` for MuPDF build (pre-Phase 3; CI gets faster).

## Re-Planning Hooks

Before starting Phase 3, re-invoke `superpowers:writing-plans` with:
- Phase 2 exit commit SHA.
- Actual L1/L2 memory numbers from stress test.
- Confirmation that D2 (callback carries `fz_context*`) stood up in practice — Phase 3 UI will continue the pattern by `PostMessage`-ing a pixmap + then calling `fz_keep_pixmap` before handoff (UI thread needs its own ctx clone separately).
