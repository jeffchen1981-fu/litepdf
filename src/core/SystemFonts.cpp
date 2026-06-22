// src/core/SystemFonts.cpp
#include "core/SystemFonts.hpp"

extern "C" {
#include <mupdf/fitz.h>
}

#include <windows.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace litepdf::core {

std::vector<std::wstring> cjk_family_candidates(int ordering, int serif) {
    switch (ordering) {  // FZ_ADOBE_CNS=0, GB=1, JAPAN=2, KOREA=3
        case 0:
            return serif ? std::vector<std::wstring>{L"PMingLiU", L"MingLiU"}
                         : std::vector<std::wstring>{L"Microsoft JhengHei",
                                                     L"Microsoft JhengHei UI"};
        case 1:
            return serif ? std::vector<std::wstring>{L"SimSun", L"NSimSun"}
                         : std::vector<std::wstring>{L"Microsoft YaHei",
                                                     L"Microsoft YaHei UI"};
        case 2:
            return serif ? std::vector<std::wstring>{L"MS Mincho", L"MS PMincho"}
                         : std::vector<std::wstring>{L"Yu Gothic", L"MS Gothic",
                                                     L"Meiryo"};
        case 3:
            return serif ? std::vector<std::wstring>{L"Batang", L"BatangChe"}
                         : std::vector<std::wstring>{L"Malgun Gothic", L"Gulim"};
        default:
            return {};
    }
}

namespace {
using Microsoft::WRL::ComPtr;

// Process-lifetime SHARED DirectWrite factory (free-threaded; created once).
// DirectWrite needs no CoInitialize (unlike the PrintDlgEx path).
IDWriteFactory* dwrite_factory() {
    static IDWriteFactory* factory = [] {
        IDWriteFactory* f = nullptr;
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                            reinterpret_cast<IUnknown**>(&f));
        return f;  // process-lifetime leak is acceptable
    }();
    return factory;
}

struct ResolvedFace { std::string path; int index = 0; bool ok = false; };

std::mutex g_cache_mtx;
std::unordered_map<int, ResolvedFace> g_cache;  // key = ordering*2 + serif

// Resolve ONE family name -> (utf8 path, ttc face index) via DirectWrite.
bool resolve_family_path(const std::wstring& family, std::string& out_path,
                         int& out_index) {
    IDWriteFactory* factory = dwrite_factory();
    if (!factory) return false;
    ComPtr<IDWriteFontCollection> coll;
    if (FAILED(factory->GetSystemFontCollection(&coll, FALSE))) return false;
    UINT32 idx = 0;
    BOOL exists = FALSE;
    if (FAILED(coll->FindFamilyName(family.c_str(), &idx, &exists)) || !exists)
        return false;
    ComPtr<IDWriteFontFamily> fam;
    if (FAILED(coll->GetFontFamily(idx, &fam))) return false;
    ComPtr<IDWriteFont> font;
    if (FAILED(fam->GetFirstMatchingFont(DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, DWRITE_FONT_STYLE_NORMAL, &font)))
        return false;
    ComPtr<IDWriteFontFace> face;
    if (FAILED(font->CreateFontFace(&face))) return false;
    UINT32 nfiles = 1;
    ComPtr<IDWriteFontFile> file;  // MUST stay alive until GetFilePathFromKey
    if (FAILED(face->GetFiles(&nfiles, file.GetAddressOf())) || nfiles != 1)
        return false;
    const void* key = nullptr;
    UINT32 keySize = 0;
    if (FAILED(file->GetReferenceKey(&key, &keySize))) return false;
    ComPtr<IDWriteFontFileLoader> loader;
    if (FAILED(file->GetLoader(&loader))) return false;
    ComPtr<IDWriteLocalFontFileLoader> local;
    if (FAILED(loader.As(&local))) return false;  // non-local -> skip candidate
    UINT32 len = 0;
    if (FAILED(local->GetFilePathLengthFromKey(key, keySize, &len))) return false;
    std::wstring wpath(static_cast<size_t>(len) + 1, L'\0');
    if (FAILED(local->GetFilePathFromKey(key, keySize, wpath.data(), len + 1)))
        return false;
    wpath.resize(len);
    out_index = static_cast<int>(face->GetIndex());
    // Convert with the EXPLICIT length (excludes the NUL) so the output buffer
    // is sized exactly and we never write a NUL into out_path[size()] (which is
    // UB on std::string). Codex BLOCKER round-1.
    const int wlen = static_cast<int>(wpath.size());
    int n = WideCharToMultiByte(CP_UTF8, 0, wpath.c_str(), wlen, nullptr, 0,
                                nullptr, nullptr);
    if (n <= 0) return false;
    out_path.resize(static_cast<size_t>(n));
    WideCharToMultiByte(CP_UTF8, 0, wpath.c_str(), wlen, out_path.data(), n,
                        nullptr, nullptr);
    return true;
}

// Cached resolution. Double-checked: the mutex is NOT held across the slow
// DirectWrite enumeration (release on miss, re-lock to insert).
ResolvedFace resolve_cached(int ordering, int serif) {
    const int key = ordering * 2 + (serif ? 1 : 0);
    {
        std::lock_guard<std::mutex> lk(g_cache_mtx);
        auto it = g_cache.find(key);
        if (it != g_cache.end()) return it->second;
    }
    ResolvedFace r;
    for (const std::wstring& fam : cjk_family_candidates(ordering, serif))
        if (resolve_family_path(fam, r.path, r.index)) { r.ok = true; break; }
    if (!r.ok)  // fall through to the other style before giving up
        for (const std::wstring& fam : cjk_family_candidates(ordering, serif ? 0 : 1))
            if (resolve_family_path(fam, r.path, r.index)) { r.ok = true; break; }
    {
        std::lock_guard<std::mutex> lk(g_cache_mtx);
        g_cache[key] = r;  // a redundant concurrent compute is harmless
    }
    return r;
}
}  // namespace

fz_font* resolve_cjk_system_font(fz_context* ctx, const char* name, int ordering,
                                 int serif) noexcept {
    try {
        ResolvedFace r = resolve_cached(ordering, serif);
        if (r.ok) {
            fz_font* font = nullptr;
            fz_var(font);  // mutated in fz_try, read after fz_catch (project convention)
            fz_try(ctx) {
                font = fz_new_font_from_file(ctx, name, r.path.c_str(), r.index, 0);
            }
            fz_catch(ctx) { font = nullptr; }  // fall through to last resort
            if (font) return font;
        }
        // D6 last resort: a guaranteed base14 face so the hook never returns
        // NULL (a cheap never-NULL invariant). A NULL return makes fz_new_cjk_font
        // throw FZ_ERROR_ARGUMENT (font.c:967); in form/annotation appearance
        // synthesis that throw is swallowed (pdf-appearance.c:3815), leaving the
        // CJK form field EMPTY (not a blank page). Returning base14 renders that
        // field as .notdef tofu instead. Catch ALL fz errors here (incl.
        // FZ_ERROR_SYSTEM/TRYLATER, which fz_load_system_cjk_font would otherwise
        // rethrow) and return NULL only past the last resort.
        fz_font* fallback = nullptr;
        fz_var(fallback);
        fz_try(ctx) {
            int len = 0;
            const unsigned char* data = fz_lookup_base14_font(ctx, "Helvetica", &len);
            if (data) fallback = fz_new_font_from_memory(ctx, name, data, len, 0, 0);
        }
        fz_catch(ctx) { fallback = nullptr; }
        return fallback;
    } catch (...) {
        return nullptr;  // no C++ exception may cross into MuPDF's C frame
    }
}

void install_system_cjk_font_loader(fz_context* ctx) noexcept {
    // Install only the CJK hook; load_font / load_fallback_font stay NULL (D2).
    fz_install_load_system_font_funcs(ctx, nullptr, &resolve_cjk_system_font,
                                      nullptr);
}

}  // namespace litepdf::core
