// =============================================================================
// tests/test_agent_config.cpp — AgentConfig YAML parsing unit tests
// =============================================================================

#include "agent_config.h"
#include <iostream>

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::cerr << "FAILED at " << __FILE__ << ":" << __LINE__ << " — " #cond "\n"; \
        return 1; \
    } \
} while (0)

int main() {
    // Full config.
    AgentConfig cfg = AgentConfig::from_string(R"yaml(
name: tester
description: A test agent
model: some-model
tool_choice: required
tools: [web_search, recall]
require_tools: [write_file, execute_shell]
max_steps: 3
context_limit: 2048
system_prompt: |
  Prompt line.
mcp_servers:
  - url: http://localhost:9000
    name: srv-a
  - http://localhost:9001
)yaml");

    CHECK(cfg.name == "tester");
    CHECK(cfg.description == "A test agent");
    CHECK(cfg.model == "some-model");
    CHECK(cfg.tool_choice == "required");
    CHECK(cfg.tools.size() == 2 && cfg.tools[0] == "web_search");
    CHECK(cfg.require_tools.size() == 2 && cfg.require_tools[1] == "execute_shell");
    CHECK(cfg.max_steps == 3);
    CHECK(cfg.context_limit == 2048);
    CHECK(cfg.system_prompt.find("Prompt line.") != std::string::npos);
    CHECK(cfg.mcp_servers.size() == 2);
    CHECK(cfg.mcp_servers[0].name == "srv-a");
    CHECK(cfg.mcp_servers[1].url == "http://localhost:9001");
    CHECK(cfg.mcp_servers[1].name == "http://localhost:9001");  // bare URL shorthand

    // Defaults.
    AgentConfig min = AgentConfig::from_string("name: minimal");
    CHECK(min.model == "default");
    CHECK(min.tool_choice == "auto");
    CHECK(min.tools.empty());
    CHECK(min.require_tools.empty());   // no contract unless asked for
    CHECK(min.answer_schema.is_null());  // ...nor an answer schema
    CHECK(min.max_steps == 8);

    // answer_schema: written as ordinary YAML, converted to JSON. The types
    // have to survive — a schema whose minItems is the string "1" enforces
    // nothing.
    AgentConfig typed = AgentConfig::from_string(R"yaml(
name: typed
system_prompt: Answer the question.
answer_schema:
  type: object
  required: [summary, sources]
  properties:
    summary: { type: string }
    sources:
      type: array
      items: { type: string }
      minItems: 1
)yaml");
    CHECK(!typed.answer_schema.is_null());
    CHECK(typed.answer_schema["type"] == "object");
    CHECK(typed.answer_schema["required"].size() == 2);
    CHECK(typed.answer_schema["required"][0] == "summary");
    CHECK(typed.answer_schema["properties"]["sources"]["minItems"] == 1);
    CHECK(typed.answer_schema["properties"]["sources"]["minItems"].is_number());
    CHECK(typed.answer_schema["properties"]["summary"]["type"] == "string");

    // The prompt half of the contract is generated at load, so an agent author
    // can't leave the prompt saying one thing while the loop enforces another.
    CHECK(typed.system_prompt.find("Answer the question.") != std::string::npos);
    CHECK(typed.system_prompt.find("sources") != std::string::npos);
    CHECK(typed.system_prompt.find("JSON") != std::string::npos);

    // Missing name → throws.
    bool threw = false;
    try { AgentConfig::from_string("description: nameless"); }
    catch (const std::exception&) { threw = true; }
    CHECK(threw);

    // Invalid YAML → throws.
    threw = false;
    try { AgentConfig::from_string(": : :"); }
    catch (const std::exception&) { threw = true; }
    CHECK(threw);

    std::cout << "test_agent_config: all tests passed\n";
    return 0;
}
