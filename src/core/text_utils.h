// =============================================================================
// src/core/text_utils.h — binary-safe text helpers
// =============================================================================
// nlohmann::json::dump() throws on invalid UTF-8 by default. Any tool that
// reads bytes it didn't produce itself (read_file, an uploaded attachment)
// can hand back something that isn't valid text, which would otherwise crash
// the SSE stream or the LLM request body. These two helpers keep that content
// out of the JSON path instead of trying to sanitize it after the fact.

#pragma once
#include "json.hpp"
#include <string>

namespace funes {

// True if `data` has no embedded NUL bytes and is well-formed UTF-8 — i.e.
// safe to treat as text. A conservative heuristic: legitimate text in a
// non-UTF-8 encoding will also read as "not text" here, which is the safe
// failure direction (report as binary rather than risk a crash downstream).
bool looks_like_text(const std::string& data);

// Truncates `s` to at most `max_bytes`, trimming a few extra bytes if needed
// so the cut doesn't land in the middle of a multi-byte UTF-8 sequence.
void truncate_utf8_safe(std::string& s, size_t max_bytes);

// json::dump() throws on invalid UTF-8 by default (json::type_error.316).
// Most values built from the model's own output are already guaranteed
// valid (they came from a successful json::parse upstream), but anything
// carrying a native tool's or MCP server's raw text — a ToolResult, a tool
// result folded into a ChatMessage, an SSE payload — isn't. Use this instead
// of a bare `.dump()` anywhere that text might reach: invalid sequences are
// replaced with U+FFFD instead of throwing, so a bad byte degrades the
// content instead of crashing the request.
inline std::string dump_safe(const nlohmann::json& j) {
    return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

} // namespace funes
