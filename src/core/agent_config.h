// =============================================================================
// src/core/agent_config.h — parsed agent definition (.yaml)
// =============================================================================
// Slimmed port from AresOS: capability tokens, governance, and priority are
// gone. An agent is a name, a prompt, a tool allowlist, and LLM settings.

#pragma once
#include <string>
#include <vector>

// Connection details for one external MCP server.
struct McpServerConfig {
    std::string url;           // e.g. "http://localhost:9000"
    std::string name;          // optional human label for logs; defaults to url
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

    // Overrides the server-wide FUNES_WORKSPACE_DIR for this agent's
    // read_file/write_file/execute_shell calls. Empty = inherit the default.
    std::string workspace_dir;

    int context_limit = 8192;
    int max_steps     = 8;    // max tool-call rounds per turn

    static AgentConfig from_file(const std::string& path);
    static AgentConfig from_string(const std::string& yaml_content);
};
