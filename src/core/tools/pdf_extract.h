// =============================================================================
// src/core/tools/pdf_extract.h — PDF text extraction via pdftotext
// =============================================================================
// Shared by read_file (src/core/tools/file_tools.cpp) and the upload
// endpoint (src/server/api.cpp) so a PDF gets the same treatment whether the
// model reads it from the workspace or the user drags it into the chat.

#pragma once
#include <filesystem>
#include <string>

namespace funes::pdf {

struct ExtractResult {
    bool        ok = false;
    std::string text_or_error;  // extracted text if ok, a human-readable
                                // error message otherwise
};

// Runs `pdftotext <pdf_path> -` (cwd = `cwd`, fixed argv, no shell — see
// process_runner.h) and returns its output, or a clear error: not
// installed, non-zero exit, output isn't valid UTF-8, or no text extracted
// (e.g. a scanned/image-only PDF).
ExtractResult extract_text(const std::filesystem::path& pdf_path,
                           const std::filesystem::path& cwd,
                           int timeout_seconds, size_t max_bytes);

} // namespace funes::pdf
