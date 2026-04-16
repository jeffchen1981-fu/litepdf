#pragma once

// core::MruList — most-recently-used file list backed by the Windows
// registry. Stores up to kMaxEntries paths under HKCU. Headless and
// unit-testable: constructor accepts a registry sub-key path so tests
// can use a disposable key.
//
// Usage:
//   MruList mru;        // uses default key SOFTWARE\LitePDF\MRU
//   mru.load();         // read from registry
//   mru.push(path);     // add/move-to-front
//   mru.save();         // write back
//   auto v = mru.entries();  // most-recent-first
//
// Limitations:
//   - Paths are loaded via a 1024-wchar stack buffer. Paths stored exceeding
//     this limit (long-path mode) are silently truncated on load. `save()` has
//     no such limit. Typical PDF paths are well under MAX_PATH (260).

#include <cstddef>
#include <string>
#include <vector>

namespace litepdf::core {

class MruList {
public:
    static constexpr std::size_t kMaxEntries = 10;
    static constexpr wchar_t kDefaultKey[] = L"SOFTWARE\\LitePDF\\MRU";

    explicit MruList(const wchar_t* registry_subkey = kDefaultKey);

    // Read entries from registry into in-memory list.
    void load();

    // Write in-memory list to registry. Returns true on full success,
    // false if the registry key could not be created or any value write
    // failed (e.g., GPO-blocked, quota exceeded). Stale-value cleanup
    // errors are intentionally ignored (cosmetic only).
    bool save() const;

    // Add or move `path` to the front. Caps at kMaxEntries.
    void push(const std::wstring& path);

    // Remove entry at `index`. No-op if out of range.
    void remove(std::size_t index);

    // Current entries, most-recent first.
    const std::vector<std::wstring>& entries() const noexcept { return entries_; }

private:
    std::wstring registry_subkey_;
    std::vector<std::wstring> entries_;
};

}  // namespace litepdf::core
