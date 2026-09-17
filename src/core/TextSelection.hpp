#pragma once

// core::TextSelection -- one tab's text selection (#52). Pure data: no MuPDF, no
// Win32, so it is headless-testable and cheap to read on the paint path.

#include <string>
#include <vector>

namespace litepdf::core {

// A position on a page in MuPDF page space: points, top-left origin, y down.
// MuPDF places the page box's top-left corner at (0, 0) for every format LitePDF
// opens -- pdf_page_obj_transform_box translates the CropBox origin, and the
// other formats' bound_page functions hard-code it -- so this is also the frame
// of the rendered bitmap. No translation exists anywhere in the selection path;
// tests/unit/test_document_selection.cpp pins it with a CropBox-offset page.
struct SelPoint {
    float x = 0.0f;
    float y = 0.0f;
};

// FZ_SELECT_CHARS / FZ_SELECT_WORDS / FZ_SELECT_LINES.
enum class SelectMode { Chars, Words, Lines };

struct Quad {
    float ul_x = 0.0f, ul_y = 0.0f;
    float ur_x = 0.0f, ur_y = 0.0f;
    float ll_x = 0.0f, ll_y = 0.0f;
    float lr_x = 0.0f, lr_y = 0.0f;
};

struct TextSelection {
    int page = -1;

    // RAW, UN-SNAPPED pointer positions. fz_snap_selection reorders the points
    // it is given, so writing a snapped result back here would move the anchor
    // on every backward drag and walk the selection across the page (spec §2).
    // Select All is an ordinary selection whose points bracket the page's text.
    SelPoint   anchor{};
    SelPoint   extent{};
    SelectMode mode = SelectMode::Chars;

    // Materialised when the gesture ends. Afterwards painting, Ctrl+C and a tab
    // switch are pure data -- no MuPDF, no lock.
    std::vector<Quad> quads;
    std::string       text_utf8;   // CRLF line endings
};

}  // namespace litepdf::core
