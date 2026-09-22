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
temperature: 0.0
tools: [web_search, recall]
require_tools: [write_file, execute_shell]
answer_from_tool: write_file
max_steps: 3
context_limit: 2048
system_prompt: |
  Prompt line.
mcp_servers:
  - url: http://localhost:9000
    name: srv-a
  - http://localhost:9001
  - command: npx -y rss-reader-mcp
    name: rss
    env:
      FOO: bar
memory_scope: funes
)yaml");

    CHECK(cfg.name == "tester");
    CHECK(cfg.description == "A test agent");
    CHECK(cfg.model == "some-model");
    CHECK(cfg.tool_choice == "required");
    CHECK(cfg.temperature == 0.0f);
    CHECK(cfg.tools.size() == 2 && cfg.tools[0] == "web_search");
    CHECK(cfg.require_tools.size() == 2 && cfg.require_tools[1] == "execute_shell");
    CHECK(cfg.answer_from_tool == "write_file");
    CHECK(cfg.max_steps == 3);
    CHECK(cfg.context_limit == 2048);
    CHECK(cfg.system_prompt.find("Prompt line.") != std::string::npos);
    CHECK(cfg.mcp_servers.size() == 3);
    CHECK(cfg.mcp_servers[0].name == "srv-a");
    CHECK(cfg.mcp_servers[1].url == "http://localhost:9001");
    CHECK(cfg.mcp_servers[1].name == "http://localhost:9001");  // bare URL shorthand
    CHECK(cfg.mcp_servers[2].command == "npx -y rss-reader-mcp");
    CHECK(cfg.mcp_servers[2].name == "rss");
    CHECK(cfg.mcp_servers[2].url.empty());
    CHECK(cfg.mcp_servers[2].env.at("FOO") == "bar");
    CHECK(cfg.memory_scope == "funes");  // explicit override

    // Defaults.
    AgentConfig min = AgentConfig::from_string("name: minimal");
    CHECK(min.model == "default");
    CHECK(min.tool_choice == "auto");
    CHECK(min.temperature < 0.0f);  // unset — inherits LLMClient's own default
    CHECK(min.tools.empty());
    CHECK(min.require_tools.empty());   // no contract unless asked for
    CHECK(min.answer_from_tool.empty());  // ...nor a tool-result shortcut
    CHECK(min.answer_schema.is_null());  // ...nor an answer schema
    CHECK(min.max_steps == 8);
    CHECK(min.memory_scope == "minimal");  // unset → own name, i.e. isolated by default

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

    // Env-var expansion in LLM credential fields.
    setenv("TEST_FUNES_API_KEY", "sk-test-123", 1);
    setenv("TEST_FUNES_URL", "https://api.example.com", 1);
    AgentConfig env_cfg = AgentConfig::from_string(R"yaml(
name: env-test
llm_api_key: ${TEST_FUNES_API_KEY}
llm_url: ${TEST_FUNES_URL}
llm_provider: ${TEST_FUNES_MISSING_VAR}
)yaml");
    CHECK(env_cfg.llm_api_key == "sk-test-123");
    CHECK(env_cfg.llm_url == "https://api.example.com");
    CHECK(env_cfg.llm_provider.empty());  // unset var → empty string
    unsetenv("TEST_FUNES_API_KEY");
    unsetenv("TEST_FUNES_URL");

    // Env-var expansion with surrounding text.
    setenv("TEST_FUNES_HOST", "gpu-box", 1);
    AgentConfig env_mixed = AgentConfig::from_string(R"yaml(
name: env-mixed
llm_url: http://${TEST_FUNES_HOST}:8080/v1
)yaml");
    CHECK(env_mixed.llm_url == "http://gpu-box:8080/v1");
    unsetenv("TEST_FUNES_HOST");

    // Literal ${...} without a closing brace is kept as-is.
    AgentConfig env_broken = AgentConfig::from_string(R"yaml(
name: env-broken
llm_api_key: "${unclosed"
)yaml");
    CHECK(env_broken.llm_api_key == "${unclosed");

    // An MCP server's command and env both expand ${VAR}. The command needs
    // it as much as the env does: a stdio server's path differs per machine
    // (dev checkout vs. the deployment host), and without expansion the only
    // way to say so is an absolute path committed to the repo, which the
    // other machine then has to edit by hand on every pull.
    setenv("TEST_MCP_DIR", "/opt/servers", 1);
    setenv("TEST_MCP_SECRET", "s3cret", 1);
    AgentConfig mcp_env = AgentConfig::from_string(R"yaml(
name: mcp-env
mcp_servers:
  - command: node ${TEST_MCP_DIR}/imap/index.js
    name: imap
    env:
      IMAP_PASSWORD: ${TEST_MCP_SECRET}
  - command: ${TEST_MCP_UNSET}/bin/thing
    name: unset
)yaml");
    CHECK(mcp_env.mcp_servers.size() == 2);
    CHECK(mcp_env.mcp_servers[0].command == "node /opt/servers/imap/index.js");
    CHECK(mcp_env.mcp_servers[0].env.at("IMAP_PASSWORD") == "s3cret");
    // An unset variable expands to nothing, as everywhere else.
    CHECK(mcp_env.mcp_servers[1].command == "/bin/thing");
    unsetenv("TEST_MCP_DIR");
    unsetenv("TEST_MCP_SECRET");

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
