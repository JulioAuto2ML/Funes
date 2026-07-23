// =============================================================================
// src/core/tools/http_tool_runtime.cpp — runtime for LLM-scaffolded HTTP tools
// =============================================================================

#include "http_tool_runtime.h"
#include "net_guard.h"
#include "httplib.h"
#include <cctype>
#include <cstdlib>
#include <functional>
#include <regex>
#include <sstream>
#include <vector>

namespace funes::tools {

namespace {

constexpr size_t MAX_RESPONSE_BYTES = 8 * 1024;

std::string url_encode(const std::string& s) {
    std::ostringstream out;
    out << std::hex;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out << c;
        } else {
            out << '%' << std::uppercase;
            out.width(2); out.fill('0');
            out << static_cast<int>(c) << std::nouppercase;
        }
    }
    return out.str();
}

std::string arg_to_plain_string(const json& v) {
    if (v.is_string()) return v.get<std::string>();
    if (v.is_null())   return "";
    return v.dump();  // numbers, bools, arrays, objects → their JSON text
}

// Replaces every {name} in `tmpl` where `name` matches a key in `args`,
// running each substituted value through `encode`.
std::string substitute_placeholders(const std::string& tmpl, const json& args,
                                    const std::function<std::string(const json&)>& encode) {
    static const std::regex re(R"(\{([A-Za-z_][A-Za-z0-9_]*)\})");
    std::string out;
    out.reserve(tmpl.size());
    auto begin = std::sregex_iterator(tmpl.begin(), tmpl.end(), re);
    auto end   = std::sregex_iterator();
    size_t last = 0;
    for (auto it = begin; it != end; ++it) {
        const auto& m = *it;
        out.append(tmpl, last, m.position(0) - last);
        const std::string key = m[1].str();
        out += args.contains(key) ? encode(args[key]) : "";
        last = m.position(0) + m.length(0);
    }
    out.append(tmpl, last, std::string::npos);
    return out;
}

std::string resolve_env_placeholders(const std::string& text) {
    static const std::regex re(R"(\$\{([A-Za-z_][A-Za-z0-9_]*)\})");
    std::string out;
    out.reserve(text.size());
    auto begin = std::sregex_iterator(text.begin(), text.end(), re);
    auto end   = std::sregex_iterator();
    size_t last = 0;
    for (auto it = begin; it != end; ++it) {
        const auto& m = *it;
        out.append(text, last, m.position(0) - last);
        const char* v = std::getenv(m[1].str().c_str());
        if (v) out += v;
        last = m.position(0) + m.length(0);
    }
    out.append(text, last, std::string::npos);
    return out;
}

std::string uppercase(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

ToolResult perform_request(const std::string& method, const net::ParsedUrl& url,
                           const httplib::Headers& headers, const std::string& body) {
    httplib::Result res;
    const char* content_type = "application/json";

    auto run = [&](auto& cli) {
        cli.set_connection_timeout(10);
        cli.set_read_timeout(20);
        cli.set_follow_location(true);
        if (method == "GET")         return cli.Get(url.path.c_str(), headers);
        else if (method == "DELETE") return cli.Delete(url.path.c_str(), headers);
        else if (method == "POST")   return cli.Post(url.path.c_str(), headers, body, content_type);
        else if (method == "PUT")    return cli.Put(url.path.c_str(), headers, body, content_type);
        else                          return cli.Patch(url.path.c_str(), headers, body, content_type);
    };

    if (url.https) {
        httplib::SSLClient cli(url.host, url.port);
        res = run(cli);
    } else {
        httplib::Client cli(url.host, url.port);
        res = run(cli);
    }

    if (!res)
        return {"HTTP request failed: connection error", /*error=*/true};

    std::string text = res->body;
    if (text.size() > MAX_RESPONSE_BYTES) {
        text.resize(MAX_RESPONSE_BYTES);
        text += "\n[response truncated at 8 KB]";
    }
    return {"HTTP " + std::to_string(res->status) + "\n" + text, res->status >= 400};
}

std::vector<RegisterFn>& generated_registry() {
    static std::vector<RegisterFn> r;
    return r;
}

} // namespace

void register_generated(RegisterFn fn) { generated_registry().push_back(fn); }

void register_all_generated_tools(ToolRegistry& reg) {
    for (auto fn : generated_registry()) fn(reg);
}

ToolHandler make_http_template_tool(const std::string& method,
                                    const std::string& url_template,
                                    const std::map<std::string, std::string>& headers,
                                    const std::string& body_template) {
    const std::string upper_method = uppercase(method);

    return [upper_method, url_template, headers, body_template]
           (const json& args, const ToolContext&) -> ToolResult {
        const std::string url = substitute_placeholders(url_template, args,
            [](const json& v) { return url_encode(arg_to_plain_string(v)); });

        net::ParsedUrl parsed;
        if (!net::parse_http_url(url, parsed))
            return {"Generated tool has an invalid URL after substitution: " + url, true};

        if (net::is_private_host(parsed.host) && !net::local_fetch_allowed())
            return {"Refusing to call private/loopback host '" + parsed.host +
                    "' (set FUNES_ALLOW_LOCAL_FETCH=1 to allow)", true};

        httplib::Headers hdrs = {{"User-Agent", "Mozilla/5.0 (Funes)"}};
        for (const auto& [name, value] : headers)
            hdrs.emplace(name, resolve_env_placeholders(value));

        std::string body;
        if (!body_template.empty())
            body = substitute_placeholders(body_template, args,
                [](const json& v) { return v.dump(); });

        return perform_request(upper_method, parsed, hdrs, body);
    };
}

} // namespace funes::tools
