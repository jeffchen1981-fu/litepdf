#pragma once

// core::PageCache — per-tab two-tier LRU cache over MuPDF refcounted objects.
// L1 (this task): fz_pixmap, keyed by (page_num, scale). Used for direct blit.
// L2 (Task 9): fz_display_list, keyed by page_num. Zoom-independent.
//
// Header stays MuPDF-free: only forward declarations of fz_* types.

#include <cstddef>
#include <memory>

// Forward decls (header stays MuPDF-free).
struct fz_pixmap;
struct fz_display_list;
struct fz_context;

namespace litepdf::core {

/// Per-tab two-tier LRU cache over MuPDF refcounted objects.
///
/// L1 (this task): fz_pixmap, keyed by (page_num, scale). Used for
/// direct blit. L2 (Task 9): fz_display_list, keyed by page_num.
/// Zoom-independent for fast re-rasterize.
///
/// Refcount contract (CRITICAL):
///   put_pixmap(page, scale, pix): cache TAKES the caller's reference.
///     Caller must NOT drop after calling put; cache drops on
///     eviction and on destruction. put_pixmap with a null pix is a
///     no-op (cache takes nothing; there is nothing to drop).
///   get_pixmap(page, scale): returns nullptr on miss. On hit, cache
///     fz_keep_pixmap's the entry and returns — caller OWNS the
///     returned ref and MUST fz_drop_pixmap when done.
///
/// Thread-safety: all methods take a std::mutex. Safe for concurrent
/// access from multiple worker threads.
///
/// Precondition: the fz_context passed at construction MUST outlive
/// the cache. It is used for all internal keep/drop operations.
/// Refcount ops are atomic across clones of the same root context,
/// so passing any clone is fine.
class PageCache {
public:
    PageCache(std::size_t l1_capacity, std::size_t l2_capacity, fz_context* ctx);
    ~PageCache();

    PageCache(const PageCache&) = delete;
    PageCache& operator=(const PageCache&) = delete;
    PageCache(PageCache&&) = delete;
    PageCache& operator=(PageCache&&) = delete;

    // L1
    fz_pixmap* get_pixmap(int page_num, float scale);
    void       put_pixmap(int page_num, float scale, fz_pixmap* pix);

    // L2 — display list, zoom-independent, keyed by page_num.
    // Refcount contract mirrors L1:
    //   put_display_list: cache TAKES caller's ref; drops on evict/replace/destroy.
    //   get_display_list: nullptr on miss; on hit, returns fz_keep_display_list'd
    //     ref (caller owns, must fz_drop_display_list when done).
    fz_display_list* get_display_list(int page_num);
    void             put_display_list(int page_num, fz_display_list* list);

    void clear();                 // drops all cached entries (L1 + L2)
    std::size_t l1_size() const;  // for tests
    std::size_t l2_size() const;  // for tests

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace litepdf::core
