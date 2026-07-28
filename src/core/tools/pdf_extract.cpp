// =============================================================================
// src/core/tools/pdf_extract.cpp — PDF text extraction via pdftotext
// =============================================================================

#include "pdf_extract.h"
#include "../base64.h"
#include "../text_utils.h"
#include "process_runner.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace funes::pdf {

namespace {

// pdftotext emits a lone form-feed (page break) per page even when a page
// has no text at all, so the raw output is never truly empty for image-only
// PDFs — check whether anything other than whitespace/form-feeds came out.
bool has_visible_text(const std::string& s) {
    return std::any_of(s.begin(), s.end(), [](unsigned char c) {
        return !std::isspace(c) && c != '\f';
    });
}

} // namespace

ExtractResult extract_text(const std::filesystem::path& pdf_path,
                           const std::filesystem::path& cwd,
                           int timeout_seconds, size_t max_bytes) {
    proc::Result r = proc::run_argv({"pdftotext", pdf_path.string(), "-"},
                                    cwd, timeout_seconds, max_bytes);

    if (r.exit_code == 126 || r.exit_code == 127)
        return {false, "Can't extract text from '" + pdf_path.string() + "': 'pdftotext' isn't "
                "installed (install poppler-utils, e.g. `apt install poppler-utils`)."};
    if (r.exit_code != 0)
        return {false, "pdftotext failed on '" + pdf_path.string() + "' (exit " +
                std::to_string(r.exit_code) + "): " + r.output};

    if (!looks_like_text(r.output))
        return {false, "Extracted text from '" + pdf_path.string() + "' isn't valid UTF-8 — "
                "can't return it."};
    if (!has_visible_text(r.output))
        return {false, "pdftotext extracted no text from '" + pdf_path.string() +
                "' (it may be a scanned/image-only PDF)."};

    return {true, r.output};
}

RenderResult render_pages_as_images(const fs::path& pdf_path, const fs::path& cwd,
                                    int timeout_seconds, int max_pages) {
    RenderResult result;
    std::error_code ec;

    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path tmp_dir = fs::temp_directory_path(ec) / ("funes_pdfrender_" + std::to_string(stamp));
    fs::create_directories(tmp_dir, ec);
    fs::path prefix = tmp_dir / "page";

    // File-based output (not `-` for stdout): pdftoppm can write warnings to
    // stderr, and run_argv combines stdout+stderr into one pipe, which would
    // corrupt binary PNG bytes read off stdout.
    proc::Result r = proc::run_argv(
        {"pdftoppm", "-png", "-r", "150", "-f", "1", "-l", std::to_string(max_pages),
         pdf_path.string(), prefix.string()},
        cwd, timeout_seconds, 64 * 1024);

    if (r.exit_code == 126 || r.exit_code == 127) {
        fs::remove_all(tmp_dir, ec);
        result.error = "Can't render '" + pdf_path.string() + "': 'pdftoppm' isn't installed "
                        "(install poppler-utils, e.g. `apt install poppler-utils`).";
        return result;
    }
    if (r.exit_code != 0) {
        fs::remove_all(tmp_dir, ec);
        result.error = "pdftoppm failed on '" + pdf_path.string() + "' (exit " +
                        std::to_string(r.exit_code) + "): " + r.output;
        return result;
    }

    std::vector<fs::path> pages;
    for (auto& entry : fs::directory_iterator(tmp_dir, ec))
        if (entry.path().extension() == ".png") pages.push_back(entry.path());
    std::sort(pages.begin(), pages.end());

    for (auto& page_path : pages) {
        std::ifstream f(page_path, std::ios::binary);
        std::ostringstream ss;
        ss << f.rdbuf();
        result.pages.push_back({"image/png", funes::base64_encode(ss.str())});
    }
    fs::remove_all(tmp_dir, ec);

    if (result.pages.empty()) {
        result.error = "pdftoppm produced no pages for '" + pdf_path.string() + "'.";
        return result;
    }
    result.ok = true;
    return result;
}

} // namespace funes::pdf
