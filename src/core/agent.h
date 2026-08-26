// =============================================================================
// src/core/agent.h — FunesAgent: one agent turn with memory and tools
// =============================================================================
//
// Slimmed port of the AresOS AgentInstance. What remains:
//   - the multi-step LLM ↔ tool loop with exact-signature loop detection
//   - YAML agent config (prompt, tool allowlist, LLM settings)
//   - external MCP server support (tools merged with the native registry)
// What was cut: capability tokens, governance hooks, audit log, pause/resume,
// sub-agent spawning.
//
// What was added:
//   - automatic memory: relevant memories are recalled and injected into the
//     system prompt before the run; the exchange is stored afterwards
//   - conversation history per session (loaded from / saved to MemoryStore)
//   - an event callback so the HTTP layer can stream progress live:
//       "memories"    {items: [{id, text, score, created_at}]}
//       "delta"       {text}                       — streamed answer fragment
//       "tool_call"   {name, args}
//       "tool_result" {name, preview, error}
//       "context_compressed" {turns_folded, summary_preview}
//       "usage"       {used, limit, estimated}     — one per run(), context gauge
// =============================================================================

#pragma once
#include "agent_config.h"
#include "context_compressor.h"
#include "llm_client.h"
#include "memory.h"
#include "tools.h"
#include "mcp_client.h"
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

using EventFn = std::function<void(const std::string& type, const json& data)>;

struct AgentDefaults {
    std::string llm_url      = "http://localhost:8080";
    std::string llm_api_key;
    std::string llm_provider = "openai";
    std::string llm_model;              // used when agent model is "default"
    std::string vision_url;             // if set, image-bearing turns use this endpoint
    int         memory_turns = 10;      // past turns loaded per run
    int         memory_recall_k = 4;    // memories injected per run
    bool        auto_memory  = true;    // store each exchange as a memory

    // Returns a "- name: description" line per other loaded agent, excluding
    // the caller (by name). Wired up in main.cpp once FunesApi owns the agent
    // table, so an agent with the delegate_to_agent tool learns its roster
    // from agents/*.yaml at request time instead of a hardcoded prompt list.
    // Left unset in contexts without an agent table (e.g. tests).
    std::function<std::string(const std::string&)> agent_roster;
};

class FunesAgent {
public:
    // tools and memory must outlive the agent. One FunesAgent per request.
    FunesAgent(const AgentConfig& cfg, ToolRegistry& tools, MemoryStore& memory,
               const AgentDefaults& defaults);
    ~FunesAgent();

    // Run one conversational turn. `images` (if any) attach to this turn's
    // user message only — they are not persisted into session history, so
    // they don't replay (or bloat storage) on future turns. Whether the
    // model actually sees them depends on the backend supporting vision.
    //
    // `persist=false` skips storing the exchange (session history + auto-
    // memory) entirely — used for delegated sub-agent calls (see
    // src/core/tools/delegation.cpp), so a specialist's internal task/answer
    // doesn't show up as a separate turn in the visible conversation. The
    // specialist's own recall/remember tool calls are unaffected either way.
    //
    // `perms` is what that user may do: it narrows the tool schema this run
    // offers the model, and is re-checked at dispatch. It only ever
    // restricts — the result is intersected with the agent's own tool list,
    // so granting a user a tool cannot give it to them through an agent that
    // was never given it.
    //
    // `user_id` is whose memories, history and stored results this run reads
    // and writes. Required rather than defaulted: every caller genuinely
    // knows who it is acting for — the API from the authenticated request,
    // the cron runner from the job's owner, delegation from its caller — and
    // a default would let a new call site silently write into the admin's
    // pool instead of failing to compile.
    //
    // Returns the final assistant text. Throws std::runtime_error on
    // unrecoverable LLM errors.
    std::string run(const std::string& user_message, const std::string& session,
                    int64_t user_id,
                    const funes::Permissions& perms,
                    const EventFn& emit = nullptr,
                    const std::vector<ImageAttachment>& images = {},
                    bool persist = true);

    const AgentConfig& config() const { return cfg_; }

private:
    AgentConfig   cfg_;
    ToolRegistry& tools_;
    MemoryStore&  memory_;
    AgentDefaults defaults_;
    LLMClient     llm_;
    std::unique_ptr<LLMClient> vision_llm_;  // set when FUNES_VISION_URL is configured
    json          tools_schema_;   // native + MCP, filtered by allowlist

    // External MCP servers (usually none), SSE or stdio. tool name → client index.
    std::vector<std::unique_ptr<mcp::client>>     mcp_clients_;
    std::unordered_map<std::string, std::size_t>  mcp_tool_index_;

    void connect_mcp_servers();
    static json mcp_tool_to_openai(const mcp::tool& t);

    // Dispatch one tool call: native registry first, then MCP servers.
    ToolResult dispatch_tool(const std::string& name, const json& args,
                             const ToolContext& ctx);

    std::string run_loop(std::vector<ChatMessage>& history, const ToolContext& ctx,
                         const EventFn& emit, int& prompt_tokens_out,
                         bool use_vision_first = false);
};

namespace funes {
std::atomic<bool>*& cancel_flag();
}
