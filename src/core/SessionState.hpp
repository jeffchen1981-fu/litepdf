#pragma once
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace litepdf::core {

enum class SessionZoom { FitWidth, FitPage, Custom };

// v2 (PR-A1): SessionTab::zoom_scale changed meaning from a point->pixel render
// scale to a user-facing magnification percentage. from_json migrates v1 by
// resetting Custom zooms to FitWidth -- the mode v1.2.0 actually persisted, and
// this build's default. PR-A1 reset them to FitPage instead only because that
// release had no wheel scrolling and a FitWidth page was unnavigable below the
// fold; PR-A2 ships the wheel. See SessionState.cpp.
inline constexpr int kSessionVersion = 2;

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

// Report the version a document DECLARES, without migrating or validating it.
// Returns nullopt if the document does not parse; treats an absent "version"
// key as 1, matching from_json.
//
// SessionStore uses this to decide whether the file on disk is still v1. A raw
// text scan cannot be trusted for that decision: the parser decodes \u escapes
// in keys and lets a later duplicate key win, so a scan and the parser can
// disagree about the one field the backup turns on.
std::optional<int> peek_version(std::string_view json);

}  // namespace litepdf::core
