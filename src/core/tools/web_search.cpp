// =============================================================================
// src/core/tools/web_search.cpp — web_search native tool
// =============================================================================
// Uses the Tavily Search API (api.tavily.com) and returns the top result
// titles, snippets, and URLs.
//
// This used to scrape DuckDuckGo's HTML interface, but DuckDuckGo now serves
// an anti-bot challenge (an image CAPTCHA, wrapped in an HTTP 202) to every
// request from a server, so that approach no longer works at all.

#include "../funes_config.h"
#include "../tools.h"
#include "httplib.h"
#include <algorithm>
#include <sstream>

namespace {

ToolResult web_search_handler(const json& args, const ToolContext&) {
    if (!args.contains("query") || !args["query"].is_string()
        || args["query"].get<std::string>().empty())
        return {"Missing or invalid 'query' argument", /*error=*/true};

    const std::string api_key = funes::env("FUNES_TAVILY_API_KEY");
    if (api_key.empty())
        return {"web_search is not configured: set FUNES_TAVILY_API_KEY (get a free key "
                "at https://tavily.com) in config/funes.local.", /*error=*/true};

    int max_results = 8;
    if (args.contains("max_results") && args["max_results"].is_number_integer())
        max_results = std::clamp(args["max_results"].get<int>(), 1, 10);

    json body = {
        {"query",       args["query"].get<std::string>()},
        {"max_results", max_results},
        // Social platforms serve a JS shell with no article content to a
        // plain fetch (and often block it outright), so they're useless as
        // "source" links regardless of how relevant Tavily thinks the post
        // is — excluding them up front beats discovering that downstream
        // when web_fetch/link-verification hits an anti-bot wall.
        {"exclude_domains", json::array({
            "tiktok.com", "instagram.com", "facebook.com",
            "x.com", "twitter.com", "youtube.com", "pinterest.com"
        })}
    };
    // "news" + a time_range lets the caller ask for genuinely recent results
    // (e.g. "last 24 hours") instead of hoping the query text implies it.
    if (args.contains("topic") && args["topic"].is_string())
        body["topic"] = args["topic"];
    if (args.contains("time_range") && args["time_range"].is_string())
        body["time_range"] = args["time_range"];

    httplib::SSLClient cli("api.tavily.com");
    cli.set_connection_timeout(10);
    cli.set_read_timeout(15);

    httplib::Headers headers = {
        {"Authorization", "Bearer " + api_key}
    };
    auto res = cli.Post("/search", headers, body.dump(), "application/json");

    if (!res)
        return {"Search request failed: connection error", /*error=*/true};
    if (res->status == 401 || res->status == 403)
        return {"Search request failed: HTTP " + std::to_string(res->status) +
                " (check FUNES_TAVILY_API_KEY is valid)", /*error=*/true};
    if (res->status == 429)
        return {"Search request failed: rate limited (HTTP 429) — try again shortly", /*error=*/true};
    if (res->status != 200)
        return {"Search request failed: HTTP " + std::to_string(res->status) + "\n" + res->body, /*error=*/true};

    json resp;
    try {
        resp = json::parse(res->body);
    } catch (...) {
        return {"Search request failed: could not parse response", /*error=*/true};
    }

    if (!resp.contains("results") || !resp["results"].is_array() || resp["results"].empty())
        return {"No results found."};

    std::ostringstream oss;
    int n = 0;
    for (const auto& r : resp["results"]) {
        if (n >= max_results) break;
        const std::string title   = r.value("title", "");
        const std::string snippet = r.value("content", "");
        const std::string url     = r.value("url", "");
        if (title.empty() || url.empty()) continue;
        if (n > 0) oss << "\n\n";
        oss << title << "\n" << snippet << "\n" << url;
        ++n;
    }

    if (n == 0)
        return {"No results found."};
    return {oss.str()};
}

} // namespace

void register_web_search(ToolRegistry& reg) {
    reg.add({
        "web_search",
        "Search the web (Tavily API). Returns the top result titles, snippets, and URLs. "
        "Social platforms (TikTok, Instagram, Facebook, X/Twitter, YouTube, Pinterest) are "
        "excluded — they serve a JS shell with no content to a plain fetch, so they're never "
        "usable as source links anyway.",
        {
            {"type", "object"},
            {"properties", {
                {"query", {{"type", "string"}, {"description", "The search query"}}},
                {"topic", {{"type", "string"}, {"enum", json::array({"general", "news", "finance"})},
                          {"description", "Search category — use 'news' for current-events queries (default: general)"}}},
                {"time_range", {{"type", "string"}, {"enum", json::array({"day", "week", "month", "year"})},
                               {"description", "Restrict results to this recency window — use 'day' for \"last 24 hours\"-style requests"}}},
                {"max_results", {{"type", "integer"}, {"minimum", 1}, {"maximum", 10},
                                 {"description", "How many results to return (default 8, max 10) — ask for more when you need several distinct items, e.g. a top-10 news roundup"}}}
            }},
            {"required", json::array({"query"})}
        },
        web_search_handler
    });
}
