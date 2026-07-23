// =============================================================================
// src/core/tools/web_fetch.cpp — web_fetch native tool
// =============================================================================
// Fetches a URL and returns its content as readable text: script/style blocks
// removed, tags stripped, common entities decoded, whitespace collapsed,
// capped at 8 KB.
//
// Private/loopback hosts are refused unless FUNES_ALLOW_LOCAL_FETCH=1, so an
// LLM-chosen URL can't probe the local network by default.

#include "../tools.h"
#include "httplib.h"
#include <cstdlib>
#include <regex>
#include <string>

namespace {

constexpr size_t MAX_TEXT_BYTES = 8 * 1024;

struct ParsedUrl {
    bool        https = false;
    std::string host;
    int         port  = 80;
    std::string path  = "/";
};

bool parse_url(const std::string& url, ParsedUrl& out) {
    static const std::regex re(R"(^(https?)://([^/:?#]+)(?::(\d+))?([^#]*)?)");
    std::smatch m;
    if (!std::regex_match(url, m, re)) return false;
    out.https = (m[1].str() == "https");
    out.host  = m[2].str();
    out.port  = m[3].matched ? std::stoi(m[3].str()) : (out.https ? 443 : 80);
    out.path  = (m[4].matched && !m[4].str().empty()) ? m[4].str() : "/";
    return true;
}

bool is_private_host(const std::string& host) {
    if (host == "localhost" || host == "::1") return true;
    static const std::regex private_ip(
        R"(^(127\.|10\.|192\.168\.|169\.254\.|172\.(1[6-9]|2[0-9]|3[01])\.|0\.).*)");
    return std::regex_match(host, private_ip);
}

std::string decode_entities(std::string s) {
    static const std::pair<const char*, const char*> entities[] = {
        {"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""},
        {"&#39;", "'"}, {"&apos;", "'"}, {"&nbsp;", " "},
    };
    for (const auto& [from, to] : entities) {
        size_t pos = 0;
        while ((pos = s.find(from, pos)) != std::string::npos) {
            s.replace(pos, std::strlen(from), to);
            pos += std::strlen(to);
        }
    }
    return s;
}

std::string html_to_text(const std::string& html) {
    // Drop non-content blocks, keep paragraph structure roughly intact.
    static const std::regex re_script(R"(<script[\s\S]*?</script>)", std::regex::icase);
    static const std::regex re_style(R"(<style[\s\S]*?</style>)",   std::regex::icase);
    static const std::regex re_block(R"(</(p|div|h[1-6]|li|tr|section|article|br)>|<br\s*/?>)",
                                     std::regex::icase);
    static const std::regex re_tag(R"(<[^>]+>)");
    static const std::regex re_spaces(R"([ \t]+)");
    static const std::regex re_newlines(R"(\n{3,})");

    std::string text = std::regex_replace(html, re_script, " ");
    text = std::regex_replace(text, re_style, " ");
    text = std::regex_replace(text, re_block, "\n");
    text = std::regex_replace(text, re_tag, " ");
    text = decode_entities(text);
    text = std::regex_replace(text, re_spaces, " ");
    text = std::regex_replace(text, re_newlines, "\n\n");

    // Trim leading/trailing whitespace on each line.
    std::string out;
    out.reserve(text.size());
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        size_t a = start, b = end;
        while (a < b && (text[a] == ' ' || text[a] == '\t')) ++a;
        while (b > a && (text[b-1] == ' ' || text[b-1] == '\t')) --b;
        if (b > a) { out.append(text, a, b - a); out += '\n'; }
        start = end + 1;
    }
    return out;
}

ToolResult web_fetch_handler(const json& args, const ToolContext&) {
    if (!args.contains("url") || !args["url"].is_string())
        return {"Missing or invalid 'url' argument", /*error=*/true};

    const std::string url = args["url"].get<std::string>();
    ParsedUrl parsed;
    if (!parse_url(url, parsed))
        return {"Invalid URL (only http:// and https:// are supported): " + url, true};

    const char* allow_local = std::getenv("FUNES_ALLOW_LOCAL_FETCH");
    if (is_private_host(parsed.host) && !(allow_local && *allow_local == '1'))
        return {"Refusing to fetch private/loopback host '" + parsed.host +
                "' (set FUNES_ALLOW_LOCAL_FETCH=1 to allow)", true};

    httplib::Headers headers = {{"User-Agent", "Mozilla/5.0 (Funes)"}};
    httplib::Result res;
    if (parsed.https) {
        httplib::SSLClient cli(parsed.host, parsed.port);
        cli.set_connection_timeout(10);
        cli.set_read_timeout(20);
        cli.set_follow_location(true);
        res = cli.Get(parsed.path.c_str(), headers);
    } else {
        httplib::Client cli(parsed.host, parsed.port);
        cli.set_connection_timeout(10);
        cli.set_read_timeout(20);
        cli.set_follow_location(true);
        res = cli.Get(parsed.path.c_str(), headers);
    }

    if (!res)
        return {"Fetch failed: connection error for " + url, true};
    if (res->status != 200)
        return {"Fetch failed: HTTP " + std::to_string(res->status) + " for " + url, true};

    const std::string content_type = res->get_header_value("Content-Type");
    std::string text;
    if (content_type.find("text/html") != std::string::npos || content_type.empty()) {
        text = html_to_text(res->body);
    } else if (content_type.find("text/") != std::string::npos
               || content_type.find("json") != std::string::npos
               || content_type.find("xml") != std::string::npos) {
        text = res->body;
    } else {
        return {"Unsupported content type '" + content_type + "' for " + url, true};
    }

    if (text.size() > MAX_TEXT_BYTES) {
        text.resize(MAX_TEXT_BYTES);
        text += "\n\n[content truncated at 8 KB]";
    }
    if (text.empty())
        return {"Fetched " + url + " but extracted no text content.", true};
    return {text};
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
