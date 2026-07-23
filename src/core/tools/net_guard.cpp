// =============================================================================
// src/core/tools/net_guard.cpp — shared outbound-request safety checks
// =============================================================================

#include "net_guard.h"
#include <cstdlib>
#include <regex>

namespace funes::net {

bool parse_http_url(const std::string& url, ParsedUrl& out) {
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

bool local_fetch_allowed() {
    const char* v = std::getenv("FUNES_ALLOW_LOCAL_FETCH");
    return v && *v == '1';
}

} // namespace funes::net
