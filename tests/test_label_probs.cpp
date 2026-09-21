// =============================================================================
// tests/test_label_probs.cpp — LLMClient::label_probs (classify_decision's primitive)
// =============================================================================
// label_probs' request/response handling is exercised the same way
// test_llm_client_images.cpp drives complete(): spin up a local mock server,
// capture the request body, and hand back a crafted llama.cpp-shaped
// response. Covers the wire format sent, correct normalization over just the
// candidate set (ignoring non-candidate tokens and leftover mass), the
// zero-match case, the two top-level response schemas label_probs accepts,
// and the Anthropic-provider refusal.
//
// "completion_probabilities" is the primary schema: verified 2026-09-21
// against the actual llama-server build running in production (yoda,
// Qwen3.8-9B-Q8_0) — not just documentation. "probs" is kept as a fallback
// for the schema ggml-org/llama.cpp's current tools/server/README.md
// documents, in case the serving side is ever upgraded past this build.

#include "httplib.h"
#include "llm_client.h"
#include <cmath>
#include <iostream>
#include <thread>

using json = nlohmann::json;

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::cerr << "FAILED at " << __FILE__ << ":" << __LINE__ << " — " #cond "\n"; \
        return 1; \
    } \
} while (0)

namespace {

struct MockServer {
    httplib::Server srv;
    std::thread th;
    std::string last_body;
    int port = 0;

    void start(const std::string& path, const std::string& response_body) {
        srv.Post(path.c_str(), [this, response_body](const httplib::Request& req, httplib::Response& res) {
            last_body = req.body;
            res.set_content(response_body, "application/json");
        });
        port = srv.bind_to_any_port("127.0.0.1");
        th = std::thread([this] { srv.listen_after_bind(); });
        for (int i = 0; i < 50 && !srv.is_running(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    ~MockServer() { srv.stop(); if (th.joinable()) th.join(); }
};

// logprob such that std::exp(logprob) == p, to the precision the test needs.
double lp(double p) { return std::log(p); }

bool approx(double a, double b) { return std::abs(a - b) < 1e-6; }

} // namespace

int test_hits_native_completion_endpoint_with_expected_fields() {
    MockServer mock;
    mock.start("/completion", R"({"completion_probabilities":[{"top_logprobs":[]}]})");

    LLMClient client("http://127.0.0.1:" + std::to_string(mock.port), "", "test-model", "openai");
    client.label_probs("State:\nx\n\nAnswer:", {" A", " B"}, "cache-key-1");

    json body = json::parse(mock.last_body);
    CHECK(body["prompt"] == "State:\nx\n\nAnswer:");
    CHECK(body["n_predict"] == 1);
    CHECK(body["n_probs"] == 40);
    CHECK(body["cache_prompt"] == true);
    CHECK(body["temperature"] == 0.0);
    CHECK(body.contains("id_slot"));
    return 0;
}

int test_normalizes_over_candidate_set_only() {
    // Shape verified against yoda's live llama-server (Qwen3.8-9B-Q8_0,
    // 2026-09-21): top-level "completion_probabilities", one entry per
    // generated token, each carrying a "top_logprobs" array of alternatives.
    MockServer mock;
    json resp = {
        {"completion_probabilities", json::array({
            {{"top_logprobs", json::array({
                {{"token", " A"}, {"logprob", lp(0.4)}},
                {{"token", " B"}, {"logprob", lp(0.2)}},
                {{"token", " C"}, {"logprob", lp(0.4)}}   // not a candidate — must be excluded
            })}}
        })}
    };
    mock.start("/completion", resp.dump());

    LLMClient client("http://127.0.0.1:" + std::to_string(mock.port), "", "test-model", "openai");
    auto out = client.label_probs("prompt", {" A", " B"}, "cache-key");

    CHECK(out.size() == 2);
    CHECK(out[0].label == " A");
    CHECK(out[1].label == " B");
    // Raw mass is 0.4 and 0.2 (0.4 for " C" discarded) — renormalized over
    // just A/B: 0.4/0.6 and 0.2/0.6.
    CHECK(approx(out[0].probability, 2.0 / 3.0));
    CHECK(approx(out[1].probability, 1.0 / 3.0));
    return 0;
}

int test_probs_schema_accepted_as_fallback() {
    // The schema ggml-org/llama.cpp's current server README documents —
    // renamed top-level key, same inner shape. Not what's deployed today,
    // but accepted so an upgrade doesn't silently zero every result.
    MockServer mock;
    json resp = {
        {"probs", json::array({
            {{"top_logprobs", json::array({
                {{"token", " A"}, {"logprob", lp(0.7)}},
                {{"token", " B"}, {"logprob", lp(0.3)}}
            })}}
        })}
    };
    mock.start("/completion", resp.dump());

    LLMClient client("http://127.0.0.1:" + std::to_string(mock.port), "", "test-model", "openai");
    auto out = client.label_probs("prompt", {" A", " B"}, "cache-key");

    CHECK(approx(out[0].probability, 0.7));
    CHECK(approx(out[1].probability, 0.3));
    return 0;
}

int test_missing_candidate_reads_as_zero_not_a_crash() {
    MockServer mock;
    json resp = {
        {"completion_probabilities", json::array({
            {{"top_logprobs", json::array({
                {{"token", " A"}, {"logprob", lp(0.5)}}
                // " B" never appears in the top_logprobs at all.
            })}}
        })}
    };
    mock.start("/completion", resp.dump());

    LLMClient client("http://127.0.0.1:" + std::to_string(mock.port), "", "test-model", "openai");
    auto out = client.label_probs("prompt", {" A", " B"}, "cache-key");

    CHECK(approx(out[0].probability, 1.0));
    CHECK(approx(out[1].probability, 0.0));
    return 0;
}

int test_no_candidate_present_returns_zeros() {
    MockServer mock;
    mock.start("/completion", R"({"completion_probabilities":[{"top_logprobs":[{"token":"Z","logprob":-0.1}]}]})");

    LLMClient client("http://127.0.0.1:" + std::to_string(mock.port), "", "test-model", "openai");
    auto out = client.label_probs("prompt", {" A", " B"}, "cache-key");

    // No candidate matched — zero, not a division-by-zero garbage value and
    // not silently made up to sum to 1. classify_decision's own averaging
    // step is what turns "no signal" into an honest uniform prior.
    CHECK(approx(out[0].probability, 0.0));
    CHECK(approx(out[1].probability, 0.0));
    return 0;
}

int test_neither_schema_key_present_returns_zeros_not_a_crash() {
    // Not a llama.cpp-family /completion response at all (or n_probs wasn't
    // honoured) — no "completion_probabilities", no "probs". Must degrade to
    // zeros with a logged warning, not throw or read garbage.
    MockServer mock;
    mock.start("/completion", R"({"content":" A"})");

    LLMClient client("http://127.0.0.1:" + std::to_string(mock.port), "", "test-model", "openai");
    auto out = client.label_probs("prompt", {" A", " B"}, "cache-key");

    CHECK(approx(out[0].probability, 0.0));
    CHECK(approx(out[1].probability, 0.0));
    return 0;
}

int test_anthropic_provider_refuses() {
    LLMClient client("https://api.anthropic.com", "", "claude-haiku-4-5-20251001", "anthropic");
    bool threw = false;
    try {
        client.label_probs("prompt", {" A", " B"}, "cache-key");
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);
    return 0;
}

int main() {
    int rc = 0;
    rc |= test_hits_native_completion_endpoint_with_expected_fields();
    rc |= test_normalizes_over_candidate_set_only();
    rc |= test_probs_schema_accepted_as_fallback();
    rc |= test_missing_candidate_reads_as_zero_not_a_crash();
    rc |= test_no_candidate_present_returns_zeros();
    rc |= test_neither_schema_key_present_returns_zeros_not_a_crash();
    rc |= test_anthropic_provider_refuses();
    if (rc == 0) std::cout << "test_label_probs: all tests passed\n";
    return rc;
}
