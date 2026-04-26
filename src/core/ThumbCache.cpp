#include "core/ThumbCache.hpp"

namespace litepdf::core {

ThumbCache::ThumbCache(std::size_t capacity) : capacity_(capacity) {}

ThumbCache::~ThumbCache() { clear(); }

HBITMAP ThumbCache::get(int page) {
    auto it = map_.find(page);
    if (it == map_.end()) return nullptr;
    lru_.splice(lru_.begin(), lru_, it->second.second);
    return it->second.first;
}

void ThumbCache::put(int page, HBITMAP bm) {
    auto it = map_.find(page);
    if (it != map_.end()) {
        if (it->second.first == bm) {
            lru_.splice(lru_.begin(), lru_, it->second.second);
            return;
        }
        DeleteObject(it->second.first);
        it->second.first = bm;
        lru_.splice(lru_.begin(), lru_, it->second.second);
        return;
    }
    while (map_.size() >= capacity_ && !lru_.empty()) {
        const int evict = lru_.back();
        lru_.pop_back();
        auto evict_it = map_.find(evict);
        if (evict_it != map_.end()) {
            DeleteObject(evict_it->second.first);
            map_.erase(evict_it);
        }
    }
    lru_.push_front(page);
    map_.emplace(page, std::make_pair(bm, lru_.begin()));
}

void ThumbCache::clear() {
    for (auto& kv : map_) DeleteObject(kv.second.first);
    map_.clear();
    lru_.clear();
}

}  // namespace litepdf::core
