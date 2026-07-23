// =============================================================================
// tests/test_llm_client_images.cpp — multipart image content in chat requests
// =============================================================================
// LLMClient's message-building is private (it's an implementation detail of
// complete()), so this drives it the same way a real request would: spin up
// a local mock server, capture the request body, and inspect the wire
// format complete() actually sent. Covers both providers and confirms the
// no-image case is untouched (still a plain string, not a 1-element array).

#include "httplib.h"
#include "llm_client.h"
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

// Starts a mock server that records the last request body it received on
// `path` and replies with `response_body`. Caller must stop()+join().
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

ChatMessage make_user_message_with_image() {
    ChatMessage m;
    m.role = "user";
    m.content = "what is this?";
    m.images.push_back({"image/png", "QUJD"});  // base64 for "ABC"
    return m;
}

} // namespace

int test_openai_image_content() {
    MockServer mock;
    mock.start("/v1/chat/completions", R"({"choices":[{"message":{"content":"a picture"}}]})");

    LLMClient client("http://127.0.0.1:" + std::to_string(mock.port), "", "test-model", "openai");
    auto resp = client.complete({make_user_message_with_image()});
    CHECK(resp.content == "a picture");

    json body = json::parse(mock.last_body);
    const json& content = body["messages"][0]["content"];
    CHECK(content.is_array());
    CHECK(content.size() == 2);
    CHECK(content[0]["type"] == "text");
    CHECK(content[0]["text"] == "what is this?");
    CHECK(content[1]["type"] == "image_url");
    CHECK(content[1]["image_url"]["url"] == "data:image/png;base64,QUJD");
    return 0;
}

int test_openai_no_image_unchanged() {
    MockServer mock;
    mock.start("/v1/chat/completions", R"({"choices":[{"message":{"content":"ok"}}]})");

    LLMClient client("http://127.0.0.1:" + std::to_string(mock.port), "", "test-model", "openai");
    ChatMessage m; m.role = "user"; m.content = "hello";
    auto resp = client.complete({m});
    CHECK(resp.content == "ok");

    json body = json::parse(mock.last_body);
    // No images: content must stay a plain string (byte-identical to the
    // pre-image-support wire format), not a 1-element array.
    CHECK(body["messages"][0]["content"].is_string());
    CHECK(body["messages"][0]["content"] == "hello");
    return 0;
}

int test_anthropic_image_content() {
    MockServer mock;
    mock.start("/v1/messages", R"({"content":[{"type":"text","text":"a picture"}]})");

    LLMClient client("http://127.0.0.1:" + std::to_string(mock.port), "", "test-model", "anthropic");
    auto resp = client.complete({make_user_message_with_image()});
    CHECK(resp.content == "a picture");

    json body = json::parse(mock.last_body);
    const json& content = body["messages"][0]["content"];
    CHECK(content.is_array());
    CHECK(content.size() == 2);
    CHECK(content[0]["type"] == "text");
    CHECK(content[1]["type"] == "image");
    CHECK(content[1]["source"]["type"] == "base64");
    CHECK(content[1]["source"]["media_type"] == "image/png");
    CHECK(content[1]["source"]["data"] == "QUJD");
    return 0;
}

int main() {
    int rc = 0;
    rc |= test_openai_image_content();
    rc |= test_openai_no_image_unchanged();
    rc |= test_anthropic_image_content();
    if (rc == 0) std::cout << "test_llm_client_images: all tests passed\n";
    return rc;
}
