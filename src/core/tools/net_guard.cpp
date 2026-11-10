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
// A name that resolves to nothing is *refused* — the fetch was going to fail
// anyway, and "could not check" must not read as "checked and fine".
//
// Until 7.0 the address was looked up again when httplib connected, so a DNS
// answer that changed between the two lookups (rebinding with a zero TTL) got
// through. check_host now returns the address it judged and callers pin the
// connection to it (pin_address); is_private_host is the verdict alone.
namespace {

std::function<std::vector<std::string>(const std::string&)>& test_resolver() {
    static std::function<std::vector<std::string>(const std::string&)> fn;
    return fn;
}

// Every numeric address the name has, IPv4 first. IPv4 first because pinning
// gives up httplib's walk down the address list: a dual-stack name pinned to
// an IPv6 address on a host with no IPv6 route would fail where the unpinned
// connect used to fall back.
std::vector<std::string> resolve_all(const std::string& host) {
    if (test_resolver()) return test_resolver()(host);

    addrinfo hints{};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    // No AI_ADDRCONFIG: judge every address the name has, including a family
    // this box cannot currently use. Otherwise a v6 literal on a v4-only host
    // is "unresolvable" and refused for the wrong reason, and the verdict
    // changes with the network the server happens to be on.
    hints.ai_flags    = 0;

    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) return {};

    std::vector<std::string> v4, v6;
    char buf[INET6_ADDRSTRLEN];
    for (const addrinfo* p = res; p; p = p->ai_next) {
        if (p->ai_family == AF_INET &&
            inet_ntop(AF_INET, &reinterpret_cast<const sockaddr_in*>(p->ai_addr)->sin_addr,
                      buf, sizeof buf))
            v4.emplace_back(buf);
        else if (p->ai_family == AF_INET6 &&
                 inet_ntop(AF_INET6, &reinterpret_cast<const sockaddr_in6*>(p->ai_addr)->sin6_addr,
                           buf, sizeof buf))
            v6.emplace_back(buf);
    }
    freeaddrinfo(res);
    v4.insert(v4.end(), v6.begin(), v6.end());
    return v4;
}

bool is_private_address(const std::string& addr) {
    in_addr a4;
    in6_addr a6;
    if (inet_pton(AF_INET, addr.c_str(), &a4) == 1)  return is_private_v4(a4);
    if (inet_pton(AF_INET6, addr.c_str(), &a6) == 1) return is_private_v6(a6);
    return true;   // not an address we can judge: refuse
}

} // namespace

void set_resolver_for_testing(std::function<std::vector<std::string>(const std::string&)> fn) {
    test_resolver() = std::move(fn);
}

HostCheck check_host(const std::string& raw_host) {
    std::string host = raw_host;
    // Bracketed IPv6 literal as it appears in a URL.
    if (host.size() >= 2 && host.front() == '[' && host.back() == ']')
        host = host.substr(1, host.size() - 2);
    // A trailing dot is the same name to the resolver but not to a regex.
    while (!host.empty() && host.back() == '.') host.pop_back();
    if (host.empty()) return {};

    const std::vector<std::string> addrs = resolve_all(host);
    HostCheck c;
    c.address    = addrs.empty() ? "" : addrs.front();
    c.is_private = addrs.empty();   // unresolvable: refuse, do not guess
    for (const auto& a : addrs)
        if (is_private_address(a)) { c.is_private = true; break; }

    // "localhost" and its subdomains are loopback by convention even where the
    // resolver has not been told so.
    std::string lower;
    for (char ch : host) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (lower == "localhost" || (lower.size() > 10
        && lower.compare(lower.size() - 10, 10, ".localhost") == 0))
        c.is_private = true;
    return c;
}

bool is_private_host(const std::string& host) { return check_host(host).is_private; }

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
