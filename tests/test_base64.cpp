// =============================================================================
// tests/test_base64.cpp — base64_encode / base64_decode
// =============================================================================

#include "base64.h"
#include <iostream>
#include <string>
#include <vector>

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::cerr << "FAILED at " << __FILE__ << ":" << __LINE__ << " — " #cond "\n"; \
        return 1; \
    } \
} while (0)

int test_known_vectors() {
    // RFC 4648 test vectors.
    CHECK(funes::base64_encode("") == "");
    CHECK(funes::base64_encode("f") == "Zg==");
    CHECK(funes::base64_encode("fo") == "Zm8=");
    CHECK(funes::base64_encode("foo") == "Zm9v");
    CHECK(funes::base64_encode("foob") == "Zm9vYg==");
    CHECK(funes::base64_encode("fooba") == "Zm9vYmE=");
    CHECK(funes::base64_encode("foobar") == "Zm9vYmFy");
    return 0;
}

int test_roundtrip() {
    const std::vector<std::string> cases = {
        "", "a", "ab", "abc", "abcd",
        std::string("\x00\x01\x02\xff\xfe", 5),  // arbitrary binary, embedded NUL
        std::string(1000, 'x'),                   // longer than one internal buffer
    };
    for (const auto& s : cases) {
        std::string encoded = funes::base64_encode(s);
        std::string decoded;
        CHECK(funes::base64_decode(encoded, decoded));
        CHECK(decoded == s);
    }
    return 0;
}

int test_decode_rejects_invalid() {
    std::string out;
    CHECK(!funes::base64_decode("not valid base64!!", out));
    CHECK(!funes::base64_decode("Z", out));  // impossible length (1 char after stripping)
    return 0;
}

int test_decode_ignores_whitespace() {
    std::string out;
    CHECK(funes::base64_decode("Zm9v\nYmFy", out));
    CHECK(out == "foobar");
    return 0;
}

int main() {
    int rc = 0;
    rc |= test_known_vectors();
    rc |= test_roundtrip();
    rc |= test_decode_rejects_invalid();
    rc |= test_decode_ignores_whitespace();
    if (rc == 0) std::cout << "test_base64: all tests passed\n";
    return rc;
}
