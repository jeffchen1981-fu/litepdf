#pragma once
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace litepdf::core {

enum class SessionZoom { FitWidth, FitPage, Custom };

inline constexpr int kSessionVersion = 1;

struct SessionTab {
    std::filesystem::path path;
    int page = 0;
    SessionZoom zoom_mode = SessionZoom::FitWidth;
    float zoom_scale = 1.0f;
};

struct SessionWindow {
    int flags = 0;   // WINDOWPLACEMENT.flags
    int show = 1;    // WINDOWPLACEMENT.showCmd (SW_SHOWNORMAL)
    int x = 0, y = 0, w = 0, h = 0;  // rcNormalPosition
};

struct SessionState {
    int version = kSessionVersion;
    SessionWindow window;
    int active_tab = 0;
    std::vector<SessionTab> tabs;
};

// Compact UTF-8 JSON. Paths are stored UTF-8 and JSON-escaped.
std::string to_json(const SessionState& s);

// Fail-safe: returns nullopt on ANY malformed input, unsupported version,
// invalid UTF-8, a \u escape outside the BMP-safe set, or a failed invariant
// check. A corrupt session.json must degrade to "no restore", never crash or
// partially apply.
std::optional<SessionState> from_json(std::string_view json);

}  // namespace litepdf::core
