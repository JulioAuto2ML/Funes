// =============================================================================
// src/core/password.cpp — PBKDF2-HMAC-SHA256 password hashing (OpenSSL)
// =============================================================================

#include "password.h"
#include "base64.h"
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <vector>

namespace funes {
namespace {

// OWASP's 2023 floor for PBKDF2-HMAC-SHA256. Roughly 200ms on yoda-class
// hardware — deliberately slow, and only paid on login, not per request
// (that's what the token table is for).
constexpr int  DEFAULT_ITERATIONS = 600000;
constexpr int  SALT_BYTES         = 16;
constexpr int  HASH_BYTES         = 32;   // SHA-256 output
constexpr int  MAX_ITERATIONS     = 10000000;  // refuse absurd stored values

const char PREFIX[] = "pbkdf2_sha256$";

std::string random_bytes(std::size_t n) {
    std::vector<unsigned char> buf(n);
    // RAND_bytes, not rand(): this is key material. A failure here means the
    // system has no usable entropy source, which is not something to paper
    // over with a predictable fallback.
    if (RAND_bytes(buf.data(), static_cast<int>(n)) != 1)
        throw std::runtime_error("RAND_bytes failed: no secure entropy source available");
    return std::string(reinterpret_cast<char*>(buf.data()), n);
}

std::string derive(const std::string& password, const std::string& salt, int iterations) {
    std::vector<unsigned char> out(HASH_BYTES);
    if (PKCS5_PBKDF2_HMAC(password.data(), static_cast<int>(password.size()),
                          reinterpret_cast<const unsigned char*>(salt.data()),
                          static_cast<int>(salt.size()),
                          iterations, EVP_sha256(),
                          HASH_BYTES, out.data()) != 1)
        throw std::runtime_error("PKCS5_PBKDF2_HMAC failed");
    return std::string(reinterpret_cast<char*>(out.data()), HASH_BYTES);
}

// Strict base-10 parse: no leading +/-, no trailing junk, no overflow. strtol
// would accept "600000abc" and "-5", both of which must be rejected here.
bool parse_iterations(const std::string& s, int& out) {
    if (s.empty() || s.size() > 9) return false;
    long value = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        value = value * 10 + (c - '0');
    }
    if (value <= 0 || value > MAX_ITERATIONS) return false;
    out = static_cast<int>(value);
    return true;
}

} // namespace

std::string hash_password(const std::string& password) {
    const std::string salt = random_bytes(SALT_BYTES);
    const std::string hash = derive(password, salt, DEFAULT_ITERATIONS);
    return std::string(PREFIX) + std::to_string(DEFAULT_ITERATIONS) + "$" +
           base64_encode(salt) + "$" + base64_encode(hash);
}

bool verify_password(const std::string& password, const std::string& encoded) {
    // Every failure below is a plain `return false`. Distinguishing "malformed
    // hash" from "wrong password" to the caller would only give an attacker a
    // way to probe the shape of the store.
    if (encoded.size() < sizeof(PREFIX)) return false;
    if (encoded.compare(0, sizeof(PREFIX) - 1, PREFIX) != 0) return false;

    const std::size_t iter_start = sizeof(PREFIX) - 1;
    const std::size_t salt_sep   = encoded.find('$', iter_start);
    if (salt_sep == std::string::npos) return false;
    const std::size_t hash_sep   = encoded.find('$', salt_sep + 1);
    if (hash_sep == std::string::npos) return false;
    // A fourth '$' means this isn't the format we think it is.
    if (encoded.find('$', hash_sep + 1) != std::string::npos) return false;

    int iterations = 0;
    if (!parse_iterations(encoded.substr(iter_start, salt_sep - iter_start), iterations))
        return false;

    const std::string salt_b64 = encoded.substr(salt_sep + 1, hash_sep - salt_sep - 1);
    const std::string hash_b64 = encoded.substr(hash_sep + 1);
    if (salt_b64.empty() || hash_b64.empty()) return false;

    std::string salt, expected;
    if (!base64_decode(salt_b64, salt) || salt.empty()) return false;
    if (!base64_decode(hash_b64, expected) || expected.size() != HASH_BYTES) return false;

    try {
        return constant_time_equals(derive(password, salt, iterations), expected);
    } catch (const std::exception&) {
        return false;  // OpenSSL failure must not authenticate anyone
    }
}

std::string random_token(std::size_t bytes) {
    static const char hex[] = "0123456789abcdef";
    const std::string raw = random_bytes(bytes);
    std::string out;
    out.reserve(bytes * 2);
    for (unsigned char c : raw) {
        out += hex[c >> 4];
        out += hex[c & 0x0f];
    }
    return out;
}

bool constant_time_equals(const std::string& a, const std::string& b) {
    // Length is not secret here (tokens are fixed-width, hashes are 32 bytes),
    // so an early length check leaks nothing an attacker doesn't already know.
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i)
        diff |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
    return diff == 0;
}

} // namespace funes
