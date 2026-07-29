// =============================================================================
// src/core/llm_client.cpp — LLM HTTP client (implementation)
// =============================================================================
//
// Non-streaming paths are ported from AresOS. Streaming paths parse the
// server-sent-events wire format of each provider:
//
//   OpenAI:    data: {"choices":[{"delta":{"content":"..","tool_calls":[..]}}]}
//   Anthropic: data: {"type":"content_block_delta","delta":{"type":"text_delta",..}}
//
// Text deltas are forwarded to the caller; tool-call fragments are accumulated
// and returned parsed in the CompletionResponse, so the agent loop is identical
// for streamed and non-streamed calls.
//
// CPPHTTPLIB_OPENSSL_SUPPORT is defined globally via CMake (PUBLIC on the mcp
// target) so ALL translation units see the same httplib::Client layout.
// =============================================================================

#include "llm_client.h"
#include "text_utils.h"
#include "httplib.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

// ── URL parsing ───────────────────────────────────────────────────────────────

static void parse_http_url(const std::string& url, bool& https, std::string& host,
                           int& port, std::string& base_path) {
    std::regex re(R"(^(https?)://([^/:]+)(?::(\d+))?(/.*)?)");
    std::smatch m;
    if (!std::regex_match(url, m, re))
        throw std::runtime_error("Invalid URL: " + url);

    https     = (m[1].str() == "https");
    host      = m[2].str();
    port      = m[3].matched ? std::stoi(m[3].str()) : (https ? 443 : 80);
    base_path = m[4].matched ? m[4].str() : "";
    if (!base_path.empty() && base_path.back() == '/')
        base_path.pop_back();
}

void LLMClient::parse_url(const std::string& url) {
    parse_http_url(url, https_, host_, port_, base_path_);
}

// ── Constructor ───────────────────────────────────────────────────────────────

LLMClient::LLMClient(const std::string& base_url,
                     const std::string& api_key,
                     const std::string& model,
                     const std::string& provider)
    : api_key_(api_key), model_(model), provider_(provider)
{
    parse_url(base_url);
}

// ── Message serialisation ─────────────────────────────────────────────────────

// Qwen3's chat template enforces strict user→assistant alternation and has no
// "tool" role — tool results must go back as user messages in <tool_response>
// tags. llama.cpp passes messages through the template verbatim, so convert
// before the HTTP call for Qwen models.
static bool is_qwen_model(const std::string& model) {
    for (size_t i = 0; i + 3 < model.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(model[i]))   == 'q' &&
            std::tolower(static_cast<unsigned char>(model[i+1])) == 'w' &&
            std::tolower(static_cast<unsigned char>(model[i+2])) == 'e' &&
            std::tolower(static_cast<unsigned char>(model[i+3])) == 'n')
            return true;
    }
    return false;
}

// OpenAI multimodal content: a plain string when there are no images (the
// common case, byte-identical to the old wire format), or an array mixing
// a text part with one image_url part per attachment.
static json openai_content_value(const ChatMessage& msg) {
    if (msg.images.empty()) return msg.content;

    json parts = json::array();
    if (!msg.content.empty())
        parts.push_back({{"type", "text"}, {"text", msg.content}});
    for (const auto& img : msg.images) {
        parts.push_back({
            {"type", "image_url"},
            {"image_url", {{"url", "data:" + img.mime_type + ";base64," + img.base64_data}}}
        });
    }
    return parts;
}

json LLMClient::build_messages_json(const std::vector<ChatMessage>& messages) {
    json arr = json::array();
    const bool qwen = is_qwen_model(model_);

    for (size_t i = 0; i < messages.size(); ) {
        const auto& msg = messages[i];

        if (qwen && msg.role == "tool") {
            std::string combined;
            std::vector<ImageAttachment> combined_images;
            while (i < messages.size() && messages[i].role == "tool") {
                combined += "<tool_response>\n" + messages[i].content + "\n</tool_response>\n";
                combined_images.insert(combined_images.end(),
                                       messages[i].images.begin(), messages[i].images.end());
                ++i;
            }
            if (combined_images.empty()) {
                arr.push_back({{"role", "user"}, {"content", combined}});
            } else {
                ChatMessage img_msg;
                img_msg.role    = "user";
                img_msg.content = combined;
                img_msg.images  = std::move(combined_images);
                arr.push_back({{"role", "user"}, {"content", openai_content_value(img_msg)}});
            }
        }
        else if (msg.role == "tool") {
            arr.push_back({
                {"role",         "tool"},
                {"content",      msg.content},
                {"tool_call_id", msg.tool_call_id},
                {"name",         msg.name}
            });
            ++i;
            // OpenAI tool-role messages are text-only — a tool that returned
            // images (e.g. rendered PDF pages) gets a synthetic follow-up
            // user turn instead, the same mechanism already used for
            // directly-attached images.
            if (!msg.images.empty()) {
                ChatMessage img_msg;
                img_msg.role    = "user";
                img_msg.content = "[Image(s) returned by the " + msg.name + " tool call above]";
                img_msg.images  = msg.images;
                arr.push_back({{"role", "user"}, {"content", openai_content_value(img_msg)}});
            }
        }
        else if (msg.role == "assistant"
                 && !msg.tool_calls.is_null() && !msg.tool_calls.empty()) {
            arr.push_back({
                {"role",       "assistant"},
                {"content",    msg.content.empty() ? json(nullptr) : json(msg.content)},
                {"tool_calls", msg.tool_calls}
            });
            ++i;
        }
        else {
            arr.push_back({{"role", msg.role}, {"content", openai_content_value(msg)}});
            ++i;
        }
    }
    return arr;
}

// ── HTTP helpers ──────────────────────────────────────────────────────────────

// Slow local models regularly exceed 2 min on long prompts with large tool
// schemas; cloud APIs never hit this.
static int llm_read_timeout() {
    static int cached = [] {
        if (const char* v = std::getenv("FUNES_LLM_TIMEOUT_SECONDS"))
            if (int t = std::atoi(v); t > 0)
                return t;
        return 600;
    }();
    return cached;
}

// Transient connection failures (backend restarting, brief network blip)
// should not kill a whole agent turn. Connection-level errors are retried
// with a short backoff; HTTP-level errors (4xx/5xx) are never retried here.
static bool is_transient(httplib::Error err) {
    return err == httplib::Error::Connection ||
           err == httplib::Error::ConnectionTimeout ||
           err == httplib::Error::ProxyConnection;
}

static constexpr int MAX_CONNECT_RETRIES = 2;

static httplib::Result do_post(bool https, const std::string& host, int port,
                               const std::string& path,
                               const httplib::Headers& headers,
                               const std::string& body) {
    const int timeout = llm_read_timeout();
    httplib::Result result;
    for (int attempt = 0; ; ++attempt) {
        if (https) {
            httplib::SSLClient cli(host, port);
            cli.set_connection_timeout(10);
            cli.set_read_timeout(timeout);
            cli.enable_server_certificate_verification(true);
            result = cli.Post(path, headers, body, "application/json");
        } else {
            httplib::Client cli(host, port);
            cli.set_connection_timeout(10);
            cli.set_read_timeout(timeout);
            result = cli.Post(path, headers, body, "application/json");
        }
        if (result || !is_transient(result.error()) || attempt >= MAX_CONNECT_RETRIES)
            return result;
        std::cerr << "[llm_client] connection to " << host << ":" << port
                  << " failed (attempt " << attempt + 1 << "), retrying…\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(400 * (attempt + 1)));
    }
}

// Streaming POST: feeds response bytes to `receiver` as they arrive.
// Retries transient connection failures only while nothing has been received
// yet — once bytes have flowed, a retry would duplicate streamed output.
static httplib::Result do_post_stream(bool https, const std::string& host, int port,
                                      const std::string& path,
                                      const httplib::Headers& headers,
                                      const std::string& body,
                                      const httplib::ContentReceiver& receiver) {
    const int timeout = llm_read_timeout();
    httplib::Result result;
    for (int attempt = 0; ; ++attempt) {
        bool received_any = false;
        httplib::Request req;
        req.method  = "POST";
        req.path    = path;
        req.headers = headers;
        req.body    = body;
        req.set_header("Content-Type", "application/json");
        req.content_receiver = [receiver, &received_any](const char* data, size_t len,
                                                         uint64_t, uint64_t) {
            received_any = true;
            return receiver(data, len);
        };
        if (https) {
            httplib::SSLClient cli(host, port);
            cli.set_connection_timeout(10);
            cli.set_read_timeout(timeout);
            cli.enable_server_certificate_verification(true);
            result = cli.send(req);
        } else {
            httplib::Client cli(host, port);
            cli.set_connection_timeout(10);
            cli.set_read_timeout(timeout);
            result = cli.send(req);
        }
        if (result || received_any || !is_transient(result.error())
            || attempt >= MAX_CONNECT_RETRIES)
            return result;
        std::cerr << "[llm_client] connection to " << host << ":" << port
                  << " failed (attempt " << attempt + 1 << "), retrying…\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(400 * (attempt + 1)));
    }
}

// Incremental SSE parser: buffers bytes, emits the payload of each
// "data: {...}" line to `on_data`.
struct SseLineParser {
    std::string buffer;
    std::function<void(const std::string&)> on_data;

    void feed(const char* data, size_t len) {
        buffer.append(data, len);
        size_t pos;
        while ((pos = buffer.find('\n')) != std::string::npos) {
            std::string line = buffer.substr(0, pos);
            buffer.erase(0, pos + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.rfind("data:", 0) == 0) {
                std::string payload = line.substr(5);
                if (!payload.empty() && payload.front() == ' ') payload.erase(0, 1);
                if (!payload.empty()) on_data(payload);
            }
        }
    }
};

// ── Tool-call recovery (local model quirk) ────────────────────────────────────
// Qwen3-class models occasionally put the tool call JSON in the content field
// rather than in tool_calls. Detect and re-parse so the agent loop isn't bypassed.

// Some local models fall back to an XML-ish syntax instead of JSON when the
// served chat template doesn't match what they were fine-tuned on:
//   <tool_call>
//   <function=NAME>
//   <parameter=PARAM>VALUE</parameter>
//   </function>
//   </tool_call>
// Parse that shape too, so it doesn't leak into the final answer as raw text.
// Extracts every <prefix NAME>BODY<close> block via plain substring scanning.
// A regex equivalent (`<tag=(name)>([\s\S]*?)</tag>`) is what this replaced —
// libstdc++'s std::regex backtracks recursively for `[\s\S]*?`, and on a large
// body (e.g. a full HTML newsletter emitted as a tool argument by a local
// model that fell back to this XML-ish syntax) that recursion is deep enough
// to blow the stack and crash the whole server. Manual scanning is O(n) with
// no recursion.
static std::vector<std::pair<std::string, std::string>> extract_xml_blocks(
        const std::string& text, const std::string& open_prefix, const std::string& close_tag) {
    std::vector<std::pair<std::string, std::string>> blocks;
    size_t pos = 0;
    while (true) {
        size_t open = text.find(open_prefix, pos);
        if (open == std::string::npos) break;
        size_t name_start = open + open_prefix.size();
        size_t name_end = text.find('>', name_start);
        if (name_end == std::string::npos) break;
        std::string name = text.substr(name_start, name_end - name_start);
        size_t body_start = name_end + 1;
        size_t close = text.find(close_tag, body_start);
        if (close == std::string::npos) break;
        if (!name.empty() && std::all_of(name.begin(), name.end(), [](unsigned char c) {
                return std::isalnum(c) || c == '_';
            })) {
            blocks.emplace_back(std::move(name), text.substr(body_start, close - body_start));
        }
        pos = close + close_tag.size();
    }
    return blocks;
}

static bool recover_xml_tool_calls(CompletionResponse& out) {
    auto trim = [](std::string s) {
        size_t b = s.find_first_not_of(" \t\r\n");
        if (b == std::string::npos) return std::string();
        size_t e = s.find_last_not_of(" \t\r\n");
        return s.substr(b, e - b + 1);
    };

    bool recovered = false;
    for (auto& [name, body] : extract_xml_blocks(out.content, "<function=", "</function>")) {
        ToolCall call;
        call.id   = "call_" + std::to_string(out.tool_calls.size());
        call.name = name;

        json args = json::object();
        for (auto& [pname, pval] : extract_xml_blocks(body, "<parameter=", "</parameter>"))
            args[pname] = trim(pval);
        call.arguments = std::move(args);
        out.tool_calls.push_back(std::move(call));
        recovered = true;
    }
    return recovered;
}

void LLMClient::recover_tool_calls_from_content(CompletionResponse& out) {
    if (!out.tool_calls.empty() || out.content.empty()) return;

    auto parse_tc = [&](const json& j) -> bool {
        if (!j.is_object()) return false;
        ToolCall call;

        auto parse_args = [](const json& a) -> json {
            if (a.is_string()) {
                try { return json::parse(a.get<std::string>()); }
                catch (...) { return json::object(); }
            }
            return a.is_object() ? a : json::object();
        };

        if (j.value("type", "") == "function" && j.contains("function")) {
            const auto& fn = j["function"];
            if (!fn.contains("name")) return false;
            call.id        = j.value("id", "call_0");
            call.name      = fn["name"].get<std::string>();
            call.arguments = parse_args(fn.contains("arguments") ? fn["arguments"] : json::object());
            out.tool_calls.push_back(std::move(call));
            return true;
        }
        if (j.contains("name") && j["name"].is_string() && j.contains("arguments")) {
            call.id        = j.value("id", "call_0");
            call.name      = j["name"].get<std::string>();
            call.arguments = parse_args(j["arguments"]);
            out.tool_calls.push_back(std::move(call));
            return true;
        }
        return false;
    };

    try {
        auto j = json::parse(out.content);
        bool recovered = false;
        if (j.is_array()) {
            for (const auto& item : j) recovered |= parse_tc(item);
        } else {
            recovered = parse_tc(j);
        }
        if (recovered) {
            std::cerr << "[llm_client] WARNING: tool call recovered from content field\n";
            out.content.clear();
        }
    } catch (...) {
        // Not JSON — try the XML-ish <function=..><parameter=..> fallback
        // format some local models emit instead.
        if (recover_xml_tool_calls(out)) {
            std::cerr << "[llm_client] WARNING: tool call recovered from XML-style content\n";
            out.content.clear();
        }
    }
}

// ── OpenAI path ───────────────────────────────────────────────────────────────

json LLMClient::build_openai_body(const std::vector<ChatMessage>& messages,
                                  const json& tools_schema, bool stream) {
    json body = {
        {"model",       model_},
        {"messages",    build_messages_json(messages)},
        {"temperature", temperature_},
        {"max_tokens",  max_tokens_}
    };
    if (!tools_schema.empty()) {
        body["tools"]       = tools_schema;
        body["tool_choice"] = tool_choice_;
    }
    if (stream)
        body["stream"] = true;
    return body;
}

static httplib::Headers openai_headers(const std::string& api_key) {
    httplib::Headers headers = {
        {"Accept", "application/json"}
    };
    if (!api_key.empty())
        headers.emplace("Authorization", "Bearer " + api_key);
    return headers;
}

static void parse_openai_tool_call(const json& tc, std::vector<ToolCall>& out) {
    ToolCall call;
    call.id   = tc.value("id", "");
    call.name = tc["function"]["name"].get<std::string>();
    const auto& args_val = tc["function"]["arguments"];
    if (args_val.is_string()) {
        try { call.arguments = json::parse(args_val.get<std::string>()); }
        catch (...) { call.arguments = json::object(); }
    } else if (args_val.is_object()) {
        call.arguments = args_val;
    } else {
        call.arguments = json::object();
    }
    out.push_back(std::move(call));
}

CompletionResponse LLMClient::complete_openai(
    const std::vector<ChatMessage>& messages,
    const json& tools_schema)
{
    json body = build_openai_body(messages, tools_schema, /*stream=*/false);
    const std::string path = base_path_ + "/v1/chat/completions";
    auto result = do_post(https_, host_, port_, path,
                          openai_headers(api_key_), funes::dump_safe(body));

    if (!result)
        throw std::runtime_error("HTTP request failed: " +
            std::string(httplib::to_string(result.error())));
    if (result->status != 200)
        throw std::runtime_error("LLM API error " +
            std::to_string(result->status) + ": " + result->body);

    json resp;
    try { resp = json::parse(result->body); }
    catch (const json::exception& e) {
        throw std::runtime_error("Invalid JSON from LLM: " + std::string(e.what()));
    }

    CompletionResponse out;
    if (resp.contains("usage") && resp["usage"].is_object()) {
        out.prompt_tokens     = resp["usage"].value("prompt_tokens",     0);
        out.completion_tokens = resp["usage"].value("completion_tokens", 0);
    }
    if (!resp.contains("choices") || resp["choices"].empty())
        throw std::runtime_error("LLM response has no choices");

    const json& msg = resp["choices"][0]["message"];
    if (msg.contains("tool_calls") && !msg["tool_calls"].is_null()) {
        for (const auto& tc : msg["tool_calls"])
            parse_openai_tool_call(tc, out.tool_calls);
    } else if (msg.contains("content") && msg["content"].is_string()) {
        out.content = msg["content"].get<std::string>();
    }

    recover_tool_calls_from_content(out);
    return out;
}

CompletionResponse LLMClient::stream_openai(
    const std::vector<ChatMessage>& messages,
    const json& tools_schema,
    const DeltaFn& on_delta)
{
    json body = build_openai_body(messages, tools_schema, /*stream=*/true);
    const std::string path = base_path_ + "/v1/chat/completions";

    CompletionResponse out;
    std::string error_body;

    // Accumulators for tool-call fragments, keyed by stream index.
    struct PartialCall { std::string id, name, arguments; };
    std::vector<PartialCall> partials;

    SseLineParser sse;
    sse.on_data = [&](const std::string& payload) {
        if (payload == "[DONE]") return;
        json chunk;
        try { chunk = json::parse(payload); } catch (...) { return; }

        if (chunk.contains("usage") && chunk["usage"].is_object()) {
            out.prompt_tokens     = chunk["usage"].value("prompt_tokens",     0);
            out.completion_tokens = chunk["usage"].value("completion_tokens", 0);
        }
        if (!chunk.contains("choices") || chunk["choices"].empty()) return;
        const json& delta = chunk["choices"][0].value("delta", json::object());

        if (delta.contains("content") && delta["content"].is_string()) {
            const std::string text = delta["content"].get<std::string>();
            if (!text.empty()) {
                out.content += text;
                if (on_delta) on_delta(text);
            }
        }
        if (delta.contains("tool_calls") && delta["tool_calls"].is_array()) {
            for (const auto& tc : delta["tool_calls"]) {
                size_t idx = tc.value("index", partials.size());
                if (idx >= partials.size()) partials.resize(idx + 1);
                auto& p = partials[idx];
                if (tc.contains("id") && tc["id"].is_string())
                    p.id = tc["id"].get<std::string>();
                if (tc.contains("function")) {
                    const auto& fn = tc["function"];
                    if (fn.contains("name") && fn["name"].is_string())
                        p.name += fn["name"].get<std::string>();
                    if (fn.contains("arguments") && fn["arguments"].is_string())
                        p.arguments += fn["arguments"].get<std::string>();
                }
            }
        }
    };

    auto result = do_post_stream(https_, host_, port_, path,
                                 openai_headers(api_key_), funes::dump_safe(body),
        [&](const char* data, size_t len) {
            sse.feed(data, len);
            error_body.append(data, len);  // kept in case of a non-200 status
            return true;
        });

    if (!result)
        throw std::runtime_error("HTTP request failed: " +
            std::string(httplib::to_string(result.error())));
    if (result->status != 200)
        throw std::runtime_error("LLM API error " +
            std::to_string(result->status) + ": " + error_body);

    for (const auto& p : partials) {
        if (p.name.empty()) continue;
        ToolCall call;
        call.id   = p.id.empty() ? ("call_" + std::to_string(out.tool_calls.size())) : p.id;
        call.name = p.name;
        try { call.arguments = p.arguments.empty() ? json::object() : json::parse(p.arguments); }
        catch (...) { call.arguments = json::object(); }
        out.tool_calls.push_back(std::move(call));
    }
    if (!out.tool_calls.empty())
        out.content.clear();

    recover_tool_calls_from_content(out);
    return out;
}

// ── Anthropic path ────────────────────────────────────────────────────────────

json LLMClient::openai_tools_to_anthropic(const json& openai_tools) {
    json result = json::array();
    for (const auto& t : openai_tools) {
        const auto& fn = t["function"];
        result.push_back({
            {"name",         fn["name"]},
            {"description",  fn.value("description", "")},
            {"input_schema", fn.contains("parameters")
                                ? fn["parameters"]
                                : json{{"type","object"},{"properties",json::object()}}}
        });
    }
    return result;
}

// Anthropic multimodal content: a plain string with no images (unchanged
// wire format), or an array mixing a text block with one image block per
// attachment (source.type=base64, same bytes web_fetch/upload already have).
static json anthropic_content_value(const ChatMessage& msg) {
    if (msg.images.empty()) return msg.content;

    json parts = json::array();
    if (!msg.content.empty())
        parts.push_back({{"type", "text"}, {"text", msg.content}});
    for (const auto& img : msg.images) {
        parts.push_back({
            {"type", "image"},
            {"source", {
                {"type",       "base64"},
                {"media_type", img.mime_type},
                {"data",       img.base64_data}
            }}
        });
    }
    return parts;
}

std::pair<std::string, json> LLMClient::build_anthropic_messages(
    const std::vector<ChatMessage>& messages)
{
    std::string system_text;
    json arr = json::array();

    for (size_t i = 0; i < messages.size(); ) {
        const auto& msg = messages[i];

        if (msg.role == "system") {
            system_text = msg.content;
            ++i;
        } else if (msg.role == "user") {
            arr.push_back({{"role", "user"}, {"content", anthropic_content_value(msg)}});
            ++i;
        } else if (msg.role == "assistant") {
            json content = json::array();
            if (!msg.content.empty())
                content.push_back({{"type", "text"}, {"text", msg.content}});
            if (!msg.tool_calls.is_null() && !msg.tool_calls.empty()) {
                for (const auto& tc : msg.tool_calls) {
                    json input;
                    try { input = json::parse(tc["function"]["arguments"].get<std::string>()); }
                    catch (...) { input = json::object(); }
                    content.push_back({
                        {"type",  "tool_use"},
                        {"id",    tc["id"]},
                        {"name",  tc["function"]["name"]},
                        {"input", input}
                    });
                }
            }
            arr.push_back({{"role", "assistant"}, {"content", content}});
            ++i;
        } else if (msg.role == "tool") {
            json tool_results = json::array();
            while (i < messages.size() && messages[i].role == "tool") {
                // Anthropic tool_result content blocks accept the same
                // text/image mix as a user turn — reuse that helper so a
                // tool that returned images (e.g. rendered PDF pages) is
                // visible to the model directly in the tool result.
                tool_results.push_back({
                    {"type",        "tool_result"},
                    {"tool_use_id", messages[i].tool_call_id},
                    {"content",     anthropic_content_value(messages[i])}
                });
                ++i;
            }
            arr.push_back({{"role", "user"}, {"content", tool_results}});
        } else {
            ++i;
        }
    }
    return {system_text, arr};
}

json LLMClient::build_anthropic_body(const std::vector<ChatMessage>& messages,
                                     const json& tools_schema, bool stream) {
    auto [system_text, msgs] = build_anthropic_messages(messages);

    json body = {
        {"model",       model_},
        {"max_tokens",  max_tokens_},
        {"temperature", temperature_},
        {"messages",    msgs}
    };
    if (!system_text.empty())
        body["system"] = system_text;
    if (!tools_schema.empty() && tool_choice_ != "none") {
        body["tools"] = openai_tools_to_anthropic(tools_schema);
        body["tool_choice"] = {{"type", (tool_choice_ == "required") ? "any" : "auto"}};
    }
    if (stream)
        body["stream"] = true;
    return body;
}

static httplib::Headers anthropic_headers(const std::string& api_key) {
    return {
        {"Accept",            "application/json"},
        {"x-api-key",         api_key},
        {"anthropic-version", "2023-06-01"}
    };
}

CompletionResponse LLMClient::complete_anthropic(
    const std::vector<ChatMessage>& messages,
    const json& tools_schema)
{
    json body = build_anthropic_body(messages, tools_schema, /*stream=*/false);
    const std::string path = base_path_ + "/v1/messages";
    auto result = do_post(https_, host_, port_, path,
                          anthropic_headers(api_key_), funes::dump_safe(body));

    if (!result)
        throw std::runtime_error("HTTP request failed: " +
            std::string(httplib::to_string(result.error())));
    if (result->status != 200)
        throw std::runtime_error("Anthropic API error " +
            std::to_string(result->status) + ": " + result->body);

    json resp;
    try { resp = json::parse(result->body); }
    catch (const json::exception& e) {
        throw std::runtime_error("Invalid JSON from Anthropic: " + std::string(e.what()));
    }

    CompletionResponse out;
    if (resp.contains("usage")) {
        out.prompt_tokens     = resp["usage"].value("input_tokens",  0);
        out.completion_tokens = resp["usage"].value("output_tokens", 0);
    }
    if (!resp.contains("content") || !resp["content"].is_array())
        throw std::runtime_error("Anthropic response missing content array");

    for (const auto& block : resp["content"]) {
        const std::string type = block.value("type", "");
        if (type == "text") {
            out.content += block.value("text", "");
        } else if (type == "tool_use") {
            ToolCall tc;
            tc.id        = block.value("id",   "");
            tc.name      = block.value("name", "");
            tc.arguments = block.contains("input") ? block["input"] : json::object();
            out.tool_calls.push_back(std::move(tc));
        }
    }
    return out;
}

CompletionResponse LLMClient::stream_anthropic(
    const std::vector<ChatMessage>& messages,
    const json& tools_schema,
    const DeltaFn& on_delta)
{
    json body = build_anthropic_body(messages, tools_schema, /*stream=*/true);
    const std::string path = base_path_ + "/v1/messages";

    CompletionResponse out;
    std::string error_body;

    // Content blocks arrive indexed; tool_use blocks accumulate partial JSON.
    struct Block { std::string type, id, name, partial_json; };
    std::vector<Block> blocks;

    SseLineParser sse;
    sse.on_data = [&](const std::string& payload) {
        json ev;
        try { ev = json::parse(payload); } catch (...) { return; }
        const std::string type = ev.value("type", "");

        if (type == "content_block_start") {
            size_t idx = ev.value("index", blocks.size());
            if (idx >= blocks.size()) blocks.resize(idx + 1);
            const json& cb = ev.value("content_block", json::object());
            blocks[idx].type = cb.value("type", "");
            blocks[idx].id   = cb.value("id",   "");
            blocks[idx].name = cb.value("name", "");
        }
        else if (type == "content_block_delta") {
            size_t idx = ev.value("index", size_t{0});
            if (idx >= blocks.size()) blocks.resize(idx + 1);
            const json& delta = ev.value("delta", json::object());
            const std::string dtype = delta.value("type", "");
            if (dtype == "text_delta") {
                const std::string text = delta.value("text", "");
                if (!text.empty()) {
                    out.content += text;
                    if (on_delta) on_delta(text);
                }
            } else if (dtype == "input_json_delta") {
                blocks[idx].partial_json += delta.value("partial_json", "");
            }
        }
        else if (type == "message_start") {
            const json& usage = ev.value("message", json::object())
                                  .value("usage", json::object());
            out.prompt_tokens = usage.value("input_tokens", 0);
        }
        else if (type == "message_delta") {
            const json& usage = ev.value("usage", json::object());
            out.completion_tokens = usage.value("output_tokens", 0);
        }
        else if (type == "error") {
            error_body = payload;
        }
    };

    auto result = do_post_stream(https_, host_, port_, path,
                                 anthropic_headers(api_key_), funes::dump_safe(body),
        [&](const char* data, size_t len) {
            sse.feed(data, len);
            error_body.append(data, len);
            return true;
        });

    if (!result)
        throw std::runtime_error("HTTP request failed: " +
            std::string(httplib::to_string(result.error())));
    if (result->status != 200)
        throw std::runtime_error("Anthropic API error " +
            std::to_string(result->status) + ": " + error_body);

    for (const auto& b : blocks) {
        if (b.type != "tool_use") continue;
        ToolCall tc;
        tc.id   = b.id;
        tc.name = b.name;
        try { tc.arguments = b.partial_json.empty() ? json::object() : json::parse(b.partial_json); }
        catch (...) { tc.arguments = json::object(); }
        out.tool_calls.push_back(std::move(tc));
    }
    if (!out.tool_calls.empty())
        out.content.clear();

    return out;
}

// ── Dispatch ──────────────────────────────────────────────────────────────────

CompletionResponse LLMClient::complete(
    const std::vector<ChatMessage>& messages,
    const json& tools_schema,
    const DeltaFn& on_delta)
{
    if (provider_ == "anthropic") {
        return on_delta ? stream_anthropic(messages, tools_schema, on_delta)
                        : complete_anthropic(messages, tools_schema);
    }
    return on_delta ? stream_openai(messages, tools_schema, on_delta)
                    : complete_openai(messages, tools_schema);
}

// ── model discovery ───────────────────────────────────────────────────────────

std::string fetch_default_model(const std::string& base_url, const std::string& api_key) {
    try {
        bool https; std::string host; int port; std::string base_path;
        parse_http_url(base_url, https, host, port, base_path);

        httplib::Headers headers = {{"Accept", "application/json"}};
        if (!api_key.empty())
            headers.emplace("Authorization", "Bearer " + api_key);

        const std::string path = base_path + "/v1/models";
        httplib::Result result;
        if (https) {
            httplib::SSLClient cli(host, port);
            cli.set_connection_timeout(5);
            cli.set_read_timeout(10);
            cli.enable_server_certificate_verification(true);
            result = cli.Get(path.c_str(), headers);
        } else {
            httplib::Client cli(host, port);
            cli.set_connection_timeout(5);
            cli.set_read_timeout(10);
            result = cli.Get(path.c_str(), headers);
        }
        if (!result || result->status != 200) return "";

        json resp = json::parse(result->body);
        if (resp.contains("data") && resp["data"].is_array() && !resp["data"].empty())
            return resp["data"][0].value("id", "");
    } catch (...) {
        // Discovery is best-effort; the caller keeps its configured name.
    }
    return "";
}

// ── EmbeddingClient ───────────────────────────────────────────────────────────

EmbeddingClient::EmbeddingClient(const std::string& base_url,
                                 const std::string& api_key,
                                 const std::string& model)
    : api_key_(api_key), model_(model)
{
    parse_http_url(base_url, https_, host_, port_, base_path_);
}

std::vector<float> EmbeddingClient::embed(const std::string& text) {
    json body = {
        {"model", model_},
        {"input", text}
    };

    httplib::Headers headers = {{"Accept", "application/json"}};
    if (!api_key_.empty())
        headers.emplace("Authorization", "Bearer " + api_key_);

    const std::string path = base_path_ + "/v1/embeddings";
    auto result = do_post(https_, host_, port_, path, headers, funes::dump_safe(body));

    if (!result)
        throw std::runtime_error("Embedding request failed: " +
            std::string(httplib::to_string(result.error())));
    if (result->status != 200)
        throw std::runtime_error("Embedding API error " +
            std::to_string(result->status) + ": " + result->body);

    json resp;
    try { resp = json::parse(result->body); }
    catch (const json::exception& e) {
        throw std::runtime_error("Invalid JSON from embedding API: " + std::string(e.what()));
    }

    if (!resp.contains("data") || !resp["data"].is_array() || resp["data"].empty()
        || !resp["data"][0].contains("embedding"))
        throw std::runtime_error("Embedding response missing data[0].embedding");

    std::vector<float> vec;
    for (const auto& v : resp["data"][0]["embedding"])
        vec.push_back(v.get<float>());
    if (vec.empty())
        throw std::runtime_error("Embedding response contained an empty vector");
    return vec;
}
