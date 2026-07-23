// =============================================================================
// src/core/text_utils.cpp — binary-safe text helpers
// =============================================================================

#include "text_utils.h"

namespace funes {

bool looks_like_text(const std::string& data) {
    size_t i = 0;
    const size_t n = data.size();
    while (i < n) {
        const unsigned char c = static_cast<unsigned char>(data[i]);
        if (c == 0) return false;

        size_t extra;
        if      ((c & 0x80) == 0x00) extra = 0;  // ASCII
        else if ((c & 0xE0) == 0xC0) extra = 1;  // 2-byte sequence
        else if ((c & 0xF0) == 0xE0) extra = 2;  // 3-byte sequence
        else if ((c & 0xF8) == 0xF0) extra = 3;  // 4-byte sequence
        else return false;                        // stray continuation/invalid lead byte

        if (i + extra >= n) return false;
        for (size_t j = 1; j <= extra; ++j) {
            const unsigned char cont = static_cast<unsigned char>(data[i + j]);
            if ((cont & 0xC0) != 0x80) return false;
        }
        i += extra + 1;
    }
    return true;
}

void truncate_utf8_safe(std::string& s, size_t max_bytes) {
    if (s.size() <= max_bytes) return;
    s.resize(max_bytes);
    if (s.empty()) return;

    // Walk back from the end over continuation bytes (at most 3 — the
    // longest UTF-8 sequence has 3) to find where the last sequence starts.
    size_t seq_start = s.size() - 1;
    size_t cont = 0;
    while (cont < 3 && seq_start > 0
           && (static_cast<unsigned char>(s[seq_start]) & 0xC0) == 0x80) {
        --seq_start;
        ++cont;
    }

    const unsigned char lead = static_cast<unsigned char>(s[seq_start]);
    size_t expected_len;
    if      ((lead & 0x80) == 0x00) expected_len = 1;  // ASCII
    else if ((lead & 0xE0) == 0xC0) expected_len = 2;
    else if ((lead & 0xF0) == 0xE0) expected_len = 3;
    else if ((lead & 0xF8) == 0xF0) expected_len = 4;
    else expected_len = 0;  // stray continuation byte with no lead in reach

    // Only drop the trailing sequence if the cut actually landed inside it —
    // a sequence that completes exactly at max_bytes must be kept whole.
    const size_t have_len = s.size() - seq_start;
    if (expected_len == 0 || have_len < expected_len) s.resize(seq_start);
}

} // namespace funes
