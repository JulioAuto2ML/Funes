// =============================================================================
// tests/test_parallel_search.cpp — Parallel as a web_search backend
// =============================================================================
// Parallel's free search MCP answers a web_search with JSON whose excerpts are
// already query-focused, so web_search can use it in place of Tavily and keep
// Tavily as the fallback. What matters here: the reply is turned into the same
// Hits Tavily produces (so format_hits, the social-domain exclusion and the
// store exemption keep working unchanged), a failure falls back instead of
// failing the search, and the whole MCP exchange works against a real server.

#include "httplib.h"
#include "json.hpp"
#include "tools/parallel_search.h"
#include "tools/search_backends.h"
#include "tools/tavily.h"
#include <cstdlib>
#include <iostream>
#include <thread>

using json = nlohmann::json;

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::cerr << "FAILED at " << __FILE__ << ":" << __LINE__ << " — " #cond "\n"; \
        return 1; \
    } \
} while (0)

// The shape search.parallel.ai returned on 2026-10-26, cut down.
static const char* kReply = R"({
  "search_id": "search_1",
  "results": [
    {"url": "https://depost.ai/blog/linkedin-algorithm",
     "title": "How the LinkedIn Algorithm Works in 2026",
     "publish_date": "2026-09-07",
     "excerpts": ["LinkedIn rebuilt its feed ranking in March 2026.", "Relevance can now outrank recency."]},
    {"url": "https://www.youtube.com/watch?v=x", "title": "Video", "publish_date": null,
     "excerpts": ["a video"]},
    {"url": "https://notyoutube.com/a", "title": "Lookalike", "publish_date": null,
     "excerpts": null},
    {"url": "", "title": "No URL", "excerpts": ["dropped"]}
  ],
  "warnings": null
})";

static int test_parse() {
    std::vector<funes::tavily::Hit> hits;
    const std::string err = funes::parallel::parse_results(
        kReply, funes::tavily::default_exclude_domains(), hits);
    CHECK(err.empty());
    // youtube.com excluded by suffix; notyoutube.com is a different domain;
    // a hit without a URL is dropped like Tavily's.
    CHECK(hits.size() == 2);
    CHECK(hits[0].title == "How the LinkedIn Algorithm Works in 2026");
    CHECK(hits[0].published == "2026-09-07");
    CHECK(hits[0].snippet.find("March 2026") != std::string::npos);
    CHECK(hits[0].snippet.find("outrank recency") != std::string::npos);
    CHECK(hits[1].url == "https://notyoutube.com/a");
    CHECK(hits[1].snippet.empty());   // null excerpts must not throw

    hits.clear();
    CHECK(!funes::parallel::parse_results("not json", {}, hits).empty());
    CHECK(funes::parallel::parse_results(R"({"results": []})", {}, hits).empty());
    CHECK(hits.empty());
    return 0;
}

static int test_fallback() {
    using funes::search::Backend;
    using funes::tavily::Hit;
    using funes::tavily::Query;
    int fallback_calls = 0;
    const Backend ok{"primary", 500, [](const Query&, std::vector<Hit>& out) {
        out.push_back({"t", "s", "https://a.example/", ""}); return std::string(); }};
    const Backend bad{"primary", 500, [](const Query&, std::vector<Hit>&) {
        return std::string("rate limited"); }};
    const Backend fb{"tavily", 300, [&](const Query&, std::vector<Hit>& out) {
        ++fallback_calls; out.push_back({"f", "s", "https://f.example/", ""});
        return std::string(); }};
    const Backend fb_bad{"tavily", 300, [](const Query&, std::vector<Hit>&) {
        return std::string("not configured"); }};

    Query q; q.query = "x";
    std::vector<Hit> hits;
    int used = -1;   // index into the list passed, -1 = none

    // Primary succeeds: the fallback is never asked.
    CHECK(funes::search::run({ok, fb}, q, hits, used).empty());
    CHECK(used == 0 && fallback_calls == 0 && hits.size() == 1);

    // Primary fails: the fallback answers, and is the one reported as used.
    hits.clear();
    CHECK(funes::search::run({bad, fb}, q, hits, used).empty());
    CHECK(used == 1 && fallback_calls == 1 && hits[0].title == "f");

    // Both fail: one error naming both, so the model (and the log) sees why.
    hits.clear();
    const std::string err = funes::search::run({bad, fb_bad}, q, hits, used);
    CHECK(err.find("rate limited") != std::string::npos);
    CHECK(err.find("not configured") != std::string::npos);
    CHECK(used == -1);

    // Backend order comes from FUNES_SEARCH_BACKEND; Tavily is the default and
    // always the last resort.
    unsetenv("FUNES_SEARCH_BACKEND");
    auto order = funes::search::configured_backends();
    CHECK(order.size() == 1 && order[0].name == "tavily");
    setenv("FUNES_SEARCH_BACKEND", "parallel", 1);
    order = funes::search::configured_backends();
    CHECK(order.size() == 2 && order[0].name == "parallel" && order[1].name == "tavily");
    // Parallel's excerpts are the content, so it gets a longer cut than Tavily's.
    CHECK(order[0].snippet_limit > order[1].snippet_limit);
    setenv("FUNES_SEARCH_BACKEND", "bogus", 1);
    order = funes::search::configured_backends();
    CHECK(order.size() == 1 && order[0].name == "tavily");   // a typo is not "no search"
    unsetenv("FUNES_SEARCH_BACKEND");
    return 0;
}

// The real exchange against a mock MCP server: initialize, then tools/call
// web_search with the query as both objective and search query, an API key
// sent as a bearer token when configured, and an isError result reported.
static int test_mcp_exchange() {
    httplib::Server svr;
    json last_args;
    std::string auth;
    bool fail_call = false;
    svr.Post("/mcp", [&](const httplib::Request& req, httplib::Response& res) {
        json m = json::parse(req.body);
        if (!m.contains("id")) { res.status = 202; return; }
        auth = req.get_header_value("Authorization");
        const std::string method = m.value("method", "");
        json result;
        if (method == "initialize") {
            result = {{"capabilities", {{"tools", json::object()}}}};
        } else {
            last_args = m["params"]["arguments"];
            result = {{"content", json::array({{{"type", "text"},
                                                {"text", fail_call ? "quota exceeded" : kReply}}})},
                      {"isError", fail_call}};
        }
        res.set_content(json{{"jsonrpc", "2.0"}, {"id", m["id"]}, {"result", result}}.dump(),
                        "application/json");
    });
    const int port = svr.bind_to_any_port("127.0.0.1");
    std::thread t([&] { svr.listen_after_bind(); });
    svr.wait_until_ready();
    setenv("FUNES_PARALLEL_URL", ("http://127.0.0.1:" + std::to_string(port) + "/mcp").c_str(), 1);

    funes::tavily::Query q;
    q.query = "linkedin feed algorithm";
    q.max_results = 1;
    std::vector<funes::tavily::Hit> hits;
    unsetenv("FUNES_PARALLEL_API_KEY");
    const std::string err = funes::parallel::search(q, hits);
    if (!err.empty()) std::cerr << err << "\n";
    CHECK(err.empty());
    CHECK(hits.size() == 1);   // max_results honoured
    CHECK(last_args["objective"] == "linkedin feed algorithm");
    CHECK(last_args["search_queries"][0] == "linkedin feed algorithm");
    CHECK(auth.empty());       // the free tier takes no key

    setenv("FUNES_PARALLEL_API_KEY", "k-123", 1);
    hits.clear();
    q.time_range = "week";
    CHECK(funes::parallel::search(q, hits).empty());
    CHECK(auth == "Bearer k-123");
    // Parallel has no recency filter; the window goes into the objective.
    CHECK(last_args["objective"].get<std::string>().find("week") != std::string::npos);

    fail_call = true;
    hits.clear();
    const std::string tool_err = funes::parallel::search(q, hits);
    CHECK(tool_err.find("quota exceeded") != std::string::npos);

    svr.stop();
    t.join();
    // Server gone: an error string, not an exception.
    hits.clear();
    CHECK(!funes::parallel::search(q, hits).empty());
    unsetenv("FUNES_PARALLEL_URL");
    unsetenv("FUNES_PARALLEL_API_KEY");
    return 0;
}

int main() {
    if (test_parse()) return 1;
    if (test_fallback()) return 1;
    if (test_mcp_exchange()) return 1;
    std::cout << "test_parallel_search: all checks passed\n";
    return 0;
}
