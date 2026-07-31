// =============================================================================
// src/core/tools/web_fetch.cpp — web_fetch native tool
// =============================================================================
// Fetches a URL and returns its content as readable text, capped at 8 KB. The
// fetching and extraction live in page_text.h, shared with harvest_candidates;
// what's left here is the cap and the tool schema.
//
// The cap is the reason this file is not just an alias: web_fetch's output goes
// into the transcript, so its size is a context cost paid on every subsequent
// completion of the turn. Anything above 8 KB is better reached through the
// result store, which is what happens to this result anyway.

#include "../text_utils.h"
#include "../tools.h"
#include "page_text.h"

namespace {

constexpr size_t MAX_TEXT_BYTES = 8 * 1024;

ToolResult web_fetch_handler(const json& args, const ToolContext&) {
    if (!args.contains("url") || !args["url"].is_string())
        return {"Missing or invalid 'url' argument", /*error=*/true};

    funes::web::Page page = funes::web::fetch_readable(args["url"].get<std::string>());
    if (!page.error.empty())
        return {page.error, /*error=*/true};

    if (page.text.size() > MAX_TEXT_BYTES) {
        funes::truncate_utf8_safe(page.text, MAX_TEXT_BYTES);
        page.text += "\n\n[content truncated at 8 KB]";
    }
    return {page.text};
}

} // namespace

void register_web_fetch(ToolRegistry& reg) {
    reg.add({
        "web_fetch",
        "Fetch a web page by URL and return its readable text content (HTML is "
        "converted to plain text, output capped at 8 KB).",
        {
            {"type", "object"},
            {"properties", {
                {"url", {{"type", "string"}, {"description", "The http(s) URL to fetch"}}}
            }},
            {"required", json::array({"url"})}
        },
        web_fetch_handler
    });
}

// Single entry point for both web tools.
void register_web_search(ToolRegistry& reg);

void register_web_tools(ToolRegistry& reg) {
    register_web_search(reg);
    register_web_fetch(reg);
}
