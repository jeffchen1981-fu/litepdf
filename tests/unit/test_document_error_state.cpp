// #146: a Document catch that swallows a MuPDF failure must also mark the
// error handled. MuPDF keeps a caught error's code on the context until a later
// catch overwrites it; a throw that comes first, or the context's
// fz_drop_context, reports the old error again as "UNHANDLED EXCEPTION!"
// (error.c, fz_vthrow; context.c, fz_drop_context).
//
// Most cases reach the Document's own context through its MuPDFRoot, install a
// warning callback there, and throw on it straight after the failed call, with
// no MuPDF call in between. A failed open() leaves no clone to reach the root
// through, so that case captures stderr across the Document's destruction.
#include "core/Document.hpp"
#include "core/EscrowContext.hpp"
#include "core/MuPDFRoot.hpp"

#include <catch2/catch_test_macros.hpp>

#include <mupdf/fitz.h>

#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>

using namespace litepdf::core;

namespace {

constexpr const char* kUnhandled = "UNHANDLED EXCEPTION!";

// Collects the warnings MuPDF raises on an open Document's own context, and
// throws on that context on request. Restores the previous callback when it
// goes. Declare it after the Document it watches, so it goes first.
class RootWarnings {
public:
    explicit RootWarnings(const Document& doc) {
        const EscrowContext esc = doc.clone_escrow_context();
        REQUIRE(esc.valid());
        root_ = detail::MuPDFRoot::of(esc.get());
        REQUIRE(root_ != nullptr);
        prev_ = fz_warning_callback(root_->ctx, &prev_user_);
        fz_set_warning_callback(root_->ctx, [](void* user, const char* message) noexcept {
            *static_cast<std::string*>(user) += std::string(message) + "\n";
        }, &text_);
    }
    ~RootWarnings() { fz_set_warning_callback(root_->ctx, prev_, prev_user_); }
    RootWarnings(const RootWarnings&) = delete;
    RootWarnings& operator=(const RootWarnings&) = delete;

    // Throws and catches a fresh error on the Document's context: a stale code
    // left by the call before it is reported as "UNHANDLED EXCEPTION!".
    void throw_on_context() {
        fz_context* ctx = root_->ctx;
        bool caught = false;
        fz_try(ctx) {
            fz_throw(ctx, FZ_ERROR_GENERIC, "#146 probe");
        }
        fz_catch(ctx) {
            caught = true;
            fz_convert_error(ctx, nullptr);
        }
        REQUIRE(caught);
    }

    const std::string& text() const { return text_; }

private:
    std::shared_ptr<detail::MuPDFRoot> root_;
    fz_warning_cb* prev_ = nullptr;
    void* prev_user_ = nullptr;
    std::string text_;
};

// Redirects fd 2 (MuPDF's default warning and error callbacks write to the
// CRT's stderr) into a temporary file until finish(). The destructor restores
// fd 2 if a REQUIRE fails first, so Catch2's own report still reaches stderr.
class StderrCapture {
public:
    explicit StderrCapture(const char* name)
        : path_(std::filesystem::temp_directory_path() / name) {
        std::fflush(stderr);
        saved_ = _dup(2);
        REQUIRE(saved_ != -1);
        const int fd = _wopen(path_.c_str(), _O_WRONLY | _O_CREAT | _O_TRUNC,
                              _S_IREAD | _S_IWRITE);
        REQUIRE(fd != -1);
        const int redirected = _dup2(fd, 2);
        _close(fd);
        REQUIRE(redirected == 0);
    }
    ~StderrCapture() {
        restore();
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }
    StderrCapture(const StderrCapture&) = delete;
    StderrCapture& operator=(const StderrCapture&) = delete;

    std::string finish() {
        restore();
        std::ifstream in(path_, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), {});
    }

private:
    void restore() {
        if (saved_ == -1) return;
        std::fflush(stderr);
        _dup2(saved_, 2);
        _close(saved_);
        saved_ = -1;
    }

    std::filesystem::path path_;
    int saved_ = -1;
};

// A one-page PDF whose /Outlines /First is a direct dictionary. MuPDF checks
// the outline tree before walking it and throws "Non-indirect outline entry
// discovered" (pdf-outline.c), so fz_load_outline fails. Written with a correct
// xref so opening it needs no repair.
class BadOutlinePdf {
public:
    BadOutlinePdf()
        : path_(std::filesystem::temp_directory_path() /
                "litepdf-146-bad-outline.pdf") {
        const std::vector<std::string> objects = {
            "<< /Type /Catalog /Pages 2 0 R /Outlines 4 0 R >>",
            "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
            "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] >>",
            "<< /Type /Outlines /First << /Title (direct) >> /Count 1 >>",
        };
        std::ostringstream pdf;
        pdf << "%PDF-1.4\n";
        std::vector<std::streamoff> offsets;
        for (std::size_t i = 0; i < objects.size(); ++i) {
            offsets.push_back(pdf.tellp());
            pdf << (i + 1) << " 0 obj\n" << objects[i] << "\nendobj\n";
        }
        const std::streamoff xref = pdf.tellp();
        pdf << "xref\n0 " << (objects.size() + 1) << "\n"
            << "0000000000 65535 f \n";
        for (const auto off : offsets) {
            char line[21];
            std::snprintf(line, sizeof line, "%010lld 00000 n \n",
                          static_cast<long long>(off));
            pdf << line;
        }
        pdf << "trailer\n<< /Size " << (objects.size() + 1)
            << " /Root 1 0 R >>\nstartxref\n" << xref << "\n%%EOF\n";

        std::ofstream out(path_, std::ios::binary | std::ios::trunc);
        out << pdf.str();
    }
    ~BadOutlinePdf() {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }
    BadOutlinePdf(const BadOutlinePdf&) = delete;
    BadOutlinePdf& operator=(const BadOutlinePdf&) = delete;

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

Document open_fixture(const std::filesystem::path& path) {
    Document doc;
    REQUIRE_FALSE(doc.open(path).has_value());
    return doc;
}

}  // namespace

TEST_CASE("Document error state: failed outline load is marked handled",
          "[document][outline]") {
    const BadOutlinePdf fixture;
    const Document doc = open_fixture(fixture.path());
    RootWarnings warnings(doc);

    REQUIRE(doc.outline().empty());
    warnings.throw_on_context();

    INFO("warnings:\n" << warnings.text());
    REQUIRE(warnings.text().find(kUnhandled) == std::string::npos);
}

TEST_CASE("Document error state: a regex that does not compile is marked handled",
          "[document][search]") {
    const Document doc = open_fixture("tests/fixtures/search.pdf");
    RootWarnings warnings(doc);

    Document::SearchFlags regex{};
    regex.regex = true;
    REQUIRE_FALSE(doc.query_compiles("foo(", regex));
    warnings.throw_on_context();

    INFO("warnings:\n" << warnings.text());
    REQUIRE(warnings.text().find(kUnhandled) == std::string::npos);
}

TEST_CASE("Document error state: failed page_size and page_text are marked handled",
          "[document][pages]") {
    const Document doc = open_fixture("tests/fixtures/simple.pdf");
    RootWarnings warnings(doc);

    REQUIRE_THROWS_AS(doc.page_size(9999), std::runtime_error);
    warnings.throw_on_context();
    REQUIRE_THROWS_AS(doc.page_text(9999), std::runtime_error);
    warnings.throw_on_context();

    INFO("warnings:\n" << warnings.text());
    REQUIRE(warnings.text().find(kUnhandled) == std::string::npos);
}

TEST_CASE("Document error state: failed open is marked handled before the context drops",
          "[document][open]") {
    StderrCapture capture("litepdf-146-open.log");
    {
        Document doc;
        const auto err = doc.open("tests/fixtures/corrupt.pdf");
        REQUIRE(err.has_value());
        REQUIRE(*err == Document::OpenError::Corrupted);
    }  // ~Document: no document to drop, so the root context drops next
    const std::string err = capture.finish();

    INFO("stderr:\n" << err);
    REQUIRE(err.find(kUnhandled) == std::string::npos);
}
