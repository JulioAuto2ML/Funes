// =============================================================================
// src/core/tools/pdf_extract.cpp — PDF text extraction via pdftotext
// =============================================================================

#include "pdf_extract.h"
#include "../text_utils.h"
#include "process_runner.h"

namespace funes::pdf {

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
    if (r.output.empty())
        return {false, "pdftotext extracted no text from '" + pdf_path.string() +
                "' (it may be a scanned/image-only PDF)."};

    return {true, r.output};
}

} // namespace funes::pdf
