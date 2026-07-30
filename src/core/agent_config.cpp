// =============================================================================
// src/core/agent_config.cpp — YAML parsing for AgentConfig
// =============================================================================
// Expected YAML schema (all optional except `name`) — see agents/ for examples.
// Uses yaml-cpp (libyaml-cpp-dev).

#include "agent_config.h"
#include "answer_schema.h"
#include <yaml-cpp/yaml.h>
#include <stdexcept>

// YAML and JSON are the same data model here, so `answer_schema:` is written
// as ordinary YAML and converted rather than embedded as a JSON string.
// yaml-cpp scalars are untyped, so numbers and booleans are recovered by
// trying the narrowest type first — a schema's `minItems: 2` has to survive as
// a number, and `"2"` would validate nothing.
static nlohmann::json yaml_to_json(const YAML::Node& node) {
    switch (node.Type()) {
        case YAML::NodeType::Map: {
            nlohmann::json obj = nlohmann::json::object();
            for (const auto& kv : node)
                obj[kv.first.as<std::string>()] = yaml_to_json(kv.second);
            return obj;
        }
        case YAML::NodeType::Sequence: {
            nlohmann::json arr = nlohmann::json::array();
            for (const auto& item : node)
                arr.push_back(yaml_to_json(item));
            return arr;
        }
        case YAML::NodeType::Scalar: {
            bool b;
            if (YAML::convert<bool>::decode(node, b)) return b;
            long long i;
            if (YAML::convert<long long>::decode(node, i)) return i;
            double d;
            if (YAML::convert<double>::decode(node, d)) return d;
            return node.as<std::string>();
        }
        case YAML::NodeType::Null:
        default:
            return nullptr;
    }
}

static AgentConfig from_node(const YAML::Node& root, const std::string& source) {
    if (!root["name"])
        throw std::runtime_error("Agent YAML missing required field 'name' in: " + source);

    AgentConfig cfg;
    cfg.name          = root["name"].as<std::string>();
    cfg.description   = root["description"]   ? root["description"].as<std::string>()   : "";
    cfg.model         = root["model"]         ? root["model"].as<std::string>()         : "default";
    cfg.llm_url       = root["llm_url"]       ? root["llm_url"].as<std::string>()       : "";
    cfg.llm_api_key   = root["llm_api_key"]   ? root["llm_api_key"].as<std::string>()   : "";
    cfg.llm_provider  = root["llm_provider"]  ? root["llm_provider"].as<std::string>()  : "";
    cfg.system_prompt = root["system_prompt"] ? root["system_prompt"].as<std::string>() : "";
    cfg.tool_choice   = root["tool_choice"]   ? root["tool_choice"].as<std::string>()   : "auto";
    cfg.workspace_dir = root["workspace_dir"] ? root["workspace_dir"].as<std::string>() : "";

    if (root["context_limit"])
        cfg.context_limit = root["context_limit"].as<int>();
    if (root["max_steps"])
        cfg.max_steps = root["max_steps"].as<int>();

    if (root["tools"] && root["tools"].IsSequence()) {
        for (const auto& t : root["tools"])
            cfg.tools.push_back(t.as<std::string>());
    }

    if (root["require_tools"] && root["require_tools"].IsSequence()) {
        for (const auto& t : root["require_tools"])
            cfg.require_tools.push_back(t.as<std::string>());
    }

    if (root["answer_schema"] && root["answer_schema"].IsMap()) {
        cfg.answer_schema = yaml_to_json(root["answer_schema"]);
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
                if (entry["url"])  srv.url  = entry["url"].as<std::string>();
                if (entry["name"]) srv.name = entry["name"].as<std::string>();
                if (srv.name.empty()) srv.name = srv.url;
            }
            if (!srv.url.empty())
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
