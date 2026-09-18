// =============================================================================
// src/core/tools/net_guard.cpp — shared outbound-request safety checks
// =============================================================================

#include "net_guard.h"
#include <arpa/inet.h>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <netdb.h>
#include <netinet/in.h>
#include <regex>
#include <sys/socket.h>

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

namespace {

// Is this IPv4 address one an LLM-chosen URL must not reach: loopback, the
// three RFC 1918 blocks, link-local (169.254/16 — where cloud metadata
// services live), the unspecified 0.0.0.0/8, or 100.64/10 (carrier NAT, and
// what Tailscale hands out — a LAN in everything but name).
bool is_private_v4(const in_addr& a) {
    const uint32_t ip = ntohl(a.s_addr);
    const uint8_t b0 = ip >> 24, b1 = (ip >> 16) & 0xff;
    if (b0 == 127)                       return true;   // loopback
    if (b0 == 10)                        return true;   // 10/8
    if (b0 == 192 && b1 == 168)          return true;   // 192.168/16
    if (b0 == 172 && (b1 & 0xf0) == 16)  return true;   // 172.16/12
    if (b0 == 169 && b1 == 254)          return true;   // link-local / metadata
    if (b0 == 0)                         return true;   // 0/8
    if (b0 == 100 && (b1 & 0xc0) == 64)  return true;   // 100.64/10
    return false;
}

bool is_private_v6(const in6_addr& a) {
    const uint8_t* b = a.s6_addr;
    if (IN6_IS_ADDR_LOOPBACK(&a))    return true;                 // ::1
    if (IN6_IS_ADDR_UNSPECIFIED(&a)) return true;                 // ::
    if ((b[0] & 0xfe) == 0xfc)       return true;                 // fc00::/7 ULA
    if (b[0] == 0xfe && (b[1] & 0xc0) == 0x80) return true;       // fe80::/10 link-local
    if (IN6_IS_ADDR_V4MAPPED(&a) || IN6_IS_ADDR_V4COMPAT(&a)) {   // ::ffff:a.b.c.d
        in_addr v4;
        std::memcpy(&v4, b + 12, 4);
        return is_private_v4(v4);
    }
    return false;
}

} // namespace

// Decided on the *addresses*, not the spelling. The previous version matched
// a regex against the host literal, which stopped "127.0.0.1" and let through
// every other way of saying it: a hostname that resolves there
// (127.0.0.1.nip.io, or any domain whose owner points an A record at your
// LAN), "0x7f000001", "2130706433", "[::ffff:127.0.0.1]", "localhost."
// with a trailing dot. So: resolve, and refuse if *any* answer is private —
// a name with one public and one private address is exactly the rebinding
// setup an attacker would build, and "some of the time it hits the LAN" is
// not a guard.
//
// Two honest limits. A name that resolves to nothing is *refused* — the
// fetch was going to fail anyway, and "could not check" must not read as
// "checked and fine". And the address is looked up again when httplib
// connects, so a DNS answer that changes between the two calls (deliberate
// rebinding with a zero TTL) still gets through; closing that needs the
// connection pinned to the checked address, which httplib does not offer.
// The window is one lookup wide instead of a regex wide.
bool is_private_host(const std::string& raw_host) {
    std::string host = raw_host;
    // Bracketed IPv6 literal as it appears in a URL.
    if (host.size() >= 2 && host.front() == '[' && host.back() == ']')
        host = host.substr(1, host.size() - 2);
    // A trailing dot is the same name to the resolver but not to a regex.
    while (!host.empty() && host.back() == '.') host.pop_back();
    if (host.empty()) return true;

    // "localhost" and its subdomains are loopback by convention even where the
    // resolver has not been told so.
    {
        std::string lower;
        for (char c : host) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (lower == "localhost" || (lower.size() > 10
            && lower.compare(lower.size() - 10, 10, ".localhost") == 0))
            return true;
    }

    addrinfo hints{};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    // No AI_ADDRCONFIG: judge every address the name has, including a family
    // this box cannot currently use. Otherwise a v6 literal on a v4-only host
    // is "unresolvable" and refused for the wrong reason, and the verdict
    // changes with the network the server happens to be on.
    hints.ai_flags    = 0;

    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res)
        return true;   // unresolvable: refuse, do not guess

    bool priv = false;
    for (const addrinfo* p = res; p && !priv; p = p->ai_next) {
        if (p->ai_family == AF_INET) {
            priv = is_private_v4(reinterpret_cast<const sockaddr_in*>(p->ai_addr)->sin_addr);
        } else if (p->ai_family == AF_INET6) {
            priv = is_private_v6(reinterpret_cast<const sockaddr_in6*>(p->ai_addr)->sin6_addr);
        }
    }
    freeaddrinfo(res);
    return priv;
}

std::string resolve_location(const ParsedUrl& base, const std::string& location) {
    if (location.empty()) return "";
    if (location.rfind("http://", 0) == 0 || location.rfind("https://", 0) == 0)
        return location;
    const std::string scheme = base.https ? "https" : "http";
    const bool default_port = (base.https && base.port == 443) || (!base.https && base.port == 80);
    const std::string origin = scheme + "://" + base.host +
                               (default_port ? "" : ":" + std::to_string(base.port));
    if (location.rfind("//", 0) == 0) return scheme + ":" + location;
    if (location[0] == '/')           return origin + location;
    // Relative to the directory of the current path.
    std::string dir = base.path;
    const size_t q = dir.find('?');
    if (q != std::string::npos) dir.erase(q);
    const size_t slash = dir.rfind('/');
    dir = (slash == std::string::npos) ? "/" : dir.substr(0, slash + 1);
    return origin + dir + location;
}

bool local_fetch_allowed() {
    const char* v = std::getenv("FUNES_ALLOW_LOCAL_FETCH");
    return v && *v == '1';
}

} // namespace funes::net
