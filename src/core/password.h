// =============================================================================
// src/core/password.h — password hashing and token generation
// =============================================================================
//
// PBKDF2-HMAC-SHA256 via OpenSSL. OpenSSL is not a new dependency: httplib
// already requires it for HTTPS (cloud LLM APIs, web_search, web_fetch), so
// authentication adds nothing to the build. bcrypt or argon2 would resist
// offline GPU cracking better, but each costs a vendored crypto library for a
// threat model — someone already holding a copy of ~/.funes/memory.db — that a
// household appliance mostly addresses by not handing out the file.
//
// Stored form is self-describing so the cost can be raised later without
// invalidating existing hashes:
//
//     pbkdf2_sha256$<iterations>$<base64 salt>$<base64 hash>
//
// verify_password() reads the iteration count out of the stored string rather
// than assuming the current default, so old rows keep verifying after a bump.
// =============================================================================

#pragma once
#include <cstddef>
#include <string>

namespace funes {

// Hash a password for storage. Generates a fresh random salt each call, so
// the same password never produces the same output twice.
std::string hash_password(const std::string& password);

// True only if `password` matches `encoded`. Returns false — never throws —
// for a malformed, truncated, or empty `encoded`, so a damaged or
// half-migrated password_hash column fails closed instead of open.
bool verify_password(const std::string& password, const std::string& encoded);

// Cryptographically random token, hex-encoded (so the result is 2*bytes
// characters). Used for session cookies and the service token.
std::string random_token(std::size_t bytes = 32);

// Length-aware, content-constant-time comparison. Used for secrets compared
// on every request (tokens), where an early-exit strcmp leaks position.
bool constant_time_equals(const std::string& a, const std::string& b);

} // namespace funes
