// =============================================================================
// tests/test_embedding_client.cpp — an input too long to embed is shortened
// =============================================================================
// yoda, 2026-11-04 → 11-10: one auto-memory held a whole attached résumé
// (14.5k chars). The embedding server (n_ctx 2048) answered 400 "input is
// larger than the max context size"; EmbeddingClient threw it like any other
// failure, and the startup backfill read that as "endpoint down" — stopped,
// retried twelve times, gave up, and did it again on every restart. The memory
// never got a vector, and anything missing one after it never would either.
//
// The mock here refuses with llama-server's exact error body, so the test
// covers the shape the real server sends.

#include "httplib.h"
#include "llm_client.h"
#include "memory.h"
#include <atomic>
#include <filesystem>
#include <iostream>
#include <thread>

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::cerr << "FAILED at " << __FILE__ << ":" << __LINE__ << " — " #cond "\n"; \
        return 1; \
    } \
} while (0)

namespace {

// "Tokens" are 4 characters each; anything over kCtx of them is refused.
constexpr int kCtx = 100;

struct MockEmbedder {
    httplib::Server srv;
    std::thread th;
    int port = 0;
    std::atomic<int> calls{0};
    std::atomic<size_t> last_accepted{0};

    MockEmbedder() {
        srv.Post("/v1/embeddings", [this](const httplib::Request& req, httplib::Response& res) {
            ++calls;
            const auto body = nlohmann::json::parse(req.body);
            const std::string input = body["input"].get<std::string>();
            const int tokens = static_cast<int>(input.size() / 4);
            if (tokens > kCtx) {
                res.status = 400;
                res.set_content(
                    "{\"error\":{\"code\":400,\"message\":\"input (" + std::to_string(tokens) +
                    " tokens) is larger than the max context size (" + std::to_string(kCtx) +
                    " tokens). skipping\",\"type\":\"exceed_context_size_error\","
                    "\"n_prompt_tokens\":" + std::to_string(tokens) +
                    ",\"n_ctx\":" + std::to_string(kCtx) + "}}",
                    "application/json");
                return;
            }
            last_accepted = input.size();
            res.set_content(R"({"data":[{"embedding":[0.1,0.2,0.3]}]})", "application/json");
        });
        port = srv.bind_to_any_port("127.0.0.1");
        th = std::thread([this] { srv.listen_after_bind(); });
        for (int i = 0; i < 50 && !srv.is_running(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    ~MockEmbedder() { srv.stop(); if (th.joinable()) th.join(); }
    std::string url() const { return "http://127.0.0.1:" + std::to_string(port); }
};

} // namespace

int test_too_long_input_is_shortened_to_fit() {
    MockEmbedder mock;
    EmbeddingClient client(mock.url(), "", "test");

    // Fits: one call, sent whole.
    CHECK(client.embed(std::string(200, 'a')).size() == 3);
    CHECK(mock.calls == 1);
    CHECK(mock.last_accepted == 200);

    // 3.5x the context: refused once, then sent shortened and accepted.
    mock.calls = 0;
    CHECK(client.embed(std::string(1400, 'b')).size() == 3);
    CHECK(mock.calls == 2);
    CHECK(mock.last_accepted > 0);
    CHECK(mock.last_accepted <= kCtx * 4);
    CHECK(mock.last_accepted >= kCtx * 4 / 2);   // shortened to fit, not to nothing
    return 0;
}

// Other errors still throw: shortening is for "too long", not for everything.
int test_other_errors_still_throw() {
    httplib::Server srv;
    srv.Post("/v1/embeddings", [](const httplib::Request&, httplib::Response& res) {
        res.status = 400;
        res.set_content(R"({"error":{"message":"model not found"}})", "application/json");
    });
    const int port = srv.bind_to_any_port("127.0.0.1");
    std::thread th([&] { srv.listen_after_bind(); });
    for (int i = 0; i < 50 && !srv.is_running(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));

    EmbeddingClient client("http://127.0.0.1:" + std::to_string(port), "", "test");
    bool threw = false;
    try { client.embed("hello"); } catch (const std::runtime_error&) { threw = true; }
    srv.stop();
    th.join();
    CHECK(threw);
    return 0;
}

// End to end through the backfill: the long memory gets its vector and so
// does the one stored after it, which used to be stuck behind it.
int test_backfill_embeds_a_long_memory() {
    namespace fs = std::filesystem;
    MockEmbedder mock;
    const fs::path db = fs::temp_directory_path() / "funes_test_embedding_client.db";
    fs::remove(db);
    {
        EmbeddingClient client(mock.url(), "", "test");
        MemoryStore memory(db.string(), &client);
        // Stored with the endpoint "down" for long inputs is the yoda state;
        // here remember() itself now succeeds, so check the count directly.
        memory.remember(1, "funes", std::string(4000, 'r'), "auto");
        memory.remember(1, "funes", "a short memory after it", "auto");
        CHECK(memory.count_missing_vectors() == 0);
        CHECK(memory.semantic_available());
    }
    fs::remove(db);
    return 0;
}

int main() {
    int rc = 0;
    rc |= test_too_long_input_is_shortened_to_fit();
    rc |= test_other_errors_still_throw();
    rc |= test_backfill_embeds_a_long_memory();
    if (rc == 0) std::cout << "test_embedding_client: all tests passed\n";
    return rc;
}
