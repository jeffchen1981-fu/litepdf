// LitePDF -- core::MuPdfPatchNotice (#229).
#include "core/MuPdfPatchNotice.hpp"

// Defined in third_party/mupdf/source/html/html-doc.c by the local patch.
extern "C" const char litepdf_mupdf_patch_notice_v1[];

namespace litepdf::core {

std::string_view mupdf_patch_notice() noexcept {
    return litepdf_mupdf_patch_notice_v1;
}

}  // namespace litepdf::core
