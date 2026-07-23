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

using json = nlohmann::json;

class MemoryStore;

// Per-call context: which agent and chat session triggered the tool.
struct ToolContext {
    std::string agent;
    std::string session;
};

struct ToolResult {
    std::string text;
    bool        error = false;
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
