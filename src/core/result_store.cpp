// =============================================================================
// src/core/result_store.cpp
// =============================================================================

#include "result_store.h"
#include "json.hpp"
#include "text_utils.h"
#include <algorithm>

namespace funes {

namespace {

// Moves `i` forward off a UTF-8 continuation byte, so a slice that starts at
// `i` starts at a character. Bounded by the string length.
size_t forward_to_boundary(const std::string& s, size_t i) {
    while (i < s.size() && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) ++i;
    return i;
}

// Copy a little past the cut before trimming: truncate_utf8_safe is a no-op on
// a string that is already short enough, so slicing to exactly max_bytes first
// would hand it a string that is already split mid-character.
constexpr size_t kMaxUtf8Sequence = 4;

std::string head_slice(const std::string& text, size_t max_bytes) {
    std::string head = text.substr(0, std::min(max_bytes + kMaxUtf8Sequence, text.size()));
    truncate_utf8_safe(head, max_bytes);
    return head;
}

std::string tail_slice(const std::string& text, size_t max_bytes) {
    if (text.size() <= max_bytes) return text;
    return text.substr(forward_to_boundary(text, text.size() - max_bytes));
}

} // namespace

std::string result_preview(int64_t result_id, const std::string& tool,
                           const std::string& text) {
    nlohmann::json j{
        {"result_id", result_id},
        {"tool",      tool},
        {"bytes",     text.size()},
        {"head",      head_slice(text, kPreviewHead)},
        {"tail",      tail_slice(text, kPreviewTail)},
        {"note",      "Full result stored, not shown. Use read_result(id=" +
                      std::to_string(result_id) + ", offset, limit) to read more of it."}
    };
    return dump_safe(j);
}

std::string result_window(const std::string& text, size_t offset, size_t limit) {
    if (offset >= text.size())
        return "(offset " + std::to_string(offset) + " is past the end of this result, "
               "which is " + std::to_string(text.size()) + " bytes)";

    offset = forward_to_boundary(text, offset);
    std::string window = text.substr(offset, limit + kMaxUtf8Sequence);
    truncate_utf8_safe(window, limit);

    const size_t next = offset + window.size();
    std::string note = next >= text.size()
        ? "\n\n[end of result: " + std::to_string(text.size()) + " bytes total]"
        : "\n\n[" + std::to_string(text.size() - next) + " of " +
          std::to_string(text.size()) + " bytes remain — continue with offset=" +
          std::to_string(next) + "]";
    return window + note;
}

} // namespace funes
