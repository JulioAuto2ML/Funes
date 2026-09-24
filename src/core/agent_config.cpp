// =============================================================================
// src/core/agent_config.cpp — YAML parsing for AgentConfig
// =============================================================================
// Expected YAML schema (all optional except `name`) — see agents/ for examples.
// Uses yaml-cpp (libyaml-cpp-dev).

#include "agent_config.h"
#include "answer_schema.h"
#include "yaml_json.h"
#include <yaml-cpp/yaml.h>
#include <cstdlib>
#include <stdexcept>

// Expand ${VAR} references in a string from the process environment.
// Unset variables expand to the empty string — a missing key is not an error
// here; the agent simply inherits the global default for that field.
static std::string expand_env(const std::string& s) {
    std::string out;
    size_t i = 0;
    while (i < s.size()) {
        if (i + 1 < s.size() && s[i] == '$' && s[i + 1] == '{') {
            size_t close = s.find('}', i + 2);
            if (close != std::string::npos) {
                std::string var = s.substr(i + 2, close - i - 2);
                const char* val = std::getenv(var.c_str());
                if (val) out += val;
                i = close + 1;
                continue;
            }
        }
        out += s[i++];
    }
    return out;
}

static AgentConfig from_node(const YAML::Node& root, const std::string& source) {
    if (!root["name"])
        throw std::runtime_error("Agent YAML missing required field 'name' in: " + source);

    AgentConfig cfg;
    cfg.name          = root["name"].as<std::string>();
    cfg.description   = root["description"]   ? root["description"].as<std::string>()   : "";
    cfg.model         = root["model"]         ? root["model"].as<std::string>()         : "default";
    cfg.llm_url       = root["llm_url"]       ? expand_env(root["llm_url"].as<std::string>())       : "";
    cfg.llm_api_key   = root["llm_api_key"]   ? expand_env(root["llm_api_key"].as<std::string>())   : "";
    cfg.llm_provider  = root["llm_provider"]  ? expand_env(root["llm_provider"].as<std::string>())  : "";
    cfg.system_prompt = root["system_prompt"] ? root["system_prompt"].as<std::string>() : "";
    cfg.tool_choice   = root["tool_choice"]   ? root["tool_choice"].as<std::string>()   : "auto";
    if (root["temperature"]) cfg.temperature = root["temperature"].as<float>();
    if (root["auto_memory"]) cfg.auto_memory = root["auto_memory"].as<bool>();
    cfg.workspace_dir    = root["workspace_dir"]    ? root["workspace_dir"].as<std::string>()    : "";
    cfg.delegation_notes = root["delegation_notes"] ? root["delegation_notes"].as<std::string>() : "";
    cfg.shared_identity  = root["shared_identity"]  ? root["shared_identity"].as<std::string>()  : "";
    cfg.memory_scope     = root["memory_scope"]     ? root["memory_scope"].as<std::string>()     : cfg.name;

    if (root["context_limit"])
        cfg.context_limit = root["context_limit"].as<int>();
    if (root["max_steps"])
        cfg.max_steps = root["max_steps"].as<int>();

    if (root["tools"] && root["tools"].IsSequence()) {
        for (const auto& t : root["tools"])
            cfg.tools.push_back(t.as<std::string>());
    }

    // scripts: [backup_db, ...] — names in the central script library. No
    // validation against the library here: agent_config.cpp does not know
    // where it is, and an agent must still load when one granted script is
    // missing (the run_script call says so, naming the script).
    if (root["scripts"] && root["scripts"].IsSequence()) {
        for (const auto& s : root["scripts"])
            cfg.scripts.push_back(s.as<std::string>());
    }

    if (root["require_tools"] && root["require_tools"].IsSequence()) {
        for (const auto& t : root["require_tools"])
            cfg.require_tools.push_back(t.as<std::string>());
    }

    cfg.answer_from_tool = root["answer_from_tool"] ? root["answer_from_tool"].as<std::string>() : "";

    // tool_limits: { web_search: 5 } — see core/tool_budget.h.
    if (root["tool_limits"] && root["tool_limits"].IsMap()) {
        for (const auto& kv : root["tool_limits"])
            cfg.tool_limits[kv.first.as<std::string>()] = kv.second.as<int>();
    }

    if (root["answer_schema"] && root["answer_schema"].IsMap()) {
        cfg.answer_schema = funes::yaml_to_json(root["answer_schema"]);
        // The prompt half of the contract is generated, never hand-written:
        // an agent author edits one place and the model is told exactly what
        // the loop will enforce.
        std::string block = funes::schema_prompt_block(cfg.answer_schema);
        if (!block.empty()) {
            if (!cfg.system_prompt.empty()) cfg.system_prompt += "\n\n";
            cfg.system_prompt += block;
        }
    }

    if (root["mcp_servers"] && root["mcp_servers"].IsSequence()) {
        for (const auto& entry : root["mcp_servers"]) {
            McpServerConfig srv;
            if (entry.IsScalar()) {
                // Bare URL shorthand: "- http://localhost:9000"
                srv.url  = entry.as<std::string>();
                srv.name = srv.url;
            } else {
                if (entry["url"])     srv.url     = entry["url"].as<std::string>();
                // ${VAR} expands in the command too, not just in env: a stdio
                // server's path is machine-local (dev checkout vs. deployment
                // host), and the alternative is an absolute path committed to
                // the repo that the other machine edits by hand on every pull.
                if (entry["command"]) srv.command = expand_env(entry["command"].as<std::string>());
                if (entry["name"])    srv.name    = entry["name"].as<std::string>();
                if (entry["env"] && entry["env"].IsMap()) {
                    // ${VAR} expands from the server's environment here, the
                    // way llm_api_key does: since children no longer inherit
                    // that environment (proc::child_environment), this is how
                    // a YAML hands its MCP server one credential from
                    // funes.local without writing the secret into the repo.
                    for (const auto& kv : entry["env"])
                        srv.env[kv.first.as<std::string>()] =
                            expand_env(kv.second.as<std::string>());
                }
                if (srv.name.empty()) srv.name = srv.command.empty() ? srv.url : srv.command;
            }
            if (!srv.url.empty() || !srv.command.empty())
                cfg.mcp_servers.push_back(std::move(srv));
        }
    }

    return cfg;
}

AgentConfig AgentConfig::from_file(const std::string& path) {
    YAML::Node root;
    try {
        root = YAML::LoadFile(path);
    } catch (const YAML::Exception& e) {
        throw std::runtime_error("Failed to parse agent YAML '" + path + "': " + e.what());
    }
    return from_node(root, path);
}

AgentConfig AgentConfig::from_string(const std::string& yaml_content) {
    YAML::Node root;
    try {
        root = YAML::Load(yaml_content);
    } catch (const YAML::Exception& e) {
        throw std::runtime_error("Failed to parse agent YAML string: " + std::string(e.what()));
    }
    return from_node(root, "<string>");
}
