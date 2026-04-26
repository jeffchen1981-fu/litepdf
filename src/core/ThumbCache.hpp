#pragma once

// core::ThumbCache - small HBITMAP LRU keyed by page_num. Separate
// from PageCache to avoid polluting L1/L2 with thumb-resolution
// pixmaps. Owns HBITMAP refs (DeleteObject on evict / replace / dtor).
//
// Thread-safety: NONE. UI-thread-only.

#include <cstddef>
#include <list>
#include <unordered_map>
#include <windows.h>

namespace litepdf::core {

class ThumbCache {
public:
    explicit ThumbCache(std::size_t capacity);
    ~ThumbCache();

    ThumbCache(const ThumbCache&)            = delete;
    ThumbCache& operator=(const ThumbCache&) = delete;

    // Returns the HBITMAP for `page` and marks it most-recently-used.
    // Caller does NOT take ownership - handle is valid until cache
    // evicts or is destroyed. Caller MUST NOT DeleteObject the result.
    HBITMAP get(int page);

    // Cache TAKES ownership of `bm`. If `page` already had a bitmap,
    // the old one is DeleteObject'd. Replacing with the same handle
    // is a no-op.
    void put(int page, HBITMAP bm);

    void clear();

    std::size_t size() const noexcept { return map_.size(); }
    std::size_t capacity() const noexcept { return capacity_; }

private:
    using Iter = std::list<int>::iterator;
    std::size_t                          capacity_;
    std::list<int>                       lru_;       // front = MRU
    std::unordered_map<int, std::pair<HBITMAP, Iter>> map_;
};

}  // namespace litepdf::core
