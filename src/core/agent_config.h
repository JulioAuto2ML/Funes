// =============================================================================
// src/core/agent_config.h — parsed agent definition (.yaml)
// =============================================================================
// Slimmed port from AresOS: capability tokens, governance, and priority are
// gone. An agent is a name, a prompt, a tool allowlist, and LLM settings.

#pragma once
#include <string>
#include <vector>
#include "json.hpp"
#include "tool_budget.h"

// Connection details for one external MCP server. Exactly one of url/command
// is expected to be set — url picks the (legacy) HTTP+SSE transport, command
// spawns a subprocess and speaks MCP over its stdin/stdout (the transport
// most published MCP servers actually use, e.g. "npx -y some-mcp-server").
struct McpServerConfig {
    std::string url;           // e.g. "http://localhost:9000" — SSE transport
    std::string command;       // e.g. "npx -y rss-reader-mcp" — stdio transport
    std::string name;          // optional human label for logs; defaults to url/command
    nlohmann::json env = nlohmann::json::object(); // extra env vars for a stdio server's process
};

struct AgentConfig {
    std::string name;
    std::string description;

    // LLM backend — empty fields inherit the FUNES_LLM_* defaults.
    std::string model = "default";
    std::string llm_url;
    std::string llm_api_key;
    std::string llm_provider;  // "openai" | "anthropic" | "" (inherit)

    // Native + MCP tools this agent may call. Empty list = all available.
    std::vector<std::string> tools;

    // Scripts from the central library (FUNES_SCRIPTS_DIR, default
    // ./scriptlib) this agent may run with run_script. **Empty means none** —
    // the opposite of `tools:` above, and the asymmetry is the point: a tool
    // is code compiled into this binary, while a script is a file an admin
    // drops into a directory, and "every agent may run every file that
    // appears there" is not a default anybody would choose deliberately.
    //
    // This is what an agent gets *instead of* execute_shell: the one job it
    // actually needs to run, named, rather than the ability to run anything.
    // See core/script_library.h.
    std::vector<std::string> scripts;

    // Additional MCP servers for this agent. Their tools are merged with the
    // native registry and filtered by the allowlist above.
    std::vector<McpServerConfig> mcp_servers;

    std::string system_prompt;
    std::string tool_choice = "auto"; // "auto" | "required" | "none"

    // Sampling temperature for this agent's own completions (not
    // classify_decision's internal forward passes, which always force 0.0
    // regardless of this). Sentinel -1 = unset, inherit LLMClient's own
    // default (0.2f) — every agent that doesn't name one keeps today's
    // behavior. Set to 0.0 for an agent whose job is a fixed transformation
    // rather than a judgment call with room for phrasing (classifier: variance
    // here means occasionally ignoring "call with {}" or looping a nudge
    // retry, not a better or more creative answer).
    float temperature = -1.0f;

    // Whether a person's run with this agent writes the `User said: ... — I
    // replied: ...` auto-memory. Can only switch it off: FUNES_AUTO_MEMORY=0
    // still wins. Off for an agent whose conversations are commands ("run
    // today's newsletter", "list my drafts") — those logs carry no fact, match
    // the next identical command best, and get recalled into it with the old
    // reply attached: a stale issue's headlines, a failure from weeks ago.
    // Measured on the deployment: over half of all recall hits were such logs.
    bool auto_memory = true;

    // Completion contract: tools that must have succeeded before a plain-text
    // answer is accepted as this agent's final answer. Empty = no contract (a
    // model may finish whenever it likes). Use it for agents whose real output
    // is a side effect — a file written, a mail sent — so "described the work"
    // can't pass for "did the work". See core/completion_contract.h.
    std::vector<std::string> require_tools;

    // Name of a tool whose own result IS this agent's final answer, verbatim
    // — skip asking the model to re-type it as text. For a "Stateless
    // sub-agent" (one tool, answer_schema mirroring its output — see
    // agents/README.md's archetype table) that final-answer step is a full
    // extra completion call spent regenerating content the tool already
    // produced. Empty = today's behavior (always ask for a written answer).
    //
    // Only takes effect on a *successful* call to this tool, and still goes
    // through the same contract/answer_schema validation (FunesAgent::
    // run_loop's `finish()`) as every other exit path — a misconfigured
    // value (naming a tool not in require_tools, say) just produces the
    // existing contract-failure message instead of silently skipping a
    // required call.
    std::string answer_from_tool;

    // The other side of require_tools: how many times a tool may be called in
    // one run. Empty = no ceilings. Past the ceiling the call is refused with
    // a message telling the model to conclude, rather than the run being
    // killed — for agents that would otherwise research forever. See
    // core/tool_budget.h.
    funes::ToolLimits tool_limits;

    // Answer schema: the JSON shape this agent's final answer must have. Empty
    // = no contract (any text is accepted, today's behavior). Declared in YAML
    // as a JSON-Schema subset; when set, a matching instruction block is
    // appended to system_prompt at load so the two can't drift apart. See
    // core/answer_schema.h.
    nlohmann::json answer_schema;

    // Overrides the server-wide FUNES_WORKSPACE_DIR for this agent's
    // read_file/write_file/execute_shell calls. Empty = inherit the default.
    std::string workspace_dir;

    // Short note injected alongside this agent's description in the roster
    // that the orchestrator sees. Use it for delegation-specific instructions
    // that the caller needs (e.g. "keep relaying until confirmed") without
    // baking agent-specific routing into the orchestrator's own prompt.
    std::string delegation_notes;

    // Set when this agent's tools authenticate as the *installation* rather
    // than as the caller — an MCP server holding one mailbox's credentials,
    // a bridge connected to one phone number, a publish directory with one
    // subscriber list. The value is the prose reason a person reads
    // ("the installation's Gmail mailbox").
    //
    // Funes isolates its own data — memories, turns, stored results,
    // workspace files — in SQL and in fs_guard. It cannot isolate an account
    // on somebody else's system reached with credentials the whole install
    // shares, so an agent that carries one is not a thing an admin can
    // safely grant per-account. Declaring it here lets the runtime say that,
    // instead of leaving a member to read "you do not have access" and guess
    // whether asking an admin would help. It grants nothing and restricts
    // nothing by itself: the agent allowlist is still what decides.
    std::string shared_identity;

    // Which agent's memory pool recall()/remember() read and write for this
    // agent — both the pre-injected recall in FunesAgent::run and the
    // recall/remember tools. Defaults to this agent's own name (i.e. every
    // agent has its own isolated memory, today's behavior); set it to
    // another agent's name to share that pool instead, e.g. a second
    // front end for the same assistant. Always resolved to a concrete name
    // at load time — never empty after parsing.
    std::string memory_scope;

    int context_limit = 8192;
    int max_steps     = 8;    // max tool-call rounds per turn

    static AgentConfig from_file(const std::string& path);
    static AgentConfig from_string(const std::string& yaml_content);
};
