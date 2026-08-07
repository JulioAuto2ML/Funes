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

    // Additional MCP servers for this agent. Their tools are merged with the
    // native registry and filtered by the allowlist above.
    std::vector<McpServerConfig> mcp_servers;

    std::string system_prompt;
    std::string tool_choice = "auto"; // "auto" | "required" | "none"

    // Completion contract: tools that must have succeeded before a plain-text
    // answer is accepted as this agent's final answer. Empty = no contract (a
    // model may finish whenever it likes). Use it for agents whose real output
    // is a side effect — a file written, a mail sent — so "described the work"
    // can't pass for "did the work". See core/completion_contract.h.
    std::vector<std::string> require_tools;

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
