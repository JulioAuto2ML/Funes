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
#include "agent_roster.h"
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

    // The caller's view of the other loaded agents, excluding itself: the ones
    // it may delegate to, and the ones it may not with the reason why. Wired
    // up in main.cpp once FunesApi owns the agent table, so an agent with the
    // delegate_to_agent tool learns its roster from agents/*.yaml at request
    // time instead of from a hardcoded prompt list.
    //
    // Takes the caller's permissions because an unfiltered roster is worse
    // than no roster: the model reads about an agent it cannot use, delegates
    // to it, is refused, and then has to explain a failure whose cause is not
    // in its context — so it invents one. See core/agent_roster.h.
    //
    // Left unset in contexts without an agent table (e.g. tests).
    std::function<funes::Roster(const std::string&, const funes::Permissions&)> agent_roster;

    // 5.0: the locale of the account a run is acting for ("es", "pt-BR"), or
    // "" when it cannot be resolved. Wired in main.cpp from UserStore, the
    // same way agent_roster is wired from the agent table — the alternative
    // was a UserStore reference inside FunesAgent, which would put
    // authentication state in the loop that runs the model.
    //
    // A resolver rather than a field because the locale belongs to whoever
    // the run is *for*, not to the server or the agent: delegated sub-agents
    // and cron jobs carry the caller's user_id, so they get the right
    // language without any of them having to pass it along.
    std::function<std::string(int64_t)> user_locale;

    // Where the central script library lives (FUNES_SCRIPTS_DIR, default
    // ./scriptlib). The loop reads it to tell an agent, in its system prompt,
    // which scripts it may run and what arguments they take — the same
    // treatment the agent roster gets, and for the same reason: a capability
    // the model has to discover with a tool call is one a small model will
    // instead guess at, usually by reaching for execute_shell. Empty in
    // contexts with no library (tests); run_script then has nothing to load
    // and says so.
    std::string scripts_dir;
};

// What a run leaves behind in the database. See FunesAgent::run.
enum class Persist {
    None,       // nothing — a delegated sub-agent call
    TurnsOnly,  // session history, but no auto-memory — a scheduled job
    Full        // history and auto-memory — a person talking
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
    // `persist` decides what this run leaves behind. It is three-valued
    // rather than a bool because the two things it used to gate together are
    // wanted separately:
    //
    //   Full      — a real conversation. Session history, the rolling summary
    //               and the auto-memory write.
    //   TurnsOnly — a transcript worth keeping, authored by nobody. Scheduled
    //               runs: the turns are the post-mortem record, but the
    //               auto-memory would say `User said: "<the job's task>"`,
    //               and the scheduler is not the user. Those memories get
    //               recalled into real conversations as things the person
    //               said, which is the actual harm — the clutter is secondary.
    //   None      — delegated sub-agent calls (src/core/tools/delegation.cpp):
    //               the task/answer isn't a turn in the visible conversation,
    //               and the orchestrating call persists its own.
    //
    // The specialist's own recall/remember tool calls are unaffected by all
    // three — an agent that deliberately stores something still does.
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
                    Persist persist = Persist::Full);

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

    // Set by run_loop when the model calls (or is refused) any tool other than
    // recall/remember. Such a run was a command — "run the newsletter", "list
    // my drafts" — and its exchange is not written as an auto-memory: it holds
    // no fact, it matches the next identical command best, and it was recalled
    // into that command with the old reply attached (a past issue's headlines,
    // a mail failure from weeks earlier). Measured on the deployment: 54% of
    // all recall hits were auto rows, the most-recalled of them commands.
    bool used_action_tool_ = false;

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
