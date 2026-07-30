// =============================================================================
// src/core/agent.cpp — FunesAgent implementation
// =============================================================================

#include "agent.h"
#include "completion_contract.h"
#include "text_utils.h"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <map>
#include <regex>
#include <set>
#include <sstream>

// ── construction ──────────────────────────────────────────────────────────────

FunesAgent::FunesAgent(const AgentConfig& cfg, ToolRegistry& tools,
                       MemoryStore& memory, const AgentDefaults& defaults)
    : cfg_(cfg)
    , tools_(tools)
    , memory_(memory)
    , defaults_(defaults)
    , llm_(cfg.llm_url.empty()      ? defaults.llm_url      : cfg.llm_url,
           cfg.llm_api_key.empty()  ? defaults.llm_api_key  : cfg.llm_api_key,
           (cfg.model.empty() || cfg.model == "default")
               ? (defaults.llm_model.empty() ? "default" : defaults.llm_model)
               : cfg.model,
           cfg.llm_provider.empty() ? defaults.llm_provider : cfg.llm_provider)
{
    llm_.set_max_tokens(cfg_.context_limit / 4);
    llm_.set_tool_choice(cfg_.tool_choice);

    tools_schema_ = tools_.openai_schema(cfg_.tools);
    connect_mcp_servers();
}

FunesAgent::~FunesAgent() = default;

// ── external MCP servers ──────────────────────────────────────────────────────

static std::pair<std::string, int> parse_host_port(const std::string& url) {
    std::regex re(R"(^https?://([^/:]+)(?::(\d+))?)");
    std::smatch m;
    if (!std::regex_search(url, m, re))
        throw std::runtime_error("Invalid MCP server URL: " + url);
    return {m[1].str(), m[2].matched ? std::stoi(m[2].str()) : 80};
}

json FunesAgent::mcp_tool_to_openai(const mcp::tool& t) {
    json tj = t.to_json();
    json params = tj.contains("inputSchema")
        ? tj["inputSchema"]
        : json{{"type", "object"}, {"properties", json::object()}};
    return {
        {"type", "function"},
        {"function", {
            {"name",        t.name},
            {"description", t.description},
            {"parameters",  params}
        }}
    };
}

void FunesAgent::connect_mcp_servers() {
    // Global servers (FUNES_MCP_SERVERS, semicolon-separated) + per-agent ones.
    std::vector<McpServerConfig> servers;
    std::set<std::string> seen;

    if (const char* env = std::getenv("FUNES_MCP_SERVERS")) {
        std::string s(env);
        std::size_t pos = 0;
        while (pos < s.size()) {
            std::size_t end = s.find(';', pos);
            if (end == std::string::npos) end = s.size();
            std::string url = s.substr(pos, end - pos);
            while (!url.empty() && std::isspace(static_cast<unsigned char>(url.front()))) url.erase(url.begin());
            while (!url.empty() && std::isspace(static_cast<unsigned char>(url.back())))  url.pop_back();
            if (!url.empty() && seen.insert(url).second)
                servers.push_back({url, url});
            pos = end + 1;
        }
    }
    for (const auto& srv : cfg_.mcp_servers)
        if (seen.insert(srv.url).second)
            servers.push_back(srv);

    for (const auto& srv : servers) {
        auto [host, port] = parse_host_port(srv.url);
        auto client = std::make_unique<mcp::sse_client>(host, port);
        client->set_timeout(120);

        if (!client->initialize("funes-agent-" + cfg_.name, "1.0.0")) {
            std::cerr << "[agent:" << cfg_.name << "] WARNING: MCP server '"
                      << srv.name << "' (" << srv.url << ") unreachable — skipping.\n";
            continue;
        }

        std::vector<mcp::tool> server_tools = client->get_tools();
        const std::size_t idx = mcp_clients_.size();
        mcp_clients_.push_back(std::move(client));

        for (const auto& tool : server_tools) {
            // Native tools and earlier servers win on name collisions.
            if (tools_.has(tool.name) || mcp_tool_index_.count(tool.name)) {
                std::cerr << "[agent:" << cfg_.name << "] INFO: MCP tool '"
                          << tool.name << "' shadowed — skipping.\n";
                continue;
            }
            if (!cfg_.tools.empty()) {
                bool allowed = false;
                for (const auto& n : cfg_.tools)
                    if (n == tool.name) { allowed = true; break; }
                if (!allowed) continue;
            }
            mcp_tool_index_[tool.name] = idx;
            tools_schema_.push_back(mcp_tool_to_openai(tool));
        }
    }
}

// ── tool dispatch ─────────────────────────────────────────────────────────────

ToolResult FunesAgent::dispatch_tool(const std::string& name, const json& args,
                                     const ToolContext& ctx) {
    if (tools_.has(name))
        return tools_.call(name, args, ctx);

    auto it = mcp_tool_index_.find(name);
    if (it == mcp_tool_index_.end())
        return {"Unknown tool: " + name, /*error=*/true};

    try {
        json result = mcp_clients_[it->second]->call_tool(name, args);
        // MCP result format: {content: [{type: "text", text: "..."}]}. The
        // server is a separate process over the wire — its "text" isn't
        // guaranteed valid UTF-8, so it goes through dump_safe rather than a
        // bare .dump() (which throws on invalid UTF-8 instead of degrading).
        if (result.contains("content") && result["content"].is_array()
            && !result["content"].empty() && result["content"][0].contains("text")
            && result["content"][0]["text"].is_string())
            return {result["content"][0]["text"].get<std::string>()};
        return {funes::dump_safe(result)};
    } catch (const std::exception& e) {
        return {std::string("MCP tool '") + name + "' failed: " + e.what(), true};
    }
}

// ── run ───────────────────────────────────────────────────────────────────────

std::string FunesAgent::run(const std::string& user_message, const std::string& session,
                            const EventFn& emit, const std::vector<ImageAttachment>& images,
                            bool persist) {
    ToolContext ctx{cfg_.name, session, cfg_.workspace_dir};

    // 1. Recall relevant memories and surface them to the UI.
    std::string memory_block;
    if (defaults_.memory_recall_k > 0) {
        auto memories = memory_.recall(cfg_.name, user_message, defaults_.memory_recall_k);
        if (!memories.empty()) {
            json items = json::array();
            std::ostringstream oss;
            for (const auto& m : memories) {
                oss << "- [" << m.created_at << "] " << m.text << "\n";
                items.push_back({
                    {"id",         m.id},
                    {"text",       m.text},
                    {"source",     m.source},
                    {"score",      m.score},
                    {"created_at", m.created_at}
                });
            }
            memory_block = oss.str();
            if (emit) emit("memories", {{"items", items}});
        }
    }

    // 2. Load the rolling summary + recent turns, compressing the oldest half
    //    of the window into the summary first if it's about to crowd out the
    //    context window (automatic safety net; see context_compressor.h).
    //    Skipped for non-persisting (delegated) calls: a specialist doing one
    //    task shouldn't rewrite the shared session's summary or prune its
    //    turns as a side effect — that's the persisting caller's job.
    std::string summary = persist ? memory_.get_summary(session) : std::string();
    std::vector<ChatMessage> recent = memory_.recent_turns(session, defaults_.memory_turns);
    if (persist) {
        constexpr double kCompressTriggerFraction = 0.7;
        constexpr int    kMinKeep = 4;
        const int budget = static_cast<int>(cfg_.context_limit * kCompressTriggerFraction);
        const int estimated = estimate_tokens(cfg_.system_prompt) + estimate_tokens(summary)
                            + estimate_tokens(recent) + estimate_tokens(user_message)
                            + static_cast<int>(images.size()) * kEstimatedTokensPerImage;
        if (estimated > budget) {
            CompressOutcome result = compress_oldest_half(memory_, llm_, session, cfg_.name,
                                                           recent, summary, kMinKeep);
            if (result.compressed && emit)
                emit("context_compressed", {{"turns_folded", result.turns_folded},
                                            {"summary_preview", result.summary_preview}});
        }
    }

    // 3. Build the conversation: system (+memories+summary), recent turns, user message.
    std::vector<ChatMessage> history;
    {
        std::string sys = cfg_.system_prompt;
        if (defaults_.agent_roster &&
            (cfg_.tools.empty() ||
             std::find(cfg_.tools.begin(), cfg_.tools.end(), "delegate_to_agent") != cfg_.tools.end())) {
            std::string roster = defaults_.agent_roster(cfg_.name);
            if (!roster.empty()) {
                sys += "\n\n## Available specialist agents (delegate_to_agent)\n" + roster
                     + "Delegate to whichever of these fits the task; this list reflects "
                       "whatever agents are currently loaded.";
            }
        }
        if (!summary.empty()) {
            sys += "\n\n## Summary of earlier conversation\n" + summary;
        }
        if (!memory_block.empty()) {
            sys += "\n\n## Relevant memories from past conversations\n" + memory_block
                 + "\nUse these naturally when they help; ignore them when irrelevant.";
        }
        if (!sys.empty()) {
            ChatMessage m; m.role = "system"; m.content = sys;
            history.push_back(std::move(m));
        }
    }
    for (auto& turn : recent)
        history.push_back(std::move(turn));
    {
        ChatMessage m; m.role = "user"; m.content = user_message; m.images = images;
        history.push_back(std::move(m));
    }

    // 4. The tool loop.
    int prompt_tokens = 0;
    std::string final_text = run_loop(history, ctx, emit, prompt_tokens);

    // 5. Persist the exchange: session history always; long-term memory when
    //    auto-memory is on (source "auto" so the UI can distinguish it).
    //    Skipped for delegated calls (persist=false) — the task/answer isn't
    //    a real turn in the visible conversation, and the orchestrating
    //    call already persists its own user message and final answer.
    if (persist) {
        memory_.append_turn(session, cfg_.name, "user", user_message);
        memory_.append_turn(session, cfg_.name, "assistant", final_text);

        if (defaults_.auto_memory && !final_text.empty()) {
            std::string reply = final_text.substr(0, 300);
            if (final_text.size() > 300) reply += "…";
            try {
                memory_.remember(cfg_.name,
                                 "User said: \"" + user_message + "\" — I replied: \"" + reply + "\"",
                                 "auto");
            } catch (const std::exception& e) {
                std::cerr << "[agent:" << cfg_.name << "] auto-memory failed: "
                          << e.what() << "\n";
            }
        }
    }

    // 6. Report context usage for the UI's gauge. Real token counts come from
    //    the LLM's usage field when the backend reports one; otherwise fall
    //    back to the same char-based estimate used for the compression trigger.
    if (emit) {
        const bool estimated_usage = prompt_tokens <= 0;
        const int used = estimated_usage ? estimate_tokens(history) : prompt_tokens;
        emit("usage", {{"used", used}, {"limit", cfg_.context_limit},
                       {"estimated", estimated_usage}});
    }

    return final_text;
}

std::string FunesAgent::run_loop(std::vector<ChatMessage>& history,
                                 const ToolContext& ctx, const EventFn& emit,
                                 int& prompt_tokens_out) {
    // Exact-signature loop detection: the same (tool, args) called 3+ times
    // means a tight loop — return early with the last result. Same tool with
    // different args is legitimate.
    std::map<std::string, int> sig_counts;
    // Near-duplicate loop detection: a model that can't get a satisfying
    // result (e.g. web_search) sometimes reworks the arguments each time
    // instead of repeating them verbatim, which the exact-signature check
    // above can't catch. Cap total calls to the same tool per turn — scaled
    // to the agent's step budget so it doesn't clip legitimately
    // tool-heavy workflows, with a floor for very short budgets.
    std::map<std::string, int> name_counts;
    const int max_same_tool = std::max(cfg_.max_steps, 6);
    std::string last_tool_result;
    std::string last_tool_name;

    // Completion contract (see core/completion_contract.h): tools that must
    // have succeeded before a text answer is final. Only successful calls
    // count — an errored write_file leaves the obligation open, which is the
    // whole point.
    const funes::CompletionContract contract{cfg_.require_tools};
    std::set<std::string> satisfied;
    int nudges_used = 0;
    // Set when a nudge was just injected: that one completion is forced to
    // produce a tool call, so the model can't answer the nudge with more prose.
    bool force_tool_call = false;

    // Every "we're done here" exit below routes through this, including the
    // loop-detector and max_steps bailouts: a run that stops with a required
    // call outstanding is a failed run no matter which branch noticed first.
    auto finish = [&](const std::string& text) -> std::string {
        if (contract.active()) {
            const std::vector<std::string> missing = contract.missing(satisfied);
            if (!missing.empty()) return funes::contract_failure(missing, text);
        }
        return text;
    };

    DeltaFn on_delta;
    if (emit)
        on_delta = [&emit](const std::string& text) {
            emit("delta", {{"text", text}});
        };

    for (int step = 0; step < cfg_.max_steps; ++step) {
        CompletionResponse resp;
        if (force_tool_call) llm_.set_tool_choice("required");
        try {
            resp = llm_.complete(history, tools_schema_, on_delta);
        } catch (const std::exception& e) {
            // Some OpenAI-compatible servers reject stream=true — retry once
            // without streaming before giving up.
            if (on_delta) {
                std::cerr << "[agent:" << cfg_.name << "] streaming failed ("
                          << e.what() << "), retrying without stream\n";
                resp = llm_.complete(history, tools_schema_);
                if (emit && !resp.content.empty())
                    emit("delta", {{"text", resp.content}});
            } else {
                llm_.set_tool_choice(cfg_.tool_choice);
                throw;
            }
        }
        if (force_tool_call) {
            llm_.set_tool_choice(cfg_.tool_choice);
            force_tool_call = false;
        }
        if (resp.prompt_tokens > 0) prompt_tokens_out = resp.prompt_tokens;

        if (resp.tool_calls.empty()) {
            // The model wants to stop. If it still owes tool calls, it doesn't
            // get to — remind it and keep looping. This has to come before the
            // empty-content retry below, which exists to *extract* a text
            // answer and would otherwise cement a premature one.
            if (contract.active()) {
                const std::vector<std::string> missing = contract.missing(satisfied);
                if (!missing.empty()) {
                    if (nudges_used++ < contract.nudge_budget()) {
                        if (!resp.content.empty()) {
                            ChatMessage asst;
                            asst.role    = "assistant";
                            asst.content = resp.content;
                            history.push_back(std::move(asst));
                        }
                        ChatMessage nudge;
                        nudge.role    = "user";
                        nudge.content = funes::contract_nudge(missing);
                        history.push_back(std::move(nudge));
                        if (emit)
                            emit("contract_nudge", {{"missing", missing},
                                                    {"attempt", nudges_used}});
                        std::cerr << "[agent:" << cfg_.name << "] premature answer at step "
                                  << step << "; still owes " << missing.size()
                                  << " required call(s), nudging (" << nudges_used
                                  << "/" << contract.nudge_budget() << ")\n";
                        force_tool_call = true;
                        continue;
                    }
                    return funes::contract_failure(missing, resp.content);
                }
            }

            // Some local models return an empty completion right after a tool
            // round trip. One follow-up call with tools disabled reliably
            // produces the text answer from the results already in history.
            if (resp.content.empty() && step > 0) {
                llm_.set_tool_choice("none");
                CompletionResponse retry = llm_.complete(history, json::array(), on_delta);
                llm_.set_tool_choice(cfg_.tool_choice);
                if (retry.prompt_tokens > 0) prompt_tokens_out = retry.prompt_tokens;
                if (!retry.content.empty())
                    return retry.content;
                return last_tool_result.empty()
                    ? "(the model returned an empty answer)"
                    : last_tool_result;
            }
            return resp.content;
        }

        // Assistant message with tool_calls must precede each tool result.
        json tc_arr = json::array();
        for (const auto& tc : resp.tool_calls) {
            tc_arr.push_back({
                {"id",   tc.id},
                {"type", "function"},
                {"function", {
                    {"name",      tc.name},
                    {"arguments", tc.arguments.dump()}
                }}
            });
        }
        ChatMessage asst_msg;
        asst_msg.role       = "assistant";
        asst_msg.content    = resp.content;
        asst_msg.tool_calls = tc_arr;
        history.push_back(std::move(asst_msg));

        for (const auto& tc : resp.tool_calls) {
            const std::string call_sig = tc.name + "|" + tc.arguments.dump();
            if (++sig_counts[call_sig] >= 3)
                return finish("Done. " + last_tool_name + " completed: " + last_tool_result);
            if (++name_counts[tc.name] > max_same_tool)
                return finish("Done. " + tc.name + " was called " + std::to_string(max_same_tool) +
                       "+ times with varying arguments and made no clear progress. " +
                       "Last result: " + last_tool_result);

            if (emit) emit("tool_call", {{"name", tc.name}, {"args", tc.arguments}});

            ToolResult result = dispatch_tool(tc.name, tc.arguments, ctx);

            if (emit) {
                // Truncate on bytes first (result.text may be arbitrarily
                // long native-tool or MCP output), then trim to a clean
                // UTF-8 boundary so the preview itself is always safe to
                // dump — a bare substr() can split a multi-byte character.
                std::string preview = result.text;
                const bool was_truncated = preview.size() > 200;
                if (was_truncated) funes::truncate_utf8_safe(preview, 200);
                if (was_truncated) preview += "…";
                emit("tool_result", {{"name", tc.name}, {"preview", preview},
                                     {"error", result.error}});
            }

            last_tool_name   = tc.name;
            last_tool_result = result.text;
            if (!result.error) satisfied.insert(tc.name);

            ChatMessage tool_msg;
            tool_msg.role         = "tool";
            tool_msg.content      = result.error
                ? "{\"error\": " + funes::dump_safe(json(result.text)) + "}"
                : result.text;
            tool_msg.tool_call_id = tc.id;
            tool_msg.name         = tc.name;
            tool_msg.images       = result.images;
            history.push_back(std::move(tool_msg));
        }
    }

    return finish("[Reached max_steps=" + std::to_string(cfg_.max_steps) +
           " without a final answer. Last tool result: " + last_tool_result + "]");
}
