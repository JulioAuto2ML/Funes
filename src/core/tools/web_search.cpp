// =============================================================================
// src/core/tools/web_search.cpp — web_search native tool
// =============================================================================
// Open-ended research: run a query, return the top titles, snippets and URLs as
// flat text for the model to read. The transport lives in tavily.h, shared with
// harvest_candidates.
//
// Deliberately left with no identifiers on its results. A model that wants to
// use one of these URLs has to retype it, which is the transcription risk that
// harvest_candidates exists to remove — but for research, where the next step
// is usually web_fetch on something the model just read about rather than a
// published artifact, flat text is the right shape and the cheaper one.

#include "../tools.h"
#include "tavily.h"
#include <algorithm>
#include <sstream>

namespace {

ToolResult web_search_handler(const json& args, const ToolContext&) {
    if (!args.contains("query") || !args["query"].is_string()
        || args["query"].get<std::string>().empty())
        return {"Missing or invalid 'query' argument", /*error=*/true};

    funes::tavily::Query q;
    q.query = args["query"].get<std::string>();
    if (args.contains("topic") && args["topic"].is_string())
        q.topic = args["topic"].get<std::string>();
    if (args.contains("time_range") && args["time_range"].is_string())
        q.time_range = args["time_range"].get<std::string>();
    q.max_results = 8;
    if (args.contains("max_results") && args["max_results"].is_number_integer())
        q.max_results = std::clamp(args["max_results"].get<int>(), 1, 10);

    std::vector<funes::tavily::Hit> hits;
    const std::string error = funes::tavily::search(q, hits);
    if (!error.empty())
        return {error, /*error=*/true};
    if (hits.empty())
        return {"No results found."};

    std::ostringstream oss;
    int n = 0;
    for (const auto& hit : hits) {
        if (n >= q.max_results) break;
        if (n > 0) oss << "\n\n";
        oss << hit.title << "\n" << hit.snippet << "\n" << hit.url;
        ++n;
    }
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
