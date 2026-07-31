// =============================================================================
// src/core/tools/tavily.cpp — Tavily Search API transport
// =============================================================================
// Lifted verbatim out of web_search.cpp when harvest_candidates needed the same
// call. This used to scrape DuckDuckGo's HTML interface, but DuckDuckGo now
// serves an anti-bot challenge (an image CAPTCHA, wrapped in an HTTP 202) to
// every request from a server, so that approach no longer works at all.

#include "tavily.h"
#include "../funes_config.h"
#include "httplib.h"
#include "json.hpp"
#include <algorithm>

using json = nlohmann::json;

namespace funes::tavily {

const std::vector<std::string>& default_exclude_domains() {
    static const std::vector<std::string> kExcluded = {
        "tiktok.com", "instagram.com", "facebook.com",
        "x.com", "twitter.com", "youtube.com", "pinterest.com"
    };
    return kExcluded;
}

std::string search(const Query& q, std::vector<Hit>& out) {
    if (q.query.empty())
        return "Missing or invalid 'query' argument";

    const std::string api_key = funes::env("FUNES_TAVILY_API_KEY");
    if (api_key.empty())
        return "web search is not configured: set FUNES_TAVILY_API_KEY (get a free key "
               "at https://tavily.com) in config/funes.local.";

    json body = {
        {"query",          q.query},
        {"max_results",    std::clamp(q.max_results, 1, 20)},
        {"exclude_domains", q.exclude_domains.empty() ? default_exclude_domains()
                                                      : q.exclude_domains}
    };
    // "news" + a time_range lets the caller ask for genuinely recent results
    // (e.g. "last 24 hours") instead of hoping the query text implies it.
    if (!q.topic.empty())      body["topic"] = q.topic;
    if (!q.time_range.empty()) body["time_range"] = q.time_range;

    httplib::SSLClient cli("api.tavily.com");
    cli.set_connection_timeout(10);
    cli.set_read_timeout(15);

    httplib::Headers headers = {{"Authorization", "Bearer " + api_key}};
    auto res = cli.Post("/search", headers, body.dump(), "application/json");

    if (!res)
        return "Search request failed: connection error";
    if (res->status == 401 || res->status == 403)
        return "Search request failed: HTTP " + std::to_string(res->status) +
               " (check FUNES_TAVILY_API_KEY is valid)";
    if (res->status == 429)
        return "Search request failed: rate limited (HTTP 429) — try again shortly";
    if (res->status != 200)
        return "Search request failed: HTTP " + std::to_string(res->status) + "\n" + res->body;

    json resp;
    try {
        resp = json::parse(res->body);
    } catch (...) {
        return "Search request failed: could not parse response";
    }

    if (!resp.contains("results") || !resp["results"].is_array())
        return {};   // no results is not an error; `out` stays empty

    for (const auto& r : resp["results"]) {
        Hit hit;
        hit.title     = r.value("title", "");
        hit.snippet   = r.value("content", "");
        hit.url       = r.value("url", "");
        hit.published = r.value("published_date", "");
        if (hit.title.empty() || hit.url.empty()) continue;
        out.push_back(std::move(hit));
    }
    return {};
}

} // namespace funes::tavily
