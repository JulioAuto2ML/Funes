// =============================================================================
// src/core/tools/agent_builder.cpp — create_agent native tool
// =============================================================================
// Unlike create_tool, agents are already data (agents/*.yaml, parsed at
// runtime by AgentConfig) — no rebuild needed. create_agent writes the YAML
// and immediately reloads FunesApi's agent table via the callback the caller
// wires in main.cpp, so a new agent shows up in the UI's agent picker right
// after this tool returns.

#include "../tools.h"
#include <yaml-cpp/yaml.h>
#include <filesystem>
#include <fstream>
#include <regex>

namespace fs = std::filesystem;

namespace {

const std::regex kNameRe(R"(^[a-z][a-z0-9_-]{1,40}$)");

bool valid_tool_choice(const std::string& s) {
    return s == "auto" || s == "required" || s == "none";
}

std::string emit_agent_yaml(const std::string& name, const std::string& description,
                            const std::string& model, const std::string& tool_choice,
                            const std::vector<std::string>& tools, int max_steps,
                            int context_limit, const std::string& system_prompt,
                            const std::string& workspace_dir,
                            const std::vector<std::string>& mcp_servers) {
    YAML::Emitter out;
    out << YAML::BeginMap;
    out << YAML::Key << "name"        << YAML::Value << name;
    out << YAML::Key << "description" << YAML::Value << description;
    out << YAML::Key << "model"       << YAML::Value << model;
    out << YAML::Key << "tool_choice" << YAML::Value << tool_choice;
    out << YAML::Key << "tools" << YAML::Value << YAML::Flow << tools;
    if (!workspace_dir.empty())
        out << YAML::Key << "workspace_dir" << YAML::Value << workspace_dir;
    if (!mcp_servers.empty())
        out << YAML::Key << "mcp_servers" << YAML::Value << YAML::Flow << mcp_servers;
    out << YAML::Key << "max_steps"     << YAML::Value << max_steps;
    out << YAML::Key << "context_limit" << YAML::Value << context_limit;
    out << YAML::Key << "system_prompt" << YAML::Value << YAML::Literal << system_prompt;
    out << YAML::EndMap;
    return out.c_str();
}

ToolResult create_agent_handler(const ToolRegistry& reg, const std::string& agents_dir,
                                const std::function<void()>& reload_agents,
                                const json& args, const ToolContext&) {
    if (!args.contains("name") || !args["name"].is_string())
        return {"Missing 'name' argument", true};
    const std::string name = args["name"].get<std::string>();
    if (!std::regex_match(name, kNameRe))
        return {"'name' must be lowercase, start with a letter, and use only "
                "letters/digits/'-'/'_' (2-41 chars)", true};

    if (!args.contains("description") || !args["description"].is_string()
        || args["description"].get<std::string>().empty())
        return {"Missing or empty 'description' argument", true};
    const std::string description = args["description"].get<std::string>();

    if (!args.contains("system_prompt") || !args["system_prompt"].is_string()
        || args["system_prompt"].get<std::string>().empty())
        return {"Missing or empty 'system_prompt' argument — draft the full prompt "
                "yourself based on the conversation before calling this", true};
    const std::string system_prompt = args["system_prompt"].get<std::string>();

    if (!args.contains("tools") || !args["tools"].is_array())
        return {"Missing 'tools' argument (array of tool names, call list_tools first)", true};
    std::vector<std::string> tools;
    for (const auto& t : args["tools"]) {
        if (!t.is_string())
            return {"'tools' must be an array of strings", true};
        const std::string tname = t.get<std::string>();
        if (!reg.has(tname))
            return {"Unknown tool '" + tname + "' — call list_tools to see what's "
                    "available", true};
        tools.push_back(tname);
    }

    const std::string model = args.value("model", std::string("default"));
    const std::string tool_choice = args.value("tool_choice", std::string("auto"));
    if (!valid_tool_choice(tool_choice))
        return {"'tool_choice' must be one of: auto, required, none", true};

    const int max_steps     = args.value("max_steps", 8);
    const int context_limit = args.value("context_limit", 8192);
    if (max_steps < 1 || max_steps > 64)
        return {"'max_steps' must be between 1 and 64", true};
    if (context_limit < 512 || context_limit > 1'000'000)
        return {"'context_limit' must be between 512 and 1,000,000", true};

    const bool overwrite = args.value("overwrite", false);
    fs::path path = fs::path(agents_dir) / (name + ".yaml");
    if (fs::exists(path) && !overwrite)
        return {"An agent named '" + name + "' already exists at " + path.string() +
                " — pass overwrite=true to replace it.", true};

    const std::string workspace_dir = args.value("workspace_dir", std::string());

    std::vector<std::string> mcp_servers;
    if (args.contains("mcp_servers")) {
        if (!args["mcp_servers"].is_array())
            return {"'mcp_servers' must be an array of URL strings", true};
        for (const auto& m : args["mcp_servers"]) {
            if (!m.is_string())
                return {"'mcp_servers' must be an array of URL strings", true};
            mcp_servers.push_back(m.get<std::string>());
        }
    }

    std::error_code ec;
    fs::create_directories(agents_dir, ec);

    const std::string yaml_text = emit_agent_yaml(name, description, model, tool_choice,
                                                  tools, max_steps, context_limit, system_prompt,
                                                  workspace_dir, mcp_servers);
    std::ofstream f(path, std::ios::trunc);
    if (!f)
        return {"Could not write " + path.string(), true};
    f << yaml_text << "\n";
    f.close();

    if (reload_agents) reload_agents();

    return {"Created agent '" + name + "' at " + path.string() +
            " and reloaded — it's available now in the agent picker."};
}

} // namespace

void register_agent_builder(ToolRegistry& reg, const std::string& agents_dir,
                            std::function<void()> reload_agents) {
    reg.add({
        "create_agent",
        "Create a new Funes agent from a fully-drafted system prompt and a chosen "
        "tool allowlist. Takes effect immediately (no rebuild) — the agent appears "
        "in the UI's agent picker right away. Gather the agent's purpose, tone, and "
        "needed capabilities through conversation first (use list_tools to see valid "
        "tool names), draft the complete system_prompt yourself, confirm the plan "
        "with the user, then call this once. When overwriting an EXISTING agent "
        "(e.g. to fix one), this replaces the whole file — read its current YAML "
        "first and pass through any workspace_dir/mcp_servers it already has "
        "unless you're deliberately changing them, or they'll be silently dropped.",
        {
            {"type", "object"},
            {"properties", {
                {"name",          {{"type", "string"}, {"description", "lowercase-with-hyphens agent name"}}},
                {"description",   {{"type", "string"}, {"description", "One-line summary shown in the agent picker"}}},
                {"system_prompt", {{"type", "string"}, {"description", "The full system prompt for this agent"}}},
                {"tools",         {{"type", "array"}, {"items", {{"type", "string"}}},
                                   {"description", "Tool names this agent may call (see list_tools)"}}},
                {"model",         {{"type", "string"}, {"description", "LLM model, or 'default' to inherit"}}},
                {"tool_choice",   {{"type", "string"}, {"description", "auto|required|none, default auto"}}},
                {"max_steps",     {{"type", "integer"}, {"description", "Max tool-call rounds per turn, default 8"}}},
                {"context_limit", {{"type", "integer"}, {"description", "Context window size, default 8192"}}},
                {"workspace_dir", {{"type", "string"}, {"description", "Overrides the default workspace directory for this agent's read_file/write_file/execute_shell (omit to inherit the server default)"}}},
                {"mcp_servers",   {{"type", "array"}, {"items", {{"type", "string"}}},
                                   {"description", "Extra MCP server URLs this agent connects to, on top of native tools (omit if none)"}}},
                {"overwrite",     {{"type", "boolean"}, {"description", "Replace an existing agent of the same name"}}}
            }},
            {"required", json::array({"name", "description", "system_prompt", "tools"})}
        },
        [&reg, agents_dir, reload_agents](const json& args, const ToolContext& ctx) {
            return create_agent_handler(reg, agents_dir, reload_agents, args, ctx);
        }
    });
}
