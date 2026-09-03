// =============================================================================
// tests/test_file_tools.cpp — fs_guard + read_file/write_file
// =============================================================================

#include "tools.h"
#include "tools/fs_guard.h"
#include <filesystem>
#include <fstream>
#include <iostream>

namespace fs = std::filesystem;

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::cerr << "FAILED at " << __FILE__ << ":" << __LINE__ << " — " #cond "\n"; \
        return 1; \
    } \
} while (0)

int test_fs_guard() {
    fs::path ws = fs::temp_directory_path() / "funes_test_fsguard_ws";
    fs::remove_all(ws);
    fs::create_directories(ws / "sub");
    std::ofstream(ws / "sub" / "existing.txt") << "hi";

    // Inside the workspace, relative.
    auto ok = funes::fsguard::resolve(ws, "sub/existing.txt");
    CHECK(ok.has_value());
    CHECK(fs::weakly_canonical(*ok) == fs::weakly_canonical(ws / "sub" / "existing.txt"));

    // A path that doesn't exist yet is still fine (needed for write_file).
    auto new_file = funes::fsguard::resolve(ws, "brand/new/file.txt");
    CHECK(new_file.has_value());

    // Escapes via "..".
    CHECK(!funes::fsguard::resolve(ws, "../outside.txt").has_value());
    CHECK(!funes::fsguard::resolve(ws, "sub/../../outside.txt").has_value());
    CHECK(!funes::fsguard::resolve(ws, "../../../../../../etc/passwd").has_value());

    // Absolute path outside the workspace.
    CHECK(!funes::fsguard::resolve(ws, "/etc/passwd").has_value());

    // Absolute path that happens to be inside the workspace is fine.
    auto abs_inside = funes::fsguard::resolve(ws, (ws / "sub" / "existing.txt").string());
    CHECK(abs_inside.has_value());

    // Empty path is rejected.
    CHECK(!funes::fsguard::resolve(ws, "").has_value());

    fs::remove_all(ws);
    return 0;
}

int test_read_write_file() {
    fs::path ws = fs::temp_directory_path() / "funes_test_file_tools_ws";
    fs::remove_all(ws);

    ToolRegistry reg;
    register_file_tools(reg, ws.string());
    CHECK(reg.has("read_file") && reg.has("write_file"));

    ToolContext ctx{"funes", "s1"};

    // write_file creates parent dirs and the file.
    auto w1 = reg.call("write_file", {{"path", "notes/todo.txt"}, {"content", "buy milk"}}, ctx);
    CHECK(!w1.error);
    // 4.0: read_file/write_file operate inside <root>/<user_id>.
    CHECK(fs::exists(ws / "1" / "notes" / "todo.txt"));

    // read_file reads it back.
    auto r1 = reg.call("read_file", {{"path", "notes/todo.txt"}}, ctx);
    CHECK(!r1.error);
    CHECK(r1.text == "buy milk");

    // append=true adds rather than replacing.
    auto w2 = reg.call("write_file", {{"path", "notes/todo.txt"}, {"content", "\nwalk dog"}, {"append", true}}, ctx);
    CHECK(!w2.error);
    auto r2 = reg.call("read_file", {{"path", "notes/todo.txt"}}, ctx);
    CHECK(r2.text == "buy milk\nwalk dog");

    // default (append=false) overwrites.
    auto w3 = reg.call("write_file", {{"path", "notes/todo.txt"}, {"content", "fresh start"}}, ctx);
    CHECK(!w3.error);
    auto r3 = reg.call("read_file", {{"path", "notes/todo.txt"}}, ctx);
    CHECK(r3.text == "fresh start");

    // Traversal is refused for both tools.
    auto w4 = reg.call("write_file", {{"path", "../escape.txt"}, {"content", "x"}}, ctx);
    CHECK(w4.error);
    auto r4 = reg.call("read_file", {{"path", "../../etc/passwd"}}, ctx);
    CHECK(r4.error);

    // Missing file.
    auto r5 = reg.call("read_file", {{"path", "does/not/exist.txt"}}, ctx);
    CHECK(r5.error);

    // Binary content that isn't a recognized image format is rejected, not
    // dumped as garbage.
    {
        fs::create_directories(ws / "1");
        std::ofstream bin(ws / "1" / "unknown.bin", std::ios::binary);
        bin.write("\x00\x01\x02\x03NOTANIMAGE\x00\x01", 15);
    }
    auto r6 = reg.call("read_file", {{"path", "unknown.bin"}}, ctx);
    CHECK(r6.error);
    CHECK(r6.text.find("binary") != std::string::npos);

    // A recognized image format is handed back as an image, not rejected.
    {
        std::ofstream img(ws / "1" / "photo.jpg", std::ios::binary);
        img.write("\xff\xd8\xff\xe0\x00\x10JFIF\x00\x01", 12);
    }
    auto r7 = reg.call("read_file", {{"path", "photo.jpg"}}, ctx);
    CHECK(!r7.error);
    CHECK(r7.images.size() == 1);
    CHECK(r7.images[0].mime_type == "image/jpeg");

    // Content over the write cap is refused.
    auto w5 = reg.call("write_file", {{"path", "big.txt"}, {"content", std::string(300000, 'x')}}, ctx);
    CHECK(w5.error);

    fs::remove_all(ws);
    return 0;
}

// Minimal hand-built single-page PDF whose content stream is just
// "Hello PDF World" — enough for pdftotext to extract real text from
// without pulling in a PDF-generation dependency for the test.
constexpr const char* kMinimalPdf =
    "%PDF-1.4\n"
    "1 0 obj<</Type/Catalog/Pages 2 0 R>>endobj\n"
    "2 0 obj<</Type/Pages/Kids[3 0 R]/Count 1>>endobj\n"
    "3 0 obj<</Type/Page/Parent 2 0 R/MediaBox[0 0 200 100]"
    "/Resources<</Font<</F1 4 0 R>>>>/Contents 5 0 R>>endobj\n"
    "4 0 obj<</Type/Font/Subtype/Type1/BaseFont/Helvetica>>endobj\n"
    "5 0 obj<</Length 44>>stream\n"
    "BT /F1 12 Tf 10 50 Td (Hello PDF World) Tj ET\n"
    "endstream\n"
    "endobj\n"
    "xref\n"
    "0 6\n"
    "trailer<</Size 6/Root 1 0 R>>\n"
    "%%EOF";

int test_pdf_extraction() {
    fs::path ws = fs::temp_directory_path() / "funes_test_pdf_ws";
    fs::remove_all(ws);
    fs::create_directories(ws);
    {
        fs::create_directories(ws / "1");
        std::ofstream f(ws / "1" / "doc.pdf", std::ios::binary);
        f << kMinimalPdf;
    }

    ToolRegistry reg;
    register_file_tools(reg, ws.string());
    ToolContext ctx{"funes", "s1"};

    auto r = reg.call("read_file", {{"path", "doc.pdf"}}, ctx);
    if (r.error && r.text.find("'pdftotext' isn't installed") != std::string::npos) {
        std::cerr << "test_pdf_extraction: pdftotext not installed, skipping content check\n";
        fs::remove_all(ws);
        return 0;
    }
    CHECK(!r.error);
    CHECK(r.text.find("Hello PDF World") != std::string::npos);

    // A non-existent PDF still gets a clear error, not a crash.
    auto missing = reg.call("read_file", {{"path", "nope.pdf"}}, ctx);
    CHECK(missing.error);

    fs::remove_all(ws);
    return 0;
}

// A single blank page with no content stream at all — pdftotext still
// emits a lone form-feed (page break) for it, which must not be mistaken
// for real extracted text (this is what image-only/scanned PDFs look like).
constexpr const char* kBlankPagePdf =
    "%PDF-1.4\n"
    "1 0 obj<</Type/Catalog/Pages 2 0 R>>endobj\n"
    "2 0 obj<</Type/Pages/Kids[3 0 R]/Count 1>>endobj\n"
    "3 0 obj<</Type/Page/Parent 2 0 R/MediaBox[0 0 200 100]>>endobj\n"
    "xref\n"
    "0 4\n"
    "trailer<</Size 4/Root 1 0 R>>\n"
    "%%EOF";

// A page with no text layer must fall back to rendered-image results
// instead of silently succeeding with a bare form-feed (the bug) or
// dead-ending the model with an unreadable error (the old fallback-less
// behavior) — read_file should still return something the model can use.
int test_pdf_no_text_extraction() {
    fs::path ws = fs::temp_directory_path() / "funes_test_pdf_blank_ws";
    fs::remove_all(ws);
    fs::create_directories(ws);
    {
        fs::create_directories(ws / "1");
        std::ofstream f(ws / "1" / "blank.pdf", std::ios::binary);
        f << kBlankPagePdf;
    }

    ToolRegistry reg;
    register_file_tools(reg, ws.string());
    ToolContext ctx{"funes", "s1"};

    auto r = reg.call("read_file", {{"path", "blank.pdf"}}, ctx);
    if (r.error && r.text.find("'pdftotext' isn't installed") != std::string::npos) {
        std::cerr << "test_pdf_no_text_extraction: pdftotext not installed, skipping\n";
        fs::remove_all(ws);
        return 0;
    }
    if (r.error && r.text.find("pdftoppm") != std::string::npos &&
        r.text.find("isn't installed") != std::string::npos) {
        std::cerr << "test_pdf_no_text_extraction: pdftoppm not installed, skipping\n";
        fs::remove_all(ws);
        return 0;
    }
    CHECK(!r.error);
    CHECK(r.text.find("no text layer") != std::string::npos);
    CHECK(!r.images.empty());
    CHECK(r.images[0].mime_type == "image/png");
    CHECK(!r.images[0].base64_data.empty());

    fs::remove_all(ws);
    return 0;
}

int test_list_files() {
    fs::path ws = fs::temp_directory_path() / "funes_test_list_files";
    fs::remove_all(ws);
    fs::create_directories(ws / "1" / "subdir");
    std::ofstream(ws / "1" / "a.txt") << "hello";
    std::ofstream(ws / "1" / "b.txt") << "world";
    std::ofstream(ws / "1" / ".hidden") << "skip";

    ToolRegistry reg;
    register_file_tools(reg, ws.string());
    ToolContext ctx{"funes", "s1"};

    auto r = reg.call("list_files", json::object(), ctx);
    CHECK(!r.error);
    CHECK(r.text.find("subdir/") != std::string::npos);
    CHECK(r.text.find("a.txt") != std::string::npos);
    CHECK(r.text.find("b.txt") != std::string::npos);
    CHECK(r.text.find(".hidden") == std::string::npos);

    auto r2 = reg.call("list_files", {{"path", "subdir"}}, ctx);
    CHECK(!r2.error);
    CHECK(r2.text.find("empty") != std::string::npos || r2.text.find("No files") != std::string::npos);

    auto r3 = reg.call("list_files", {{"path", "../../etc"}}, ctx);
    CHECK(r3.error);

    fs::remove_all(ws);
    return 0;
}

int main() {
    int rc = 0;
    rc |= test_fs_guard();
    rc |= test_read_write_file();
    rc |= test_list_files();
    rc |= test_pdf_extraction();
    rc |= test_pdf_no_text_extraction();
    if (rc == 0) std::cout << "test_file_tools: all tests passed\n";
    return rc;
}
