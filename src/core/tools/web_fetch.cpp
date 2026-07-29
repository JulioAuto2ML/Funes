// =============================================================================
// src/core/tools/web_fetch.cpp — web_fetch native tool
// =============================================================================
// Fetches a URL and returns its content as readable text: script/style blocks
// removed, tags stripped, common entities decoded, whitespace collapsed,
// capped at 8 KB.
//
// Private/loopback hosts are refused unless FUNES_ALLOW_LOCAL_FETCH=1, so an
// LLM-chosen URL can't probe the local network by default.

#include "../text_utils.h"
#include "../tools.h"
#include "net_guard.h"
#include "httplib.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

using funes::net::ParsedUrl;
constexpr size_t MAX_TEXT_BYTES = 8 * 1024;

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

// Tags whose closing tag should become a newline (paragraph/line breaks);
// everything else collapses to a space, matching typical reading flow.
bool is_block_close_tag(const std::string& lower, size_t name_start, size_t name_len) {
    static const char* kTags[] = {
        "p", "div", "h1", "h2", "h3", "h4", "h5", "h6", "li", "tr", "section", "article"
    };
    for (const char* t : kTags) {
        size_t len = std::strlen(t);
        if (name_len == len && lower.compare(name_start, len, t) == 0) return true;
    }
    return false;
}

// Manual scan instead of regex for anything that runs over the full (often
// 100KB+) fetched body: libstdc++'s std::regex recurses once per repetition
// for quantified subexpressions (`[\s\S]*?`, `[^>]+`, `[ \t]+`, ...), so a
// long run of matching characters — an ordinary large inline <script> block,
// not an edge case — can blow the stack and crash the whole process.
std::string html_to_text(const std::string& html) {
    std::string lower(html.size(), '\0');
    std::transform(html.begin(), html.end(), lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    std::string text;
    text.reserve(html.size());
    size_t i = 0;
    while (i < html.size()) {
        if (html[i] != '<') { text += html[i++]; continue; }

        if (lower.compare(i, 7, "<script") == 0 || lower.compare(i, 6, "<style") == 0) {
            const bool is_script = lower.compare(i, 7, "<script") == 0;
            const std::string close = is_script ? "</script>" : "</style>";
            size_t close_pos = lower.find(close, i);
            text += ' ';
            i = (close_pos == std::string::npos) ? html.size() : close_pos + close.size();
            continue;
        }

        if (lower.compare(i, 3, "<br") == 0) {
            text += '\n';
            size_t gt = html.find('>', i);
            i = (gt == std::string::npos) ? html.size() : gt + 1;
            continue;
        }

        size_t gt = html.find('>', i);
        if (gt == std::string::npos) { text += html[i++]; continue; } // stray '<': literal

        size_t name_start = i + 1;
        bool closing = name_start < gt && html[name_start] == '/';
        if (closing) ++name_start;
        size_t name_end = name_start;
        while (name_end < gt && (std::isalnum(static_cast<unsigned char>(lower[name_end])) || lower[name_end] == '-'))
            ++name_end;

        text += (closing && is_block_close_tag(lower, name_start, name_end - name_start)) ? '\n' : ' ';
        i = gt + 1;
    }

    text = decode_entities(text);

    // Collapse runs of spaces/tabs to one space, and runs of 3+ newlines to two.
    std::string collapsed;
    collapsed.reserve(text.size());
    int newline_run = 0;
    for (char c : text) {
        if (c == ' ' || c == '\t') {
            if (collapsed.empty() || collapsed.back() != ' ') collapsed += ' ';
            newline_run = 0;
        } else if (c == '\n') {
            if (++newline_run <= 2) collapsed += '\n';
        } else {
            newline_run = 0;
            collapsed += c;
        }
    }

    // Trim leading/trailing whitespace on each line.
    std::string out;
    out.reserve(collapsed.size());
    size_t start = 0;
    while (start < collapsed.size()) {
        size_t end = collapsed.find('\n', start);
        if (end == std::string::npos) end = collapsed.size();
        size_t a = start, b = end;
        while (a < b && (collapsed[a] == ' ' || collapsed[a] == '\t')) ++a;
        while (b > a && (collapsed[b-1] == ' ' || collapsed[b-1] == '\t')) --b;
        if (b > a) { out.append(collapsed, a, b - a); out += '\n'; }
        start = end + 1;
    }
    return out;
}

ToolResult web_fetch_handler(const json& args, const ToolContext&) {
    if (!args.contains("url") || !args["url"].is_string())
        return {"Missing or invalid 'url' argument", /*error=*/true};

    const std::string url = args["url"].get<std::string>();
    ParsedUrl parsed;
    if (!funes::net::parse_http_url(url, parsed))
        return {"Invalid URL (only http:// and https:// are supported): " + url, true};

    if (funes::net::is_private_host(parsed.host) && !funes::net::local_fetch_allowed())
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

    const bool was_truncated = text.size() > MAX_TEXT_BYTES;
    if (was_truncated) funes::truncate_utf8_safe(text, MAX_TEXT_BYTES);

    // A page served with a non-UTF-8 charset (or a text/* response that's
    // actually binary) would otherwise flow into the chat history and crash
    // JSON serialization the first time it's dumped.
    if (!funes::looks_like_text(text))
        return {"Fetched " + url + " but its content isn't valid UTF-8 text "
                "(wrong charset or not actually text) — can't return it.", true};

    if (was_truncated) text += "\n\n[content truncated at 8 KB]";
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
