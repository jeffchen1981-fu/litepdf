#include "core/SessionState.hpp"

#include <windows.h>   // WideCharToMultiByte / MultiByteToWideChar
#include <cmath>       // std::isfinite
#include <cstdio>      // snprintf
#include <cstdlib>     // std::strtol / std::strtod
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

// Strict UTF-8 -> UTF-16. Returns false on invalid UTF-8 (caller fails the parse).
bool utf8_to_wide_strict(const std::string& s, std::wstring& out) {
    if (s.empty()) { out.clear(); return true; }
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) return false;
    out.assign(n, L'\0');
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), (int)s.size(),
                               out.data(), n) > 0;
}

struct ParseError {};

class Json {
public:
    explicit Json(std::string_view in) : s_(in) {}

    void parse_root_object_into(SessionState& out) {
        ws();
        parse_object(out);
        ws();
        if (i_ != s_.size()) throw ParseError{};   // reject trailing junk
    }

private:
    std::string_view s_;
    size_t i_ = 0;

    [[noreturn]] void fail() { throw ParseError{}; }
    void ws() { while (i_ < s_.size() && (s_[i_]==' '||s_[i_]=='\t'||s_[i_]=='\n'||s_[i_]=='\r')) ++i_; }
    char peek() { if (i_ >= s_.size()) fail(); return s_[i_]; }
    char get()  { if (i_ >= s_.size()) fail(); return s_[i_++]; }
    void expect(char c) { if (get() != c) fail(); }

    int hex4() {
        int v = 0;
        for (int k = 0; k < 4; ++k) {
            char h = get(); v <<= 4;
            if (h>='0'&&h<='9') v |= h-'0';
            else if (h>='a'&&h<='f') v |= h-'a'+10;
            else if (h>='A'&&h<='F') v |= h-'A'+10;
            else fail();
        }
        return v;
    }

    // Append codepoint cp (BMP only here) as UTF-8 bytes.
    static void append_utf8(std::string& r, unsigned cp) {
        if (cp < 0x80) { r.push_back((char)cp); }
        else if (cp < 0x800) {
            r.push_back((char)(0xC0 | (cp >> 6)));
            r.push_back((char)(0x80 | (cp & 0x3F)));
        } else {
            r.push_back((char)(0xE0 | (cp >> 12)));
            r.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
            r.push_back((char)(0x80 | (cp & 0x3F)));
        }
    }

    std::string parse_string() {
        expect('"');
        std::string r;
        for (;;) {
            char c = get();
            if (c == '"') return r;
            if (c == '\\') {
                char e = get();
                switch (e) {
                    case '"': r.push_back('"'); break;
                    case '\\': r.push_back('\\'); break;
                    case '/': r.push_back('/'); break;
                    case 'b': r.push_back('\b'); break;
                    case 'f': r.push_back('\f'); break;
                    case 'n': r.push_back('\n'); break;
                    case 'r': r.push_back('\r'); break;
                    case 't': r.push_back('\t'); break;
                    case 'u': {
                        unsigned cp = (unsigned)hex4();
                        if (cp >= 0xD800 && cp <= 0xDBFF) {        // high surrogate
                            if (get() != '\\' || get() != 'u') fail();
                            unsigned lo = (unsigned)hex4();
                            if (lo < 0xDC00 || lo > 0xDFFF) fail();
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            // 4-byte UTF-8
                            r.push_back((char)(0xF0 | (cp >> 18)));
                            r.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
                            r.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
                            r.push_back((char)(0x80 | (cp & 0x3F)));
                        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {  // lone low surrogate
                            fail();
                        } else {
                            append_utf8(r, cp);
                        }
                        break;
                    }
                    default: fail();
                }
            } else {
                if ((unsigned char)c < 0x20) fail();  // RFC 7159 sec 7: no unescaped 0x00-0x1F in strings
                r.push_back(c);  // raw UTF-8 bytes pass through
            }
        }
    }

    long parse_int() {
        ws();
        size_t start = i_;
        if (i_ < s_.size() && (s_[i_]=='-'||s_[i_]=='+')) ++i_;
        size_t digits_start = i_;
        while (i_ < s_.size() && s_[i_]>='0' && s_[i_]<='9') ++i_;
        if (i_ == digits_start) fail();   // require >= 1 digit after optional sign
        return std::strtol(std::string(s_.substr(start, i_-start)).c_str(), nullptr, 10);
    }

    double parse_number() {
        ws();
        size_t start = i_;
        while (i_ < s_.size()) {
            char c = s_[i_];
            if ((c>='0'&&c<='9')||c=='-'||c=='+'||c=='.'||c=='e'||c=='E') ++i_;
            else break;
        }
        if (i_ == start) fail();
        std::string tok(s_.substr(start, i_-start));
        char* end = nullptr;
        double v = std::strtod(tok.c_str(), &end);
        if (end != tok.c_str() + tok.size()) fail();  // reject "1.2.3", "1e", "1e+"
        return v;
    }

    SessionZoom parse_zoom() {
        std::string z = parse_string();
        if (z == "fit_width") return SessionZoom::FitWidth;
        if (z == "fit_page")  return SessionZoom::FitPage;
        if (z == "custom")    return SessionZoom::Custom;
        fail();
    }

    void parse_window(SessionWindow& w) {
        expect('{');
        for (;;) {
            ws();
            std::string key = parse_string();
            ws(); expect(':');
            long v = parse_int();
            if (key=="flags") w.flags=(int)v;
            else if (key=="show") w.show=(int)v;
            else if (key=="x") w.x=(int)v;
            else if (key=="y") w.y=(int)v;
            else if (key=="w") w.w=(int)v;
            else if (key=="h") w.h=(int)v;
            else fail();
            ws();
            char c = get();
            if (c=='}') break;
            if (c!=',') fail();
        }
    }

    void parse_tab(SessionTab& t) {
        expect('{');
        for (;;) {
            ws();
            std::string key = parse_string();
            ws(); expect(':');
            if (key=="path") {
                std::wstring w;
                if (!utf8_to_wide_strict(parse_string(), w)) fail();
                t.path = std::filesystem::path(w);
            }
            else if (key=="page") t.page = (int)parse_int();
            else if (key=="zoom_mode") t.zoom_mode = parse_zoom();
            else if (key=="zoom_scale") t.zoom_scale = (float)parse_number();
            else fail();
            ws();
            char c = get();
            if (c=='}') break;
            if (c!=',') fail();
        }
    }

    void parse_tabs(std::vector<SessionTab>& tabs) {
        expect('[');
        ws();
        if (peek()==']') { get(); return; }
        for (;;) {
            ws();
            SessionTab t;
            parse_tab(t);
            tabs.push_back(std::move(t));
            ws();
            char c = get();
            if (c==']') break;
            if (c!=',') fail();
        }
    }

    void parse_object(SessionState& out) {
        expect('{');
        for (;;) {
            ws();
            std::string key = parse_string();
            ws(); expect(':');
            if (key=="version") out.version = (int)parse_int();
            else if (key=="window") parse_window(out.window);
            else if (key=="active") out.active_tab = (int)parse_int();
            else if (key=="tabs") parse_tabs(out.tabs);
            else fail();
            ws();
            char c = get();
            if (c=='}') break;
            if (c!=',') fail();
        }
    }
};

// Post-parse invariant validation. Returns false to reject the whole file.
bool validate(const SessionState& s) {
    if (s.version != kSessionVersion) return false;
    // active_tab must index a real tab. Reject a negative active_tab whenever
    // there are tabs to restore (a stray -1 alongside non-empty tabs is the
    // shutdown-race case); a zero-tab session with active_tab 0 is benign.
    if (s.tabs.empty()) {
        if (s.active_tab != 0 && s.active_tab != -1) return false;
    } else if (s.active_tab < 0 || s.active_tab >= (int)s.tabs.size()) {
        return false;
    }
    for (const auto& t : s.tabs) {
        if (t.path.empty()) return false;   // missing/blank "path" — to_json never emits this
        if (t.page < 0) return false;
        // zoom_scale only matters for Custom zoom; FitWidth/FitPage recompute it
        // from the viewport on restore (restore_on_tab_ready) and never read the
        // saved value. A minimized window can momentarily save a 0 fit-scale
        // (0x0 client rect) — rejecting the whole file for that would silently
        // drop an otherwise-valid restore set. Only enforce positivity for Custom.
        if (t.zoom_mode == SessionZoom::Custom &&
            (!std::isfinite(t.zoom_scale) || t.zoom_scale <= 0.0f)) return false;
    }
    // Window dims: 0 means "unset" (open at default); negative is corrupt.
    if (s.window.w < 0 || s.window.h < 0) return false;
    return true;
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

std::optional<SessionState> from_json(std::string_view json) {
    try {
        SessionState s;
        Json(json).parse_root_object_into(s);
        if (!validate(s)) return std::nullopt;
        return s;
    } catch (const ParseError&) {
        return std::nullopt;
    }
}

}  // namespace litepdf::core
