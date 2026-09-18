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

// Decided on what the name resolves to, not on how it is spelled — see the
// comment in net_guard.cpp for what the literal-matching version let through.
// Unresolvable names count as private (refuse, don't guess).
bool is_private_host(const std::string& host);

// Turns a Location header into an absolute URL against the request it
// answered: absolute stays as is, "//host/p" takes the scheme, "/p" takes
// scheme+host+port, anything else is relative to the request path's
// directory. Empty when the result is not an http(s) URL.
//
// Exists because httplib's own set_follow_location(true) hops to whatever
// host the server names *without* going back through is_private_host — a
// public page answering 302 to http://127.0.0.1:8080/ walked straight past
// the guard. Callers follow redirects themselves, one hop at a time, and
// check each hop.
std::string resolve_location(const ParsedUrl& base, const std::string& location);

// Redirect hops a guarded fetch will follow before giving up.
constexpr int kMaxRedirects = 5;

// Reads FUNES_ALLOW_LOCAL_FETCH once per process; "1" enables local fetches.
bool local_fetch_allowed();

} // namespace funes::net
