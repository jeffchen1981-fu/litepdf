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

#include <algorithm>
#include <cstdint>
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
    // call back into us during long blits). Re-post WM_QUIT so the host
    // message loop in MainWindow::run still sees app-shutdown requests
    // (taskbar X / Alt-F4 / system shutdown) that arrive mid-print.
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            PostQuitMessage(static_cast<int>(msg.wParam));
            if (g_active_abort_flag) g_active_abort_flag->request_abort();
            break;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (g_active_abort_flag && g_active_abort_flag->is_aborted())
           ? FALSE : TRUE;
}

// Blit a top-down BGRA bitmap (from Document::with_rendered_page) to the
// printer DC at (dst_x, dst_y). No fz_* calls -- buffer is already pure
// pixel data owned by the caller.
bool blit_bgra_to_hdc(HDC hdc, int w, int h, const std::uint8_t* bgra,
                      int dst_x, int dst_y) {
    if (!bgra || w <= 0 || h <= 0) return false;

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
        bgra, &bmi, DIB_RGB_COLORS, SRCCOPY);
    return rc != GDI_ERROR && rc != 0;
}

} // anonymous namespace

bool PrintJob::run(HWND parent,
                   const litepdf::core::Document& doc,
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
    // PrintDlgEx internally uses COM property sheets and per MSDN remarks
    // requires the calling thread to have called CoInitializeEx with
    // COINIT_APARTMENTTHREADED. Without it the dialog silently fails
    // (returns S_OK + PD_RESULT_CANCEL on some Windows versions, which
    // looks like "no dialog appears" to the user). The legacy PrintDlg
    // does NOT need COM; we hit this when switching to PrintDlgEx.
    struct ComScope {
        HRESULT init_hr;
        ComScope() {
            init_hr = CoInitializeEx(
                nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        }
        ~ComScope() {
            // Don't uninitialize if we hit RPC_E_CHANGED_MODE -- some other
            // thread/library set the apartment first; not ours to tear down.
            if (init_hr == S_OK || init_hr == S_FALSE) CoUninitialize();
        }
    } com_scope;

    PRINTPAGERANGE ranges[10] = {};
    PRINTDLGEXW pd = {};
    pd.lStructSize    = sizeof(pd);
    pd.hwndOwner      = parent;
    // PD_USEDEVMODECOPIESANDCOLLATE delegates copy multiplication and
    // collation order to the printer driver -- it sets DEVMODE.dmCopies
    // and dmCollate from the dialog, and we send the page sequence once.
    // PD_COLLATE pre-selects the Collate checkbox so the dialog UI
    // defaults to "Collate ON" matching the natural user expectation
    // (1,2,3,1,2,3 order). User can untick to get uncollated output.
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
        // RAII guards above (hdc_owner / devmode_owner / devnames_owner)
        // cover this early return. If a future refactor moves them, also
        // free pd.hDC + pd.hDevMode + pd.hDevNames before returning.
        show_error(parent, L"No pages selected.");
        return false;
    }

    // Copies: without PD_USEDEVMODECOPIESANDCOLLATE pd.nCopies holds the
    // user's choice. BUT some drivers (notably Microsoft Print to PDF)
    // ALSO populate DEVMODE.dmCopies regardless of the flag, and the
    // driver multiplies by dmCopies internally during printing -- if we
    // also loop nCopies times we get nCopies * dmCopies pages (4x for 2
    // copies). Force dmCopies=1 in DEVMODE and apply ResetDC so the HDC
    // honors it; we are the sole multiplier via our outer copies loop.
    // With PD_USEDEVMODECOPIESANDCOLLATE: pd.nCopies is always 1, and
    // copy count + collation order live in DEVMODE (dm->dmCopies,
    // dm->dmCollate). The driver applies both during print -- we send
    // the page sequence ONCE.
    //
    // PD_COLLATE pre-selects Collate ON in the dialog, but the Win11
    // modern Print dialog HIDES the Collate option entirely on many
    // drivers (Brother DCP-L2540DW, etc.) so the flag's pre-selection
    // is silently overridden by the driver's own default (FALSE -- gives
    // 1,1,2,2,3,3). Force dmCollate=TRUE post-dialog so the driver
    // emits collated output regardless of what the dialog returned.
    if (pd.hDevMode) {
        auto* dm = static_cast<DEVMODEW*>(GlobalLock(pd.hDevMode));
        if (dm) {
            dm->dmCollate = DMCOLLATE_TRUE;
            ResetDCW(pd.hDC, dm);
            GlobalUnlock(pd.hDevMode);
        }
    }

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

    // [5] Page loop. PD_USEDEVMODECOPIESANDCOLLATE means the driver
    // multiplies by dm->dmCopies and re-orders per dm->dmCollate; we
    // send the page sequence ONCE.
    PrintProgressDlg progress(parent, abort_flag, pages.size());
    if (!progress.is_valid()) {
        AbortDoc(pd.hDC);
        show_error(parent, L"Failed to create print progress dialog.");
        return false;
    }
    std::size_t emitted = 0;
    bool error_aborted = false;
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

        // FillRect white: see commit history -- "Microsoft Print to
        // PDF" leaves untouched DC areas BLACK; StretchDIBits only
        // paints the page-content rect.
        const int paper_w_px = GetDeviceCaps(pd.hDC, HORZRES);
        const int paper_h_px = GetDeviceCaps(pd.hDC, VERTRES);
        {
            RECT paper_rect{ 0, 0, paper_w_px, paper_h_px };
            FillRect(pd.hDC, &paper_rect,
                     reinterpret_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
        }

        // Compute matrix from page size + paper geometry.
        const auto ps = doc.page_size(p);
        const Rect page_rect_pt{ 0.0f, 0.0f, ps.width_pt, ps.height_pt };
        const Rect paper_rect_px{ 0.0f, 0.0f,
                                  static_cast<float>(paper_w_px),
                                  static_cast<float>(paper_h_px) };
        const float dpi_x = static_cast<float>(GetDeviceCaps(pd.hDC, LOGPIXELSX));
        const float dpi_y = static_cast<float>(GetDeviceCaps(pd.hDC, LOGPIXELSY));
        const auto pm = compute_render_matrix(
            page_rect_pt, paper_rect_px,
            scale->mode, scale->custom_pct,
            /*auto_rotate*/true, dpi_x, dpi_y);
        const int dst_x = static_cast<int>(pm.tx + 0.5f);
        const int dst_y = static_cast<int>(pm.ty + 0.5f);

        // Render + blit via Document escape hatch (uses Document's
        // own ctx + doc -- preserves auth + layout state).
        bool render_ok = false;
        const bool got_pix = doc.with_rendered_page(
            p, pm.scale_x, pm.scale_y, pm.rotate_90,
            [&](int w, int h, const std::uint8_t* bgra) {
                render_ok = blit_bgra_to_hdc(
                    pd.hDC, w, h, bgra, dst_x, dst_y);
            });
        if (!got_pix || !render_ok) {
            AbortDoc(pd.hDC);
            wchar_t buf[96];
            swprintf_s(buf, L"Failed to render page %zu.", p + 1);
            show_error(parent, buf);
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
