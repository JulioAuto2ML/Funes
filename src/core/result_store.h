// =============================================================================
// src/core/result_store.h — pass-by-reference for large tool results
// =============================================================================
// Every ToolResult.text used to go verbatim into the transcript as a tool
// message, and stayed there for the rest of the turn: a 40KB web_fetch cost
// ~10K tokens on every subsequent completion until compress_context threw it
// away lossily. The content is usually needed once, in full, right after the
// call — and never again in that shape.
//
// So results above kInlineLimit are stored once (MemoryStore::store_result,
// session-scoped) and the transcript carries a bounded preview instead: type,
// exact size, a head/tail sample, and the id to fetch more with `read_result`.
// The shape is deliberately explicit — a model reads "41320 bytes, result_id
// 17" and understands the handle refers to a real object it can go get.
//
// Small results (the majority: recall, list_tools, shell exit status) are
// untouched, so this is invisible until a tool actually returns something big.

#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace funes {

// Results at or below this many bytes stay inline, verbatim. Roughly 500
// tokens — small enough that carrying a few of them costs nothing.
constexpr size_t kInlineLimit = 2048;

// How much of a stored result the preview shows. Head is generous (the useful
// part of a page or a file is usually at the top); tail is a hint that the
// content ends the way the model expects.
constexpr size_t kPreviewHead = 1000;
constexpr size_t kPreviewTail = 200;

inline bool exceeds_inline_limit(const std::string& text) {
    return text.size() > kInlineLimit;
}

// The tool that reads a stored result back. Its output is a slice of something
// already in the store, so it must never be stored itself.
constexpr const char* kDereferenceTool = "read_result";

// True if this tool's output is exempt from storage regardless of size. There
// is one such tool, and the exemption is load-bearing: a full window plus the
// "N bytes remain" note result_window appends comes to slightly more than
// kInlineLimit, so on 2026-07-31 every dereference was stored in turn and
// answered with a fresh result_id instead of the text. The model reads, gets a
// new id, reads that, gets another — `researcher` spent all 20 of its steps
// this way and returned nothing. Clamping the window was tried and is what
// broke: the clamp bounds the window, not the note. Size can't be the guard
// here, so the tool is.
inline bool exempt_from_store(const std::string& tool) {
    return tool == kDereferenceTool;
}

// The JSON object that replaces `text` in the transcript. Both slices are cut
// on UTF-8 boundaries, so the preview is always safe to dump even when the
// underlying result is not (a tool can return any bytes it likes).
std::string result_preview(int64_t result_id, const std::string& tool,
                           const std::string& text);

// A window of a stored result, cut to UTF-8 boundaries on both ends, plus a
// trailing note stating what is left and the offset to continue from. `limit`
// is clamped to kInlineLimit by the caller, which bounds how much context one
// dereference costs. It does NOT keep the result under the storage threshold —
// the note is appended after the clamp — so re-storage is prevented by
// exempt_from_store above instead.
std::string result_window(const std::string& text, size_t offset, size_t limit);

} // namespace funes
