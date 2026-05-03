// src/printing/PrintJob.cpp -- Phase 8.5 Task 6
// PRINTPAGERANGE and the lpPageRanges / nPageRanges / nMaxPageRanges
// fields live on PRINTDLGEXW (PrintDlgExW), not PRINTDLGW. The plan
// referred to PRINTDLG.lpPageRanges; that was an error in the plan
// (those fields don't exist on PRINTDLG[W]). We use PrintDlgExW which
// is the modern Win32 print API and what real-world readers (SumatraPDF,
// Foxit) use.
#ifndef WINVER
#define WINVER       0x0601  // Windows 7 baseline
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include <windows.h>
// commdlg.h gates PRINTPAGERANGE / PRINTDLGEXW under
// #ifdef STDMETHOD which lives in <objbase.h>. The project's global
// WIN32_LEAN_AND_MEAN suppresses OLE/COM auto-includes from windows.h,
// so we include objbase.h explicitly here before commdlg.h.
#include <objbase.h>
#include <commdlg.h>

#include "printing/PrintJob.hpp"
#include "printing/PrintAbortFlag.hpp"
#include "printing/PrintGeometry.hpp"
#include "printing/PrintProgressDlg.hpp"
#include "printing/PrintRange.hpp"
#include "core/Document.hpp"

#include <mupdf/fitz.h>

#include <algorithm>
#include <cstdio>
#include <vector>

#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "gdi32.lib")

namespace litepdf::printing {

namespace {

// Convert PRINTPAGERANGE[] (Win32) -> our PageRange[] (POD).
std::vector<PageRange> to_page_ranges(PRINTPAGERANGE* p, std::size_t n) {
    std::vector<PageRange> out;
    out.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        out.push_back({ p[i].nFromPage, p[i].nToPage });
    }
    return out;
}

void show_error(HWND parent, const wchar_t* msg) {
    MessageBoxW(parent, msg, L"Print", MB_OK | MB_ICONERROR);
}

// Module-static back-pointer so the GDI abort callback (which has no
// user-data parameter) can find the active flag. Single-instance
// because PrintJob::run is single-threaded modal -- only one print
// job at a time. Reset to nullptr after EndDoc / AbortDoc via RAII.
static PrintAbortFlag* g_active_abort_flag = nullptr;

BOOL CALLBACK abort_proc(HDC, int /*iError*/) {
    // Pump messages so PrintProgressDlg's Cancel button can be observed
    // even while a single StretchDIBits is mid-execution (printer drivers
    // call back into us during long blits).
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (g_active_abort_flag && g_active_abort_flag->is_aborted())
           ? FALSE : TRUE;
}

// RAII wrapper: clones the Document's fz_context and re-opens the source
// file on the clone. MuPDF forbids sharing fz_document across contexts
// (RenderEngine pattern). Both context and document are dropped on scope
// exit per the project's mupdf-refcount-conventions skill.
struct PrintMupdfHandle {
    fz_context*  ctx = nullptr;
    fz_document* doc = nullptr;

    explicit PrintMupdfHandle(litepdf::core::Document& d) {
        ctx = d.clone_context();
        if (!ctx) return;
        const auto utf8 = d.source_path().u8string();
        fz_try(ctx) {
            doc = fz_open_document(
                ctx, reinterpret_cast<const char*>(utf8.c_str()));
        } fz_catch(ctx) {
            doc = nullptr;
        }
    }
    ~PrintMupdfHandle() {
        if (doc) fz_drop_document(ctx, doc);
        if (ctx) fz_drop_context(ctx);
    }
    PrintMupdfHandle(const PrintMupdfHandle&)            = delete;
    PrintMupdfHandle& operator=(const PrintMupdfHandle&) = delete;
    [[nodiscard]] bool valid() const { return ctx && doc; }
};

// Render one page to a BGRA pixmap at the printer's native DPI per the
// supplied scale mode. Returns the pixmap and the px translation needed
// to center it on paper. Caller owns the returned pixmap and MUST drop
// it via fz_drop_pixmap once StretchDIBits has consumed it.
struct RenderResult {
    fz_pixmap* pix = nullptr;
    int        dst_x = 0;
    int        dst_y = 0;
};

RenderResult render_page_to_pixmap(
    fz_context* ctx, fz_document* doc, std::size_t page_idx,
    HDC hdc, ScaleMode mode, float custom_pct, bool auto_rotate)
{
    RenderResult result{};
    fz_page* page = nullptr;
    fz_try(ctx) {
        page = fz_load_page(ctx, doc, static_cast<int>(page_idx));
    } fz_catch(ctx) {
        page = nullptr;
    }
    if (!page) return result;

    // page_rect_pt: fz_bound_page returns MuPDF POINTS (1 pt = 1/72 inch).
    fz_rect bound{};
    fz_try(ctx) {
        bound = fz_bound_page(ctx, page);
    } fz_catch(ctx) {
        fz_drop_page(ctx, page);
        return result;
    }
    const Rect page_rect_pt{ bound.x0, bound.y0, bound.x1, bound.y1 };

    const float dpi_x = static_cast<float>(GetDeviceCaps(hdc, LOGPIXELSX));
    const float dpi_y = static_cast<float>(GetDeviceCaps(hdc, LOGPIXELSY));

    // paper_rect_px: printable area in PRINTER DEVICE PIXELS.
    const Rect paper_rect_px{
        0.0f, 0.0f,
        static_cast<float>(GetDeviceCaps(hdc, HORZRES)),
        static_cast<float>(GetDeviceCaps(hdc, VERTRES)),
    };

    // compute_render_matrix returns scale in px/pt -- correct unit for fz_scale.
    const auto pm = compute_render_matrix(
        page_rect_pt, paper_rect_px, mode, custom_pct, auto_rotate, dpi_x, dpi_y);

    // pm.scale_x is in px/pt. fz_scale(s) applied to page points yields
    // device pixels -- exactly what StretchDIBits needs. Translation is
    // applied in StretchDIBits dst rect, NOT in the MuPDF matrix; this
    // keeps the rendered pixmap's bbox at origin (0,0,w,h) and avoids
    // wasted pixels for the centering offset.
    fz_matrix m = fz_scale(pm.scale_x, pm.scale_y);
    if (pm.rotate_90) m = fz_pre_rotate(m, 90.0f);

    fz_pixmap* pix = nullptr;
    fz_try(ctx) {
        pix = fz_new_pixmap_from_page(
            ctx, page, m, fz_device_bgr(ctx), /*alpha*/1);
    } fz_always(ctx) {
        fz_drop_page(ctx, page);
    } fz_catch(ctx) {
        pix = nullptr;
    }

    result.pix = pix;
    result.dst_x = static_cast<int>(pm.tx + 0.5f);
    result.dst_y = static_cast<int>(pm.ty + 0.5f);
    return result;
}

bool blit_pixmap_to_hdc(HDC hdc, fz_pixmap* pix, fz_context* ctx,
                        int dst_x, int dst_y) {
    if (!pix) return false;

    int w = 0, h = 0;
    const unsigned char* samples = nullptr;
    fz_try(ctx) {
        w       = fz_pixmap_width(ctx, pix);
        h       = fz_pixmap_height(ctx, pix);
        samples = fz_pixmap_samples(ctx, pix);
    } fz_catch(ctx) {
        return false;
    }
    if (!samples || w <= 0 || h <= 0) return false;

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize        = sizeof(bmi.bmiHeader);
    bmi.bmiHeader.biWidth       = w;
    bmi.bmiHeader.biHeight      = -h;          // top-down
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    const int rc = StretchDIBits(
        hdc,
        /*dst*/ dst_x, dst_y, w, h,
        /*src*/ 0, 0, w, h,
        samples, &bmi, DIB_RGB_COLORS, SRCCOPY);
    return rc != GDI_ERROR && rc != 0;
}

} // anonymous namespace

bool PrintJob::run(HWND parent,
                   litepdf::core::Document& doc,
                   std::size_t /*active_page*/)
{
    if (!doc.is_open() || doc.page_count() == 0) {
        show_error(parent, L"No document is open.");
        return false;
    }

    // [1] Pre-flight scale picker.
    auto scale = PrintProgressDlg::show_config(parent);
    if (!scale) return false;

    // [2] OS PrintDlgEx (modern API; supports lpPageRanges).
    PRINTPAGERANGE ranges[10] = {};
    PRINTDLGEXW pd = {};
    pd.lStructSize    = sizeof(pd);
    pd.hwndOwner      = parent;
    pd.Flags          = PD_RETURNDC | PD_NOSELECTION
                      | PD_NOCURRENTPAGE
                      | PD_USEDEVMODECOPIESANDCOLLATE
                      | PD_COLLATE;
    pd.nMinPage       = 1;
    pd.nMaxPage       = static_cast<DWORD>(
                          std::min<std::size_t>(doc.page_count(), 0xFFFF));
    pd.nCopies        = 1;
    pd.lpPageRanges   = ranges;
    pd.nMaxPageRanges = 10;
    pd.nPageRanges    = 0;
    pd.nStartPage     = START_PAGE_GENERAL;

    HRESULT hr = PrintDlgExW(&pd);
    if (FAILED(hr)) {
        wchar_t buf[128];
        swprintf_s(buf, L"PrintDlgEx failed (hr=0x%08X).",
                   static_cast<unsigned>(hr));
        show_error(parent, buf);
        return false;
    }
    if (pd.dwResultAction != PD_RESULT_PRINT) {
        // PD_RESULT_CANCEL or PD_RESULT_APPLY -- silently return.
        if (pd.hDC)       DeleteDC(pd.hDC);
        if (pd.hDevMode)  GlobalFree(pd.hDevMode);
        if (pd.hDevNames) GlobalFree(pd.hDevNames);
        return false;
    }

    // RAII: PD_RETURNDC transferred HDC ownership to us.
    struct HdcOwner { HDC h; ~HdcOwner() { if (h) DeleteDC(h); } };
    HdcOwner hdc_owner{ pd.hDC };

    // RAII: PrintDlgEx allocates DEVMODE/DEVNAMES via GMEM_MOVEABLE.
    struct GMemOwner { HGLOBAL h; ~GMemOwner() { if (h) GlobalFree(h); } };
    GMemOwner devmode_owner { pd.hDevMode  };
    GMemOwner devnames_owner{ pd.hDevNames };

    // [3] Compute the page list.
    std::vector<std::size_t> pages;
    if (pd.Flags & PD_PAGENUMS) {
        std::vector<PageRange> pr = to_page_ranges(
            pd.lpPageRanges, static_cast<std::size_t>(pd.nPageRanges));
        pages = parse_page_ranges(pr.data(), pr.size(), doc.page_count());
    } else {
        pages.reserve(doc.page_count());
        for (std::size_t i = 0; i < doc.page_count(); ++i) pages.push_back(i);
    }
    if (pages.empty()) {
        show_error(parent, L"No pages selected.");
        return false;
    }

    // Copies: PrintDlgEx returns the resolved nCopies on the struct itself
    // when PD_USEDEVMODECOPIESANDCOLLATE is set.
    DWORD copies = std::max<DWORD>(1, pd.nCopies);

    // [4] StartDoc + abort callback registration (Task 8).
    PrintAbortFlag abort_flag;
    DOCINFOW di = { sizeof(di) };
    auto leaf = doc.source_path().filename().wstring();
    di.lpszDocName = leaf.empty() ? L"LitePDF" : leaf.c_str();
    if (StartDocW(pd.hDC, &di) <= 0) {
        show_error(parent, L"Failed to start print job.");
        return false;
    }
    g_active_abort_flag = &abort_flag;
    SetAbortProc(pd.hDC, abort_proc);
    struct AbortFlagGuard {
        ~AbortFlagGuard() { g_active_abort_flag = nullptr; }
    } abort_guard;

    // [5] Open a per-job MuPDF context + document (Task 7).
    PrintMupdfHandle mupdf(doc);
    if (!mupdf.valid()) {
        AbortDoc(pd.hDC);
        show_error(parent, L"Failed to open document for printing.");
        return false;
    }

    // [6] Page loop with MuPDF -> StretchDIBits.
    PrintProgressDlg progress(parent, abort_flag, pages.size() * copies);
    if (!progress.is_valid()) {
        AbortDoc(pd.hDC);
        show_error(parent, L"Failed to create print progress dialog.");
        return false;
    }
    std::size_t emitted = 0;
    bool error_aborted = false;
    for (DWORD c = 0; c < copies && !abort_flag.is_aborted() && !error_aborted; ++c) {
        for (std::size_t p : pages) {
            if (abort_flag.is_aborted()) break;
            ++emitted;
            progress.set_progress(emitted);
            if (StartPage(pd.hDC) <= 0) {
                AbortDoc(pd.hDC);
                show_error(parent, L"Printer reported an error mid-job.");
                error_aborted = true;
                break;
            }

            auto rr = render_page_to_pixmap(
                mupdf.ctx, mupdf.doc, p, pd.hDC,
                scale->mode, scale->custom_pct, /*auto_rotate*/true);
            if (!rr.pix) {
                AbortDoc(pd.hDC);
                wchar_t buf[96];
                swprintf_s(buf, L"Failed to render page %zu.", p + 1);
                show_error(parent, buf);
                error_aborted = true;
                break;
            }
            const bool ok = blit_pixmap_to_hdc(
                pd.hDC, rr.pix, mupdf.ctx, rr.dst_x, rr.dst_y);
            fz_drop_pixmap(mupdf.ctx, rr.pix);
            if (!ok) {
                AbortDoc(pd.hDC);
                show_error(parent, L"StretchDIBits failed.");
                error_aborted = true;
                break;
            }

            if (EndPage(pd.hDC) <= 0) {
                AbortDoc(pd.hDC);
                wchar_t buf[96];
                swprintf_s(buf, L"Printer reported error after page %zu.", p + 1);
                show_error(parent, buf);
                error_aborted = true;
                break;
            }
        }
    }

    if (error_aborted) return false;

    // [6] EndDoc / AbortDoc.
    if (abort_flag.is_aborted()) {
        AbortDoc(pd.hDC);
        return false;
    }
    EndDoc(pd.hDC);
    return true;
}

} // namespace litepdf::printing
