// =============================================================================
// src/core/auto_memory.cpp — what an exchange is stored as
// =============================================================================

#include "auto_memory.h"
#include "text_utils.h"

namespace funes {

namespace {

const std::string kFileMarker  = "[Attached file: ";
const std::string kImageMarker = "[Attached image: ";
const std::string kBinaryNote  = "(binary file, saved to the workspace, not shown here)";

std::string collapse_whitespace(const std::string& s) {
    std::string out;
    bool space = false;
    for (char c : s) {
        if (c == ' ' || c == '\n' || c == '\t' || c == '\r') { space = !out.empty(); continue; }
        if (space) out += ' ';
        space = false;
        out += c;
    }
    return out;
}

} // namespace

std::string shorten_on_sentence(const std::string& text, size_t max_bytes) {
    if (text.size() <= max_bytes) return text;

    std::string head = text;
    truncate_utf8_safe(head, max_bytes);

    // A sentence end in the second half of the window: cutting there keeps a
    // whole thought; cutting one in the first half would throw most of the
    // window away for it.
    for (size_t i = head.size(); i-- > head.size() / 2;) {
        const char c = head[i];
        if ((c == '.' || c == '?' || c == '!') && i + 1 < text.size() && text[i + 1] == ' ')
            return head.substr(0, i + 1) + " …";
    }
    if (const size_t sp = head.rfind(' '); sp != std::string::npos && sp > head.size() / 2)
        return head.substr(0, sp) + "…";
    return head + "…";
}

std::string without_attachments(const std::string& user_message) {
    std::string out;
    size_t pos = 0;
    while (pos < user_message.size()) {
        const size_t file  = user_message.find(kFileMarker, pos);
        const size_t image = user_message.find(kImageMarker, pos);
        const size_t at = std::min(file, image);
        if (at == std::string::npos) { out += user_message.substr(pos); break; }

        out += user_message.substr(pos, at - pos);
        const size_t name_start = at + (at == file ? kFileMarker.size() : kImageMarker.size());
        const size_t name_end = user_message.find(']', name_start);
        if (name_end == std::string::npos) { out += user_message.substr(at); break; }
        out += "[attached: " + user_message.substr(name_start, name_end - name_start) + "] ";
        pos = name_end + 1;

        if (at != file) continue;
        // What follows a file marker: a fenced block, or the binary note.
        size_t next = pos;
        while (next < user_message.size() && user_message[next] == '\n') ++next;
        if (user_message.compare(next, 3, "```") == 0) {
            const size_t close = user_message.find("\n```", next + 3);
            pos = close == std::string::npos ? user_message.size() : close + 4;
        } else if (user_message.compare(next, kBinaryNote.size(), kBinaryNote) == 0) {
            pos = next + kBinaryNote.size();
        }
    }
    return collapse_whitespace(out);
}

std::string auto_memory_text(const std::string& user_message, const std::string& reply) {
    return "User said: \"" + shorten_on_sentence(without_attachments(user_message), kHalfCap)
         + "\" — I replied: \"" + shorten_on_sentence(reply, kHalfCap) + "\"";
}

} // namespace funes
