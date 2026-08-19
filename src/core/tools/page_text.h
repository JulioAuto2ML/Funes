// =============================================================================
// src/core/tools/page_text.h — fetch a URL as readable text
// =============================================================================
// The extraction half of web_fetch, split out when harvest_candidates needed
// the same thing at a different size: web_fetch caps its output at 8 KB because
// it goes straight into the transcript, while a harvested candidate's text is
// kept out of the transcript and checked against later, so it wants all of it.
//
// Truncation is therefore the caller's decision and is not done here.

#pragma once
#include <string>

namespace funes::web {

// HTML → readable text: <script>/<style> dropped, block-level closing tags
// become newlines, common entities decoded, whitespace collapsed, each line
// trimmed.
std::string html_to_text(const std::string& html);

struct Page {
    std::string text;    // readable text, untruncated
    std::string error;   // empty = success; otherwise written to be read by a model
};

// Refuses private/loopback hosts unless FUNES_ALLOW_LOCAL_FETCH=1 (net_guard.h),
// so an LLM-chosen URL can't probe the local network by default. Non-2xx, a
// content type that isn't text, and content that isn't valid UTF-8 all come
// back as errors rather than as empty text: a caller that drops a candidate on
// a bad fetch needs to be able to tell "nothing there" from "nothing said".
Page fetch_readable(const std::string& url);

// Rewrites www.reddit.com / reddit.com URLs to old.reddit.com (server-rendered
// HTML instead of a React SPA shell). Other URLs pass through unchanged.
std::string rewrite_reddit(const std::string& url);

} // namespace funes::web
