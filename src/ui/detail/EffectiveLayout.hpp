#pragma once

// #55: which layout a tab shows (spec 7.3) and what each kick_render reason does
// in the strip (spec 3.4), as two small pure tables. No Win32, no MuPDF:
// MainWindow (PR 2) feeds effective_layout the tab's state and consults
// strip_kick_actions only when the tab's layout is the strip.

#include <cstddef>
#include <filesystem>

namespace litepdf::ui {

enum class LayoutKind { PagedSingle, PagedSpread, Strip };

// Spread wins over the continuous-scroll preference; the strip needs the
// preference and a document format that passed the sizing gate (spec 4.4).
// No view (no document, or a tab that failed to open) is always paged.
inline LayoutKind effective_layout(bool has_view, bool spread,
                                   bool pref_continuous,
                                   bool format_ok) noexcept {
    if (!has_view) return LayoutKind::PagedSingle;
    if (spread) return LayoutKind::PagedSpread;
    if (pref_continuous && format_ok) return LayoutKind::Strip;
    return LayoutKind::PagedSingle;
}

// The formats whose page sizes were measured cheap enough for the strip (spec 4.4,
// 7.3), by case-insensitive extension, and a document with at least one page.
inline bool strip_format_ok(const std::filesystem::path& source, int page_count) {
    if (page_count < 1) return false;
    const std::filesystem::path::string_type ext = source.extension().native();
    auto equals = [&ext](const char* want) {
        std::size_t i = 0;
        for (; want[i] != '\0'; ++i) {
            if (i >= ext.size()) return false;
            const auto c = ext[i];
            const auto lower = (c >= 'A' && c <= 'Z') ? static_cast<decltype(c)>(c - 'A' + 'a') : c;
            if (lower != static_cast<decltype(c)>(want[i])) return false;
        }
        return i == ext.size();
    };
    return equals(".pdf") || equals(".epub") || equals(".cbz") || equals(".svg");
}

enum class KickReason {
    Layout,
    Zoom,
    Navigate,
    TabSwitch,
    Restore,
    Restyle,
    Spread,
};

// What a kick does when the tab's layout is the strip. Callers in a paged layout
// never consult it. reschedule is true for every reason.
struct StripKickActions {
    bool refit = false;
    bool refresh_readout = false;
    bool restore_tab_position = false;
    bool relayout = false;
    bool restyle = false;
    bool reschedule = true;
};

inline StripKickActions strip_kick_actions(KickReason r) noexcept {
    StripKickActions a;
    switch (r) {
        case KickReason::Layout:
            a.refit = true;
            a.refresh_readout = true;
            a.relayout = true;
            break;
        case KickReason::Zoom:
            a.refresh_readout = true;
            a.relayout = true;
            break;
        case KickReason::Navigate:
            break;
        case KickReason::TabSwitch:
            a.refit = true;
            a.refresh_readout = true;
            a.restore_tab_position = true;
            a.relayout = true;
            break;
        case KickReason::Restore:
            // The caller navigates to the saved page.
            a.refit = true;
            a.refresh_readout = true;
            a.relayout = true;
            break;
        case KickReason::Restyle:
            a.restyle = true;
            break;
        case KickReason::Spread:
            // Spread turned off, entering the strip. apply_effective_layout has
            // already applied the 7.3 carry; restoring the tab's stored position
            // would overwrite it.
            a.refit = true;
            a.refresh_readout = true;
            a.relayout = true;
            break;
    }
    return a;
}

// Switching to the tab that is already current is not a tab switch: it only
// reschedules.
inline KickReason tab_switch_reason(int new_index, int old_index) noexcept {
    return new_index == old_index ? KickReason::Navigate : KickReason::TabSwitch;
}

}  // namespace litepdf::ui
