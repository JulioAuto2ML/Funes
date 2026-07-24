// =============================================================================
// tests/test_delegation.cpp — delegate_to_agent validation
// =============================================================================
// Covers the validation surface that doesn't need a live LLM backend
// (missing args, self-delegation, unknown agent). The full success path —
// including that persist=false keeps a delegated sub-call out of session
// history — is exercised end-to-end in tests/integration.sh against the
// mock LLM, since it requires actually running a sub-agent's tool loop.

#include "agent.h"
#include "tools.h"
#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::cerr << "FAILED at " << __FILE__ << ":" << __LINE__ << " — " #cond "\n"; \
        return 1; \
    } \
} while (0)

int test_validation() {
    fs::path db = fs::temp_directory_path() / "funes_test_delegation.db";
    fs::remove(db);
    MemoryStore memory(db.string(), nullptr);
    AgentDefaults defaults;

    auto find_agent = [](const std::string& name) -> AgentConfig {
        AgentConfig cfg;
        if (name == "researcher") { cfg.name = "researcher"; cfg.system_prompt = "You research."; }
        return cfg;  // empty name for anything else — "not found"
    };
    auto list_agent_names = [] { return std::vector<std::string>{"funes", "researcher"}; };

    ToolRegistry reg;
    register_delegation_tool(reg, memory, defaults, find_agent, list_agent_names);
    CHECK(reg.has("delegate_to_agent"));

    ToolContext ctx{"funes", "s1"};

    auto missing_agent = reg.call("delegate_to_agent", {{"task", "do something"}}, ctx);
    CHECK(missing_agent.error);

    auto missing_task = reg.call("delegate_to_agent", {{"agent", "researcher"}}, ctx);
    CHECK(missing_task.error);

    auto self_delegate = reg.call("delegate_to_agent",
        {{"agent", "funes"}, {"task", "do something"}}, ctx);
    CHECK(self_delegate.error);
    CHECK(self_delegate.text.find("yourself") != std::string::npos);

    auto unknown = reg.call("delegate_to_agent",
        {{"agent", "ghost"}, {"task", "do something"}}, ctx);
    CHECK(unknown.error);
    CHECK(unknown.text.find("Unknown agent") != std::string::npos);
    CHECK(unknown.text.find("researcher") != std::string::npos);  // lists available agents

    return 0;
}

int main() {
    int rc = 0;
    rc |= test_validation();
    if (rc == 0) std::cout << "test_delegation: all tests passed\n";
    return rc;
}
