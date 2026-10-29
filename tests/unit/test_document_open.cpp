#include "core/Document.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>
#include <system_error>

using namespace litepdf::core;

namespace {

// A copy of simple.pdf under the temp directory, at a caller-chosen name.
class TempPdfCopy {
public:
    explicit TempPdfCopy(const std::wstring& name)
        : path_(std::filesystem::temp_directory_path() / name) {
        std::filesystem::copy_file("tests/fixtures/simple.pdf", path_,
                                   std::filesystem::copy_options::overwrite_existing);
    }
    ~TempPdfCopy() {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }
    TempPdfCopy(const TempPdfCopy&) = delete;
    TempPdfCopy& operator=(const TempPdfCopy&) = delete;

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

// An unpaired UTF-16 surrogate: NTFS accepts it in a file name, but it has no
// UTF-8 form, so path::u8string() throws std::system_error (MSVC's STL
// converts with WC_ERR_INVALID_CHARS).
constexpr wchar_t kLoneSurrogate = static_cast<wchar_t>(0xD800);

} // namespace

TEST_CASE("Document::open succeeds on a simple PDF", "[document][open]") {
    Document doc;
    REQUIRE_FALSE(doc.is_open());

    auto err = doc.open("tests/fixtures/simple.pdf");
    INFO("OpenError value: " << (err ? static_cast<int>(*err) : -1));
    REQUIRE_FALSE(err.has_value());
    REQUIRE(doc.is_open());
}

TEST_CASE("Document::open returns FileNotFound for missing file", "[document][open]") {
    Document doc;
    auto err = doc.open("tests/fixtures/does_not_exist.pdf");
    REQUIRE(err.has_value());
    REQUIRE(*err == Document::OpenError::FileNotFound);
    REQUIRE_FALSE(doc.is_open());
}

TEST_CASE("Document::open returns UnsupportedFormat for PNG", "[document][open]") {
    Document doc;
    auto err = doc.open("tests/fixtures/sample.png");
    REQUIRE(err.has_value());
    REQUIRE(*err == Document::OpenError::UnsupportedFormat);
    REQUIRE_FALSE(doc.is_open());
}

TEST_CASE("Document::open returns Corrupted for truncated PDF", "[document][open]") {
    Document doc;
    auto err = doc.open("tests/fixtures/corrupt.pdf");
    REQUIRE(err.has_value());
    REQUIRE(*err == Document::OpenError::Corrupted);
    REQUIRE_FALSE(doc.is_open());
}

// The open runs on a std::thread with no catch (MainWindow::open_tab_async), so
// a throw here terminated the app. It must come back as an error the open
// dialog can name instead.
TEST_CASE("Document::open returns UnconvertiblePath for a file name with a lone surrogate",
          "[document][open]") {
    const TempPdfCopy fixture(std::wstring(L"litepdf-name-") + kLoneSurrogate + L".pdf");
    REQUIRE(std::filesystem::exists(fixture.path()));
    REQUIRE_THROWS_AS(fixture.path().u8string(), std::system_error);

    Document doc;
    std::optional<Document::OpenError> err;
    REQUIRE_NOTHROW(err = doc.open(fixture.path()));
    REQUIRE(err.has_value());
    REQUIRE(*err == Document::OpenError::UnconvertiblePath);
    REQUIRE_FALSE(doc.is_open());
    REQUIRE(doc.source_path().empty());

    // The failure leaves the Document reusable.
    REQUIRE_FALSE(doc.open("tests/fixtures/simple.pdf").has_value());
    REQUIRE(doc.is_open());
}

// The conversion covers the WHOLE path, so a good file name inside a folder
// whose name has no UTF-8 form is refused the same way (#184). The open dialog
// tells the two apart by the path; this pins the error it is given.
TEST_CASE("Document::open returns UnconvertiblePath for a folder name with a lone surrogate",
          "[document][open]") {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path()
                       / (std::wstring(L"litepdf-folder-") + kLoneSurrogate);
    fs::create_directories(dir);
    struct RemoveDir {
        fs::path dir;
        ~RemoveDir() {
            std::error_code ec;
            fs::remove_all(dir, ec);
        }
    } remove_dir{dir};
    const fs::path file = dir / L"simple.pdf";
    fs::copy_file("tests/fixtures/simple.pdf", file, fs::copy_options::overwrite_existing);
    REQUIRE(fs::exists(file));
    REQUIRE_NOTHROW(file.filename().u8string());
    REQUIRE_THROWS_AS(file.u8string(), std::system_error);

    Document doc;
    std::optional<Document::OpenError> err;
    REQUIRE_NOTHROW(err = doc.open(file));
    REQUIRE(err.has_value());
    REQUIRE(*err == Document::OpenError::UnconvertiblePath);
    REQUIRE_FALSE(doc.is_open());
}

// The extension check runs before the path is converted for MuPDF, on the
// extension alone; it must not throw on the same character either.
TEST_CASE("Document::open returns UnsupportedFormat for an extension with a lone surrogate",
          "[document][open]") {
    const TempPdfCopy fixture(std::wstring(L"litepdf-ext.pd") + kLoneSurrogate);
    REQUIRE(std::filesystem::exists(fixture.path()));
    REQUIRE_THROWS_AS(fixture.path().extension().u8string(), std::system_error);

    Document doc;
    std::optional<Document::OpenError> err;
    REQUIRE_NOTHROW(err = doc.open(fixture.path()));
    REQUIRE(err.has_value());
    REQUIRE(*err == Document::OpenError::UnsupportedFormat);
    REQUIRE_FALSE(doc.is_open());
}

TEST_CASE("Document is safe to use after being moved from", "[document][move]") {
    Document a;
    REQUIRE_FALSE(a.open("tests/fixtures/simple.pdf").has_value());

    Document b = std::move(a);
    REQUIRE(b.is_open());

    // a is moved-from; must be safe to query (returns false) and reusable.
    REQUIRE_FALSE(a.is_open());              // does NOT crash
    REQUIRE_FALSE(a.open("tests/fixtures/simple.pdf").has_value());  // reusable
    REQUIRE(a.is_open());
}
