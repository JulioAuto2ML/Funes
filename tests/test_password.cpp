// =============================================================================
// tests/test_password.cpp — password hashing and token generation
// =============================================================================
// PBKDF2-HMAC-SHA256 via OpenSSL, which Funes already links (httplib needs it
// for HTTPS), so authentication costs no new dependency.
//
// What matters here is not that the KDF is correct — that's OpenSSL's job —
// but that the encode/verify wrapper around it can't be tricked: a malformed
// stored hash must fail closed rather than crash or, worse, compare equal.

#include "password.h"
#include <algorithm>
#include <cstdio>
#include <iostream>
#include <set>
#include <string>

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::cerr << "FAILED at " << __FILE__ << ":" << __LINE__ << " — " #cond "\n"; \
        return 1; \
    } \
} while (0)

int test_hash_and_verify() {
    const std::string pw = "correct horse battery staple";
    std::string encoded = funes::hash_password(pw);

    // Shape: algorithm$iterations$salt$hash — four fields, recognisable prefix.
    CHECK(!encoded.empty());
    CHECK(encoded.rfind("pbkdf2_sha256$", 0) == 0);
    CHECK(std::count(encoded.begin(), encoded.end(), '$') == 3);

    // The password itself must never appear in the stored form.
    CHECK(encoded.find(pw) == std::string::npos);

    CHECK(funes::verify_password(pw, encoded));
    CHECK(!funes::verify_password("wrong password", encoded));
    CHECK(!funes::verify_password("", encoded));

    // Off-by-one variants, to be sure it isn't comparing prefixes.
    CHECK(!funes::verify_password("correct horse battery stapl", encoded));
    CHECK(!funes::verify_password("correct horse battery staple ", encoded));
    return 0;
}

int test_salt_is_random() {
    // Same password, different stored hashes — otherwise one rainbow table
    // covers every account, and equal hashes leak "these two users share a
    // password" to anyone who reads the database.
    const std::string pw = "hunter2";
    std::set<std::string> seen;
    for (int i = 0; i < 8; ++i) {
        std::string h = funes::hash_password(pw);
        CHECK(seen.insert(h).second);       // never repeats
        CHECK(funes::verify_password(pw, h)); // and each one still verifies
    }
    return 0;
}

int test_malformed_hashes_fail_closed() {
    // Anything that isn't a well-formed stored hash must verify as false, not
    // throw and not accidentally succeed. An empty password_hash column (a
    // half-finished migration, a hand-edited row) is the dangerous case: it
    // must not mean "any password works".
    const char* bad[] = {
        "",
        "$",
        "$$$",
        "pbkdf2_sha256$",
        "pbkdf2_sha256$600000$",
        "pbkdf2_sha256$600000$c2FsdA==$",       // empty hash field
        "pbkdf2_sha256$notanumber$c2FsdA==$aGE=",
        "pbkdf2_sha256$0$c2FsdA==$aGE=",        // zero iterations
        "pbkdf2_sha256$-5$c2FsdA==$aGE=",       // negative iterations
        "bcrypt$600000$c2FsdA==$aGE=",          // wrong algorithm
        "pbkdf2_sha256$600000$!!!not base64!!!$aGE=",
        "plaintextpassword",
    };
    for (const char* b : bad) {
        CHECK(!funes::verify_password("anything", b));
        CHECK(!funes::verify_password("", b));
    }
    return 0;
}

int test_iteration_count_is_respected() {
    // The stored iteration count must actually drive verification, so an
    // attacker who can write to the DB can't downgrade a hash to 1 round and
    // still have it verify against the original hash bytes.
    std::string encoded = funes::hash_password("pw");
    std::string downgraded = encoded;
    size_t first = downgraded.find('$');
    size_t second = downgraded.find('$', first + 1);
    downgraded = downgraded.substr(0, first + 1) + "1" + downgraded.substr(second);

    CHECK(!funes::verify_password("pw", downgraded));
    return 0;
}

int test_random_token() {
    std::set<std::string> seen;
    for (int i = 0; i < 32; ++i) {
        std::string t = funes::random_token(32);
        CHECK(t.size() == 64);  // 32 bytes as hex
        for (char c : t)
            CHECK((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
        CHECK(seen.insert(t).second);
    }
    CHECK(funes::random_token(8).size() == 16);
    return 0;
}

int test_constant_time_equals() {
    CHECK(funes::constant_time_equals("abc", "abc"));
    CHECK(!funes::constant_time_equals("abc", "abd"));
    CHECK(!funes::constant_time_equals("abc", "ab"));   // length differs
    CHECK(!funes::constant_time_equals("", "a"));
    CHECK(funes::constant_time_equals("", ""));
    return 0;
}

int main() {
    int rc = 0;
    rc |= test_hash_and_verify();
    rc |= test_salt_is_random();
    rc |= test_malformed_hashes_fail_closed();
    rc |= test_iteration_count_is_respected();
    rc |= test_random_token();
    rc |= test_constant_time_equals();
    if (rc == 0) std::cout << "test_password: all passed\n";
    return rc;
}
