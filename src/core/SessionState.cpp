#include "core/SessionState.hpp"

#include <windows.h>   // WideCharToMultiByte / MultiByteToWideChar
#include <cstdio>      // snprintf
#include <string>

namespace litepdf::core {
namespace {

std::string wide_to_utf8(const std::wstring& w) {
    if (w.empty()) return {};
    // Source is a live std::filesystem::path (valid UTF-16); WC_ERR_INVALID_CHARS
    // is belt-and-suspenders. On failure return empty (caller path is then "").
    int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, w.data(), (int)w.size(),
                                nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, w.data(), (int)w.size(),
                        out.data(), n, nullptr, nullptr);
    return out;
}

std::string float_to_str(float f) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.9g", (double)f);
    return buf;
}

void append_escaped(std::string& out, const std::string& s) {
    out.push_back('"');
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back((char)c);  // UTF-8 bytes pass through
                }
        }
    }
    out.push_back('"');
}

const char* zoom_to_str(SessionZoom z) {
    switch (z) {
        case SessionZoom::FitWidth: return "fit_width";
        case SessionZoom::FitPage:  return "fit_page";
        case SessionZoom::Custom:   return "custom";
    }
    return "fit_width";
}

}  // namespace

std::string to_json(const SessionState& s) {
    std::string o;
    o.reserve(256);
    o += "{\"version\":" + std::to_string(s.version);
    o += ",\"window\":{";
    o += "\"flags\":" + std::to_string(s.window.flags);
    o += ",\"show\":"  + std::to_string(s.window.show);
    o += ",\"x\":"     + std::to_string(s.window.x);
    o += ",\"y\":"     + std::to_string(s.window.y);
    o += ",\"w\":"     + std::to_string(s.window.w);
    o += ",\"h\":"     + std::to_string(s.window.h);
    o += "}";
    o += ",\"active\":" + std::to_string(s.active_tab);
    o += ",\"tabs\":[";
    for (size_t i = 0; i < s.tabs.size(); ++i) {
        const auto& t = s.tabs[i];
        if (i) o.push_back(',');
        o += "{\"path\":";
        append_escaped(o, wide_to_utf8(t.path.wstring()));
        o += ",\"page\":" + std::to_string(t.page);
        o += ",\"zoom_mode\":\"";
        o += zoom_to_str(t.zoom_mode);
        o += "\",\"zoom_scale\":" + float_to_str(t.zoom_scale);
        o += "}";
    }
    o += "]}";
    return o;
}

}  // namespace litepdf::core
