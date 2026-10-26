// =============================================================================
// src/core/tools/parallel_search.cpp — see parallel_search.h
// =============================================================================

#include "parallel_search.h"
#include "../funes_config.h"
#include "../mcp_http_client.h"
#include "../text_utils.h"
#include <algorithm>
#include <cctype>

using json = nlohmann::json;

namespace funes::parallel {
namespace {

std::string host_of(const std::string& url) {
    size_t start = url.find("://");
    start = (start == std::string::npos) ? 0 : start + 3;
    const size_t end = url.find_first_of("/:?#", start);
    std::string host = url.substr(start, end == std::string::npos ? std::string::npos
                                                                  : end - start);
    std::transform(host.begin(), host.end(), host.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return host;
}

bool excluded(const std::string& url, const std::vector<std::string>& domains) {
    const std::string host = host_of(url);
    for (const auto& d : domains) {
        if (host == d) return true;
        if (host.size() > d.size() && host.compare(host.size() - d.size(), d.size(), d) == 0
            && host[host.size() - d.size() - 1] == '.')
            return true;
    }
    return false;
}

std::string collapse_whitespace(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    bool space = false;
    for (unsigned char c : s) {
        if (std::isspace(c)) { space = !out.empty(); continue; }
        if (space) { out += ' '; space = false; }
        out += static_cast<char>(c);
    }
    return out;
}

std::string objective_for(const tavily::Query& q) {
    if (q.time_range.empty()) return q.query;
    return q.query + " (published in the last " + q.time_range + ")";
}

} // namespace

std::string parse_results(const std::string& text,
                          const std::vector<std::string>& exclude_domains,
                          std::vector<tavily::Hit>& out) {
    json resp;
    try {
        resp = json::parse(text);
    } catch (...) {
        return "Search request failed: could not parse Parallel's response";
    }
    if (!resp.is_object() || !resp.contains("results") || !resp["results"].is_array())
        return {};

    for (const auto& r : resp["results"]) {
        if (!r.is_object()) continue;
        tavily::Hit hit;
        // json_string: publish_date and excerpts come back present-and-null.
        hit.title     = funes::json_string(r, "title");
        hit.url       = funes::json_string(r, "url");
        hit.published = funes::json_string(r, "publish_date");
        if (hit.title.empty() || hit.url.empty() || excluded(hit.url, exclude_domains))
            continue;
        if (r.contains("excerpts") && r["excerpts"].is_array()) {
            std::string joined;
            for (const auto& e : r["excerpts"]) {
                if (!e.is_string()) continue;
                const std::string piece = collapse_whitespace(e.get<std::string>());
                if (piece.empty()) continue;
                if (!joined.empty()) joined += " … ";
                joined += piece;
            }
            hit.snippet = std::move(joined);
        }
        out.push_back(std::move(hit));
    }
    return {};
}

std::string search(const tavily::Query& q, std::vector<tavily::Hit>& out) {
    if (q.query.empty())
        return "Missing or invalid 'query' argument";

    json queries = json::array({q.query});
    for (const auto& d : q.include_domains) queries.push_back(q.query + " site:" + d);

    const std::string url = funes::env("FUNES_PARALLEL_URL", kDefaultUrl);
    try {
        HttpMcpClient client(url, /*timeout_seconds=*/30);
        const std::string key = funes::env("FUNES_PARALLEL_API_KEY");
        if (!key.empty()) client.set_header("Authorization", "Bearer " + key);
        if (!client.initialize("funes", "1"))
            return "Search request failed: could not open a session with Parallel (" + url + ")";

        const json result = client.call_tool("web_search", {
            {"objective",      objective_for(q)},
            {"search_queries", queries},
        });
        std::string text;
        if (result.contains("content") && result["content"].is_array())
            for (const auto& c : result["content"])
                if (c.is_object() && c.value("type", "") == "text")
                    text += funes::json_string(c, "text");
        if (result.value("isError", false))
            return "Search request failed: Parallel returned an error: " +
                   (text.empty() ? std::string("(no message)") : text);

        std::vector<tavily::Hit> hits;
        const std::string err = parse_results(
            text,
            q.exclude_domains.empty() ? tavily::default_exclude_domains() : q.exclude_domains,
            hits);
        if (!err.empty()) return err;
        if (static_cast<int>(hits.size()) > q.max_results) hits.resize(std::max(q.max_results, 0));
        for (auto& h : hits) out.push_back(std::move(h));
        return {};
    } catch (const std::exception& e) {
        return std::string("Search request failed: ") + e.what();
    }
}

} // namespace funes::parallel
