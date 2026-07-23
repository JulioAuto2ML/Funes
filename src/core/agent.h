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
// =============================================================================

#pragma once
#include "agent_config.h"
#include "llm_client.h"
#include "memory.h"
#include "tools.h"
#include "mcp_sse_client.h"
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
    int         memory_turns = 10;      // past turns loaded per run
    int         memory_recall_k = 4;    // memories injected per run
    bool        auto_memory  = true;    // store each exchange as a memory
};

class FunesAgent {
public:
    // tools and memory must outlive the agent. One FunesAgent per request.
    FunesAgent(const AgentConfig& cfg, ToolRegistry& tools, MemoryStore& memory,
               const AgentDefaults& defaults);
    ~FunesAgent();

    // Run one conversational turn. Returns the final assistant text.
    // Throws std::runtime_error on unrecoverable LLM errors.
    std::string run(const std::string& user_message, const std::string& session,
                    const EventFn& emit = nullptr);

    const AgentConfig& config() const { return cfg_; }

private:
    AgentConfig   cfg_;
    ToolRegistry& tools_;
    MemoryStore&  memory_;
    AgentDefaults defaults_;
    LLMClient     llm_;
    json          tools_schema_;   // native + MCP, filtered by allowlist

    // External MCP servers (usually none). tool name → client index.
    std::vector<std::unique_ptr<mcp::sse_client>> mcp_clients_;
    std::unordered_map<std::string, std::size_t>  mcp_tool_index_;

    void connect_mcp_servers();
    static json mcp_tool_to_openai(const mcp::tool& t);

    // Dispatch one tool call: native registry first, then MCP servers.
    ToolResult dispatch_tool(const std::string& name, const json& args,
                             const ToolContext& ctx);

    std::string run_loop(std::vector<ChatMessage>& history, const ToolContext& ctx,
                         const EventFn& emit);
};
