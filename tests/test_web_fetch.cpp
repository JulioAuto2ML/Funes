// =============================================================================
// tests/test_web_fetch.cpp — web_fetch against a non-UTF-8 response
// =============================================================================
// The actual risk this guards against: a page served as text/* whose body
// isn't valid UTF-8 (wrong charset, or not really text at all) must not
// crash web_fetch or anything downstream that JSON-serializes its result.
// Spins up a real local httplib::Server rather than mocking, so this
// exercises the exact code path a real fetch would.

#include "httplib.h"
#include "tools.h"
#include "tools/page_text.h"
#include <cstdlib>
#include <iostream>
#include <thread>

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::cerr << "FAILED at " << __FILE__ << ":" << __LINE__ << " — " #cond "\n"; \
        return 1; \
    } \
} while (0)

int test_non_utf8_response_does_not_crash() {
    // web_fetch refuses loopback/private hosts by default; opt in the same
    // way a real deployment would, since the whole point here is fetching
    // from a local mock server.
    setenv("FUNES_ALLOW_LOCAL_FETCH", "1", 1);

    httplib::Server srv;
    srv.Get("/bad-charset", [](const httplib::Request&, httplib::Response& res) {
        // Latin-1 bytes (0xE9 = 'é' in Latin-1) are not valid UTF-8 on their own.
        res.set_content(std::string("Caf\xe9 con leche"), "text/plain");
    });
    srv.Get("/binary", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(std::string("\xff\xd8\xff\xe0\x00\x10JFIF", 10), "text/plain");
    });
    srv.Get("/fine", [](const httplib::Request&, httplib::Response& res) {
        res.set_content("perfectly ordinary text", "text/plain");
    });

    const int port = srv.bind_to_any_port("127.0.0.1");
    std::thread th([&] { srv.listen_after_bind(); });
    // Give the listener a moment to actually start accepting.
    for (int i = 0; i < 50 && !srv.is_running(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));

    // A joinable std::thread destructing on an early CHECK failure would
    // itself abort the process — make sure the server always gets torn down.
    struct ServerGuard {
        httplib::Server& srv;
        std::thread& th;
        ~ServerGuard() { srv.stop(); if (th.joinable()) th.join(); }
    } guard{srv, th};

    ToolRegistry reg;
    register_web_tools(reg);
    ToolContext ctx{"funes", "s1"};
    const std::string base = "http://127.0.0.1:" + std::to_string(port);

    // The actual assertion is as much "the process is still alive to check
    // this" as it is the specific message.
    auto bad = reg.call("web_fetch", {{"url", base + "/bad-charset"}}, ctx);
    CHECK(bad.error);
    CHECK(bad.text.find("valid UTF-8") != std::string::npos);

    auto binary = reg.call("web_fetch", {{"url", base + "/binary"}}, ctx);
    CHECK(binary.error);
    CHECK(binary.text.find("valid UTF-8") != std::string::npos);

    auto fine = reg.call("web_fetch", {{"url", base + "/fine"}}, ctx);
    CHECK(!fine.error);
    CHECK(fine.text.find("perfectly ordinary text") != std::string::npos);

    return 0;
}

// A large inline <script> block used to crash the process: html_to_text used
// std::regex with `<script[\s\S]*?</script>`, and libstdc++'s regex engine
// recurses once per repetition for quantified subexpressions, blowing the
// stack on an ordinary large script block (real pages routinely ship
// hundreds of KB of inline JS) — not a contrived adversarial input.
int test_large_inline_script_does_not_crash() {
    setenv("FUNES_ALLOW_LOCAL_FETCH", "1", 1);

    std::string big_script = "console.log('x');\n";
    while (big_script.size() < 500 * 1024) big_script += big_script;

    httplib::Server srv;
    srv.Get("/big-script", [&](const httplib::Request&, httplib::Response& res) {
        std::string body = "<html><head><script>" + big_script +
                            "</script></head><body><p>Hello world</p></body></html>";
        res.set_content(body, "text/html");
    });

    const int port = srv.bind_to_any_port("127.0.0.1");
    std::thread th([&] { srv.listen_after_bind(); });
    for (int i = 0; i < 50 && !srv.is_running(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));

    struct ServerGuard {
        httplib::Server& srv;
        std::thread& th;
        ~ServerGuard() { srv.stop(); if (th.joinable()) th.join(); }
    } guard{srv, th};

    ToolRegistry reg;
    register_web_tools(reg);
    ToolContext ctx{"funes", "s1"};
    const std::string base = "http://127.0.0.1:" + std::to_string(port);

    auto res = reg.call("web_fetch", {{"url", base + "/big-script"}}, ctx);
    CHECK(!res.error);
    CHECK(res.text.find("Hello world") != std::string::npos);
    CHECK(res.text.find("console.log") == std::string::npos);

    return 0;
}

// A CNBC-style page hit this in production: a nav-heavy header pushed the
// real article text past a candidate's excerpt window entirely, leaving
// nothing quotable and forcing the model to fake "evidence" from the title.
// html_to_text dropped <script>/<style> content but not <nav>/<header>/
// <footer>/<aside> — this locks in that those four are dropped the same way.
int test_boilerplate_landmarks_are_dropped() {
    using funes::web::html_to_text;

    std::string html =
        "<html><body>"
        "<header><nav><ul><li><a href='/markets'>Markets</a></li>"
        "<li><a href='/business'>Business</a></li>"
        "<li><a href='/tech'>Tech</a></li></ul></nav></header>"
        "<article><h1>Real headline</h1>"
        "<p>The actual article text goes here.</p></article>"
        "<aside><div>Related: five other stories you might like</div></aside>"
        "<footer><p>Copyright 2026. Contact us. Privacy policy.</p></footer>"
        "</body></html>";

    std::string text = html_to_text(html);
    CHECK(text.find("Markets") == std::string::npos);
    CHECK(text.find("Business") == std::string::npos);
    CHECK(text.find("Related:") == std::string::npos);
    CHECK(text.find("Copyright") == std::string::npos);
    CHECK(text.find("Privacy policy") == std::string::npos);
    CHECK(text.find("Real headline") != std::string::npos);
    CHECK(text.find("The actual article text goes here.") != std::string::npos);

    // Tag names that merely start with one of the dropped names (e.g. a
    // hypothetical <navigation>) must not be mistaken for <nav> and eaten.
    std::string not_a_landmark =
        "<navigation>this is not a landmark tag</navigation>"
        "<asideways>neither is this</asideways>";
    std::string text2 = html_to_text(not_a_landmark);
    CHECK(text2.find("this is not a landmark tag") != std::string::npos);
    CHECK(text2.find("neither is this") != std::string::npos);

    return 0;
}

int test_reddit_url_rewrite() {
    using funes::web::rewrite_reddit;

    CHECK(rewrite_reddit("https://www.reddit.com/r/LocalLLaMA/comments/abc123/thread_title")
        == "https://old.reddit.com/r/LocalLLaMA/comments/abc123/thread_title");

    CHECK(rewrite_reddit("https://reddit.com/r/selfhosted/comments/xyz")
        == "https://old.reddit.com/r/selfhosted/comments/xyz");

    CHECK(rewrite_reddit("http://www.reddit.com/r/privacy")
        == "http://old.reddit.com/r/privacy");

    // Already old.reddit.com — no change.
    CHECK(rewrite_reddit("https://old.reddit.com/r/LocalLLaMA")
        == "https://old.reddit.com/r/LocalLLaMA");

    // Not Reddit at all — no change.
    CHECK(rewrite_reddit("https://news.ycombinator.com/item?id=123")
        == "https://news.ycombinator.com/item?id=123");

    return 0;
}

int main() {
    int rc = 0;
    rc |= test_non_utf8_response_does_not_crash();
    rc |= test_large_inline_script_does_not_crash();
    rc |= test_boilerplate_landmarks_are_dropped();
    rc |= test_reddit_url_rewrite();
    if (rc == 0) std::cout << "test_web_fetch: all tests passed\n";
    return rc;
}
