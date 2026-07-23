// =============================================================================
// tests/test_text_utils.cpp — looks_like_text / truncate_utf8_safe
// =============================================================================

#include "text_utils.h"
#include <iostream>
#include <string>

using json = nlohmann::json;

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::cerr << "FAILED at " << __FILE__ << ":" << __LINE__ << " — " #cond "\n"; \
        return 1; \
    } \
} while (0)

int test_looks_like_text() {
    CHECK(funes::looks_like_text(""));
    CHECK(funes::looks_like_text("hello, world"));
    CHECK(funes::looks_like_text("café \xc3\xa9 \xe4\xb8\xad\xe6\x96\x87"));  // é, 中文

    std::string with_nul = "hello";
    with_nul += '\0';
    with_nul += "world";
    CHECK(!funes::looks_like_text(with_nul));

    CHECK(!funes::looks_like_text(std::string("\xff\xfe\x00\x01\x02", 5)));  // typical binary header
    CHECK(!funes::looks_like_text(std::string("\xc3")));                     // truncated 2-byte sequence
    CHECK(!funes::looks_like_text(std::string("\x80")));                     // stray continuation byte
    return 0;
}

int test_truncate_utf8_safe() {
    std::string s = "abcdef";
    funes::truncate_utf8_safe(s, 10);
    CHECK(s == "abcdef");  // no-op, already under the cap

    s = "abcdef";
    funes::truncate_utf8_safe(s, 3);
    CHECK(s == "abc");

    // 3-byte UTF-8 sequence (e.g. 中, E4 B8 AD) cut right in the middle.
    s = "ab\xe4\xb8\xad" "cd";
    funes::truncate_utf8_safe(s, 3);  // lands after "ab" + first byte of the sequence
    CHECK(s == "ab");
    CHECK(funes::looks_like_text(s));

    s = "ab\xe4\xb8\xad" "cd";
    funes::truncate_utf8_safe(s, 4);  // lands after "ab" + first 2 bytes of the sequence
    CHECK(s == "ab");
    CHECK(funes::looks_like_text(s));

    s = "ab\xe4\xb8\xad" "cd";
    funes::truncate_utf8_safe(s, 5);  // lands exactly on the full sequence boundary
    CHECK(s == "ab\xe4\xb8\xad");
    CHECK(funes::looks_like_text(s));
    return 0;
}

int test_dump_safe() {
    // Valid content dumps normally.
    const json good = {{"text", "hello"}};
    CHECK(funes::dump_safe(good) == good.dump());

    // Invalid UTF-8 would make a bare .dump() throw json::type_error.316 —
    // dump_safe must not throw, and must still produce valid JSON output.
    const json bad = {{"text", std::string("caf\xe9")}};
    std::string out;
    bool threw = false;
    try { out = funes::dump_safe(bad); }
    catch (...) { threw = true; }
    CHECK(!threw);
    CHECK(!out.empty());
    // The result itself must be valid, re-parseable JSON.
    bool reparse_threw = false;
    json reparsed;
    try { reparsed = json::parse(out); } catch (...) { reparse_threw = true; }
    CHECK(!reparse_threw);
    CHECK(reparsed.is_object());

    // A bare dump() on the same value really does throw — confirms the test
    // is exercising the actual failure mode, not a no-op.
    bool bare_dump_threw = false;
    try { (void)bad.dump(); } catch (const json::type_error&) { bare_dump_threw = true; }
    CHECK(bare_dump_threw);

    return 0;
}

int main() {
    int rc = 0;
    rc |= test_looks_like_text();
    rc |= test_truncate_utf8_safe();
    rc |= test_dump_safe();
    if (rc == 0) std::cout << "test_text_utils: all tests passed\n";
    return rc;
}
