#include "core/PageCache.hpp"

#include <mupdf/fitz.h>

#include <cstddef>
#include <functional>
#include <list>
#include <mutex>
#include <unordered_map>

namespace litepdf::core {

namespace {

struct L1Key {
    int page;
    float scale;
    bool operator==(const L1Key& o) const noexcept {
        return page == o.page && scale == o.scale;
    }
};

struct L1KeyHash {
    std::size_t operator()(const L1Key& k) const noexcept {
        // Mix page bits with scale bits. std::hash<float> already folds
        // the raw IEEE-754 bits, so we just need a decent combiner.
        std::size_t h1 = std::hash<int>{}(k.page);
        std::size_t h2 = std::hash<float>{}(k.scale);
        return h1 ^ (h2 + 0x9e3779b97f4a7c15ULL + (h1 << 6) + (h1 >> 2));
    }
};

} // namespace

struct PageCache::Impl {
    fz_context* ctx;
    std::size_t l1_capacity;
    std::size_t l2_capacity;  // stored for Task 9; ignored here
    mutable std::mutex mtx;

    struct L1Entry {
        fz_pixmap* pix;
        std::list<L1Key>::iterator lru_it;
    };

    std::list<L1Key> l1_lru;  // front = most recent, back = oldest
    std::unordered_map<L1Key, L1Entry, L1KeyHash> l1_map;

    Impl(std::size_t l1_cap, std::size_t l2_cap, fz_context* c)
        : ctx(c), l1_capacity(l1_cap), l2_capacity(l2_cap) {}

    // Caller must hold mtx.
    void evict_oldest_l1() {
        if (l1_lru.empty()) return;
        const L1Key oldest = l1_lru.back();  // copy, not ref — erase invalidates
        auto it = l1_map.find(oldest);
        if (it != l1_map.end()) {
            fz_drop_pixmap(ctx, it->second.pix);
            l1_map.erase(it);
        }
        l1_lru.pop_back();
    }
};

PageCache::PageCache(std::size_t l1_capacity, std::size_t l2_capacity, fz_context* ctx)
    : impl_(std::make_unique<Impl>(l1_capacity, l2_capacity, ctx)) {}

PageCache::~PageCache() {
    if (!impl_) return;
    std::lock_guard<std::mutex> lk(impl_->mtx);
    for (auto& kv : impl_->l1_map) {
        fz_drop_pixmap(impl_->ctx, kv.second.pix);
    }
    impl_->l1_map.clear();
    impl_->l1_lru.clear();
}

fz_pixmap* PageCache::get_pixmap(int page_num, float scale) {
    std::lock_guard<std::mutex> lk(impl_->mtx);
    L1Key k{page_num, scale};
    auto it = impl_->l1_map.find(k);
    if (it == impl_->l1_map.end()) return nullptr;
    // Move to front (most recent).
    impl_->l1_lru.erase(it->second.lru_it);
    impl_->l1_lru.push_front(k);
    it->second.lru_it = impl_->l1_lru.begin();
    // Cache keeps its ref; caller gets a new ref via fz_keep_pixmap.
    return fz_keep_pixmap(impl_->ctx, it->second.pix);
}

void PageCache::put_pixmap(int page_num, float scale, fz_pixmap* pix) {
    if (!pix) return;  // no-op on null; nothing to take ownership of
    std::lock_guard<std::mutex> lk(impl_->mtx);
    L1Key k{page_num, scale};
    auto it = impl_->l1_map.find(k);
    if (it != impl_->l1_map.end()) {
        // Replace at same key: drop old, store new, promote to front.
        fz_drop_pixmap(impl_->ctx, it->second.pix);
        it->second.pix = pix;
        impl_->l1_lru.erase(it->second.lru_it);
        impl_->l1_lru.push_front(k);
        it->second.lru_it = impl_->l1_lru.begin();
        return;
    }
    // New entry. Evict until we have room. If l1_capacity == 0 we must
    // drop the incoming pixmap — cache takes the ref either way.
    if (impl_->l1_capacity == 0) {
        fz_drop_pixmap(impl_->ctx, pix);
        return;
    }
    while (impl_->l1_map.size() >= impl_->l1_capacity) {
        impl_->evict_oldest_l1();
    }
    impl_->l1_lru.push_front(k);
    impl_->l1_map.emplace(k, Impl::L1Entry{pix, impl_->l1_lru.begin()});
}

void PageCache::clear() {
    std::lock_guard<std::mutex> lk(impl_->mtx);
    for (auto& kv : impl_->l1_map) {
        fz_drop_pixmap(impl_->ctx, kv.second.pix);
    }
    impl_->l1_map.clear();
    impl_->l1_lru.clear();
}

std::size_t PageCache::l1_size() const {
    std::lock_guard<std::mutex> lk(impl_->mtx);
    return impl_->l1_map.size();
}

} // namespace litepdf::core
