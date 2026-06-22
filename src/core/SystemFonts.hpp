// src/core/SystemFonts.hpp
#pragma once

#include <string>
#include <vector>

// Forward-declare the MuPDF C ABI handles. Like Document.hpp, this header is
// included by translation units (and unit tests) that do NOT get MuPDF's include
// path, so it must not pull in any mupdf/*.h.
extern "C" {
struct fz_context;
struct fz_font;
}

namespace litepdf::core {

// Pure mapping: Adobe CJK ordering (FZ_ADOBE_CNS=0, GB=1, JAPAN=2, KOREA=3) +
// serif flag -> an ordered list of Windows font-family names to try. Empty for
// an unknown ordering. No OS calls — unit-testable.
std::vector<std::wstring> cjk_family_candidates(int ordering, int serif);

// MuPDF system-CJK-font hook (matches fz_load_system_cjk_font_fn). Resolves a
// Windows system font via DirectWrite; returns a new fz_font (caller owns one
// ref). NEVER returns NULL unless even the base14 last-resort fails (pathological).
// noexcept: no C++ exception may cross into MuPDF's C frame.
fz_font* resolve_cjk_system_font(fz_context* ctx, const char* name,
                                 int ordering, int serif) noexcept;

// Install resolve_cjk_system_font as ctx's CJK hook. Call once per Document base
// context, right after fz_register_document_handlers. fz_clone_context shares the
// refcounted font context, so all per-render/worker clones inherit it.
void install_system_cjk_font_loader(fz_context* ctx) noexcept;

}  // namespace litepdf::core
