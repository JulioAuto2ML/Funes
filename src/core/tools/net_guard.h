// =============================================================================
// src/core/tools/net_guard.h — shared outbound-request safety checks
// =============================================================================
// Used by every native tool that lets the model choose a URL (web_fetch,
// generated HTTP-template tools): refuses private/loopback hosts unless
// FUNES_ALLOW_LOCAL_FETCH=1, so an LLM-chosen URL can't probe the local
// network by default. One implementation so the guard can't drift between
// tools.

#pragma once
#include <string>

namespace funes::net {

struct ParsedUrl {
    bool        https = false;
    std::string host;
    int         port  = 80;
    std::string path  = "/";
};

// Only http:// and https:// are accepted.
bool parse_http_url(const std::string& url, ParsedUrl& out);

bool is_private_host(const std::string& host);

// Reads FUNES_ALLOW_LOCAL_FETCH once per process; "1" enables local fetches.
bool local_fetch_allowed();

} // namespace funes::net
