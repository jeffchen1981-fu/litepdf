#pragma once
#include <filesystem>
#include <optional>
#include "core/SessionState.hpp"

namespace litepdf::core {

inline constexpr unsigned kMaxSessionBytes = 1u << 20;  // 1 MB

// Returns false on any I/O failure (and leaves any existing file untouched).
bool save_session(const std::filesystem::path& file, const SessionState& s);

// Returns nullopt if the file is missing, oversized, unreadable, or malformed.
std::optional<SessionState> load_session(const std::filesystem::path& file);

}  // namespace litepdf::core
