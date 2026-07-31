// =============================================================================
// src/core/tools/tavily.h — the Tavily Search API, once
// =============================================================================
// web_search had this inline. harvest_candidates needs the same call with a
// different result shape, and a second copy of an HTTP client, an exclude list
// and a set of status-code messages is exactly the kind of duplication that
// drifts: fix a 429 message in one and the other keeps the old one.
//
// So the transport lives here and returns structured hits. What a caller does
// with them — flat text for the model to read, or a numbered pool for it to
// pick from — is the tool's business, not this file's.

#pragma once
#include <string>
#include <vector>

namespace funes::tavily {

struct Hit {
    std::string title;
    std::string snippet;    // Tavily's "content": its own extract, not the page
    std::string url;
    std::string published;  // "published_date" — only set for topic: news
};

struct Query {
    std::string query;
    std::string topic;       // "" | "general" | "news" | "finance"
    std::string time_range;  // "" | "day" | "week" | "month" | "year"
    int         max_results = 8;
    // Empty = default_exclude_domains(). A caller that wants to add to the
    // defaults rather than replace them must concatenate them itself.
    std::vector<std::string> exclude_domains;
};

// Social platforms serve a JS shell with no article content to a plain fetch
// (and often block it outright), so they're useless as "source" links
// regardless of how relevant Tavily thinks the post is — excluding them up
// front beats discovering that downstream when the fetch or the link check
// hits an anti-bot wall.
const std::vector<std::string>& default_exclude_domains();

// Empty return = success, `out` holds the hits (hits missing a title or a URL
// are dropped). Otherwise the message is written to be read by a model: it
// says what failed and, where there is one, what to do about it.
std::string search(const Query& q, std::vector<Hit>& out);

} // namespace funes::tavily
