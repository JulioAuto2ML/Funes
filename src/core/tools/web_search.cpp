// =============================================================================
// src/core/tools/web_search.cpp — web_search native tool
// =============================================================================
// Searches DuckDuckGo's HTML interface and extracts up to 5 title/snippet
// pairs. Ported from the AresOS MCP tool, minus the MCP wrapper.

#include "../tools.h"
#include "httplib.h"
#include <cctype>
#include <cstdio>
#include <regex>
#include <sstream>

namespace {

std::string url_encode(const std::string& s) {
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else if (c == ' ') {
            out += '+';
        } else {
            char buf[4];
            std::snprintf(buf, sizeof(buf), "%%%02X", static_cast<unsigned>(c));
            out += buf;
        }
    }
    return out;
}

std::string strip_tags(const std::string& html) {
    return std::regex_replace(html, std::regex("<[^>]+>"), "");
}

void trim(std::string& s) {
    auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) { s.clear(); return; }
    s = s.substr(first, s.find_last_not_of(" \t\r\n") - first + 1);
}

ToolResult web_search_handler(const json& args, const ToolContext&) {
    if (!args.contains("query") || !args["query"].is_string()
        || args["query"].get<std::string>().empty())
        return {"Missing or invalid 'query' argument", /*error=*/true};

    const std::string query = args["query"].get<std::string>();
    const std::string path  = "/html/?q=" + url_encode(query);

    httplib::SSLClient cli("html.duckduckgo.com");
    cli.set_connection_timeout(10);
    cli.set_read_timeout(15);

    httplib::Headers headers = {{"User-Agent", "Mozilla/5.0"}};
    auto res = cli.Get(path.c_str(), headers);

    if (!res)
        return {"Search request failed: connection error", /*error=*/true};
    if (res->status != 200)
        return {"Search request failed: HTTP " + std::to_string(res->status), /*error=*/true};

    const std::string& html = res->body;

    static const std::regex re_result(
        R"(<a[^>]+class="result__a"[^>]*>(.*?)</a>.*?)"
        R"(<a[^>]+class="result__snippet"[^>]*>(.*?)</a>)",
        std::regex::ECMAScript | std::regex::multiline
    );

    std::ostringstream oss;
    int n = 0;
    auto begin = std::sregex_iterator(html.begin(), html.end(), re_result);
    for (auto it = begin; it != std::sregex_iterator() && n < 5; ++it) {
        std::string title   = strip_tags((*it)[1].str());
        std::string snippet = strip_tags((*it)[2].str());
        trim(title);
        trim(snippet);
        if (title.empty() || snippet.empty()) continue;
        if (n > 0) oss << "\n\n";
        oss << title << "\n" << snippet;
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
        "Search the web (DuckDuckGo). Returns the top result titles and snippets.",
        {
            {"type", "object"},
            {"properties", {
                {"query", {{"type", "string"}, {"description", "The search query"}}}
            }},
            {"required", json::array({"query"})}
        },
        web_search_handler
    });
}
