// =============================================================================
// src/core/tools.h — in-process tool registry
// =============================================================================
//
// Funes tools are plain C++ functions registered at startup — no HTTP hop, no
// protocol overhead. The registry converts tool definitions to OpenAI function
// schemas for the LLM, and dispatches calls made by the agent loop.
//
// External MCP servers are still supported (the agent merges their tools with
// this registry), but the built-in tools live here.
// =============================================================================

#pragma once
#include <functional>
#include <map>
#include <string>
#include <vector>
#include "json.hpp"
#include "llm_client.h"

using json = nlohmann::json;

class MemoryStore;
struct AgentDefaults;
struct AgentConfig;

// Per-call context: which user, agent and chat session triggered the tool.
//
// Not a plain aggregate: the constructor defaults memory_scope to agent when
// left unset, so every existing 2-/3-arg `ToolContext{agent, session, ...}`
// call site (tests included) keeps today's behavior — its own isolated
// memory — without having to be updated. Only a caller that explicitly
// passes a 4th argument (FunesAgent::run, from AgentConfig::memory_scope)
// gets the sharing behavior.
struct ToolContext {
    std::string agent;
    std::string session;
    // Overrides the workspace directory read_file/write_file/execute_shell
    // were registered with, e.g. a skill-specific agent scoped to one
    // folder. Empty = use the server-wide default.
    std::string workspace_dir;
    // Which agent's memory pool recall/remember read and write — see
    // AgentConfig::memory_scope.
    std::string memory_scope;
    // Whose data this call reads and writes. Every memory, turn and stored
    // result the tool touches is scoped to it. Trailing (and defaulted to the
    // admin) rather than leading so the existing positional call sites in
    // tests keep compiling; the paths that matter — FunesAgent::run and
    // delegate_to_agent — all set it explicitly from the authenticated
    // request, and there is a test asserting delegation carries it across.
    int64_t user_id = 1;

    ToolContext(std::string agent_, std::string session_,
                std::string workspace_dir_ = "", std::string memory_scope_ = "",
                int64_t user_id_ = 1)
        : agent(std::move(agent_)), session(std::move(session_)),
          workspace_dir(std::move(workspace_dir_)),
          memory_scope(memory_scope_.empty() ? agent : std::move(memory_scope_)),
          user_id(user_id_) {}
};

struct ToolResult {
    std::string text;
    bool        error = false;
    // Optional images alongside text (e.g. rendered pages of an image-only
    // PDF that has no extractable text layer) — carried into the tool-result
    // ChatMessage so a vision-capable backend can see them.
    std::vector<ImageAttachment> images;
};

using ToolHandler = std::function<ToolResult(const json& args, const ToolContext& ctx)>;

struct NativeTool {
    std::string name;
    std::string description;
    json        parameters;   // JSON-schema object for the arguments
    ToolHandler handler;
};

class ToolRegistry {
public:
    void add(NativeTool tool);
    bool has(const std::string& name) const { return tools_.count(name) > 0; }

    std::vector<std::string> names() const;

    // OpenAI-format tool definitions, filtered by allowlist (empty = all).
    json openai_schema(const std::vector<std::string>& allowlist = {}) const;

    // Dispatch a call. Handler exceptions become error ToolResults — a tool
    // failure must never crash an agent run.
    ToolResult call(const std::string& name, const json& args,
                    const ToolContext& ctx) const;

private:
    std::map<std::string, NativeTool> tools_;
};

// ── built-in tool packs ───────────────────────────────────────────────────────
void register_web_tools(ToolRegistry& reg);                        // web_search, web_fetch
void register_memory_tools(ToolRegistry& reg, MemoryStore& store); // remember, recall
void register_result_tools(ToolRegistry& reg, MemoryStore& store); // read_result
void register_context_tools(ToolRegistry& reg, MemoryStore& store,
                            const AgentDefaults& defaults);        // compress_context
void register_introspection_tools(ToolRegistry& reg);              // list_tools
void register_tool_builder(ToolRegistry& reg,
                           const std::string& generated_dir);       // create_tool
void register_agent_builder(ToolRegistry& reg, const std::string& agents_dir,
                            std::function<void()> reload_agents);  // create_agent
void register_file_tools(ToolRegistry& reg,
                         const std::string& workspace_dir);        // read_file, write_file
void register_shell_tool(ToolRegistry& reg,
                         const std::string& workspace_dir);        // execute_shell
void register_delegation_tool(ToolRegistry& reg, MemoryStore& memory, const AgentDefaults& defaults,
                              std::function<AgentConfig(const std::string&)> find_agent,
                              std::function<std::vector<std::string>()> list_agent_names);
                                                                     // delegate_to_agent
