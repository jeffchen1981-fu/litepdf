// LitePDF -- core::MuPdfPatchNotice: the notice that litepdf's MuPDF is modified (#229).
#pragma once

#include <string_view>

namespace litepdf::core {

// The text of litepdf_mupdf_patch_notice_v1, which the local MuPDF patch
// third_party/mupdf-patches/0001-htdoc-one-pass-outline.patch defines in
// source/html/html-doc.c. It is the AGPL-3.0 section 5(a) notice that MuPDF
// was modified and when, shown in the About box.
//
// Referencing the symbol is also the proof that the linked libmupdf.lib was
// built from the patched source: a library built without the patch fails the
// link. The name carries the patch's version (V1 -> _v1) and is renamed with
// every new patch version, so a library built from an older version of the
// patch fails the link too.
std::string_view mupdf_patch_notice() noexcept;

}  // namespace litepdf::core
