#pragma once

// core::ThumbnailRenderer - thumb-friendly facade over the existing
// per-tab RenderEngine. Submits at priority=3 with bypass_cache=true
// so the main render cache (L1/L2) is never touched.
//
// Lifetime contract: on_done runs on a worker thread and receives an
// HBITMAP (or nullptr on render failure); caller takes ownership.
// Adopts Phase 6 C1's task-drain pattern (D16): the dtor blocks until
// every in-flight on_done has completed, so it is safe to destroy the
// renderer (and any objects captured by on_done lambdas) right after
// submitting work. Mirrors the fix in commit 8e11f39 for SearchSession.

#include "core/RenderEngine.hpp"

#include <functional>
#include <memory>
#include <windows.h>

namespace litepdf::core {

class ThumbnailRenderer {
public:
    using OnDone = std::function<void(HBITMAP)>;

    // `engine` must outlive this ThumbnailRenderer. Typically each
    // DocumentView owns one RenderEngine and one ThumbnailRenderer
    // that borrows the engine. Default scale = 0.15 (~ 11 dpi at the
    // 72-baseline, producing 120-dip-wide thumbs on letter pages).
    explicit ThumbnailRenderer(RenderEngine& engine, float scale = 0.15f);
    ~ThumbnailRenderer();

    ThumbnailRenderer(const ThumbnailRenderer&)            = delete;
    ThumbnailRenderer& operator=(const ThumbnailRenderer&) = delete;

    // Submits page render at priority=3, bypass_cache=true. On completion,
    // on_done fires on a worker thread.
    void submit(int page, OnDone on_done);

    // Cancels all not-yet-started priority=3 requests. In-flight ones
    // still complete (cooperative cancellation, same as RenderEngine).
    void cancel_pending();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace litepdf::core
