// =============================================================================
// src/core/llm_client.h — LLM HTTP client (OpenAI-compatible + Anthropic)
// =============================================================================
//
// Ported from AresOS and extended with:
//   - token streaming (stream=true, SSE parsing) via an on_delta callback
//   - an EmbeddingClient for OpenAI-compatible /v1/embeddings endpoints
//
// Supported backends:
//   - llama-server (llama.cpp) at http://localhost:8080
//   - OpenAI / Groq / any server implementing POST /v1/chat/completions
//   - Anthropic (native /v1/messages wire format)
//
// Usage:
//   LLMClient client("http://localhost:8080", "", "llama3.1");
//   auto resp = client.complete(messages, tools);                  // blocking
//   auto resp = client.complete(messages, tools, [](auto& d){...}); // streaming
// =============================================================================

#pragma once
#include <functional>
#include <string>
#include <vector>
#include "json.hpp"

using json = nlohmann::json;

// ── Data structures ───────────────────────────────────────────────────────────

struct ChatMessage {
    std::string role;           // "system" | "user" | "assistant" | "tool"
    std::string content;
    std::string tool_call_id;   // set when role == "tool" (result of a tool call)
    std::string name;           // set when role == "tool" (tool name)
    json        tool_calls;     // set when role == "assistant" and model called tools
                                // (json::array of OpenAI-format tool call objects)
};

struct ToolCall {
    std::string id;             // unique call ID from the model
    std::string name;           // tool name
    json        arguments;      // parsed arguments object
};

struct CompletionResponse {
    std::string           content;      // final text (empty if tool_calls non-empty)
    std::vector<ToolCall> tool_calls;   // requested tool invocations
    int                   prompt_tokens     = 0;
    int                   completion_tokens = 0;
};

// Called with each text fragment as the model generates it. Only text content
// is streamed; tool-call arguments are accumulated internally and returned in
// the CompletionResponse.
using DeltaFn = std::function<void(const std::string& text_delta)>;

// ── LLMClient ─────────────────────────────────────────────────────────────────

class LLMClient {
public:
    // base_url: e.g. "http://localhost:8080" or "https://api.anthropic.com"
    // api_key:  Bearer token for OpenAI; x-api-key value for Anthropic
    // model:    model identifier, e.g. "llama3.1", "claude-haiku-4-5-20251001"
    // provider: "openai" (default) | "anthropic"
    LLMClient(const std::string& base_url,
              const std::string& api_key,
              const std::string& model,
              const std::string& provider = "openai");

    // Send a chat completion request.
    // tools_schema: array of OpenAI-format tool definitions (may be empty).
    // on_delta:     if set, the request streams and text fragments are emitted
    //               through the callback as they arrive.
    // Throws std::runtime_error on HTTP or JSON errors.
    CompletionResponse complete(
        const std::vector<ChatMessage>& messages,
        const json& tools_schema = json::array(),
        const DeltaFn& on_delta = nullptr
    );

    void set_temperature(float t)              { temperature_  = t; }
    void set_max_tokens(int n)                 { max_tokens_   = n; }
    void set_tool_choice(const std::string& s) { tool_choice_  = s; }

    const std::string& model()    const { return model_; }
    const std::string& provider() const { return provider_; }

private:
    std::string host_;
    int         port_;
    bool        https_;
    std::string base_path_;
    std::string api_key_;
    std::string model_;
    std::string provider_;  // "openai" | "anthropic"
    float       temperature_ = 0.2f;
    int         max_tokens_  = 2048;
    std::string tool_choice_ = "auto"; // "auto" | "required" | "none"

    void parse_url(const std::string& url);

    // OpenAI path
    json build_messages_json(const std::vector<ChatMessage>& messages);
    json build_openai_body(const std::vector<ChatMessage>& messages,
                           const json& tools_schema, bool stream);
    CompletionResponse complete_openai(const std::vector<ChatMessage>& messages,
                                       const json& tools_schema);
    CompletionResponse stream_openai(const std::vector<ChatMessage>& messages,
                                     const json& tools_schema,
                                     const DeltaFn& on_delta);

    // Anthropic path
    json build_anthropic_body(const std::vector<ChatMessage>& messages,
                              const json& tools_schema, bool stream);
    CompletionResponse complete_anthropic(const std::vector<ChatMessage>& messages,
                                          const json& tools_schema);
    CompletionResponse stream_anthropic(const std::vector<ChatMessage>& messages,
                                        const json& tools_schema,
                                        const DeltaFn& on_delta);
    std::pair<std::string, json> build_anthropic_messages(
        const std::vector<ChatMessage>& messages);
    static json openai_tools_to_anthropic(const json& openai_tools);

    // Re-parse tool calls that some local models emit as plain text content.
    static void recover_tool_calls_from_content(CompletionResponse& out);
};

// ── EmbeddingClient ───────────────────────────────────────────────────────────
// POSTs to any OpenAI-compatible /v1/embeddings endpoint (llama-server with
// --embedding, OpenAI, etc.). Used by the memory engine for semantic search.

class EmbeddingClient {
public:
    EmbeddingClient(const std::string& base_url,
                    const std::string& api_key,
                    const std::string& model);
    virtual ~EmbeddingClient() = default;

    // Returns the embedding vector for `text`.
    // Throws std::runtime_error on HTTP or JSON errors.
    virtual std::vector<float> embed(const std::string& text);

private:
    std::string host_;
    int         port_    = 80;
    bool        https_   = false;
    std::string base_path_;
    std::string api_key_;
    std::string model_;
};
