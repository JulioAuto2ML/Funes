// =============================================================================
// tests/test_shell_tool.cpp — execute_shell
// =============================================================================
// FUNES_ALLOW_SHELL defaults to unset, so the disabled-path test must run
// first and confirm the tool refuses to run anything before we opt in for
// the rest of the process's lifetime.

#include "tools.h"
#include <cstdlib>
#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::cerr << "FAILED at " << __FILE__ << ":" << __LINE__ << " — " #cond "\n"; \
        return 1; \
    } \
} while (0)

int test_disabled_by_default() {
    unsetenv("FUNES_ALLOW_SHELL");
    fs::path ws = fs::temp_directory_path() / "funes_test_shell_ws_disabled";
    fs::remove_all(ws);

    ToolRegistry reg;
    register_shell_tool(reg, ws.string());
    ToolContext ctx{"funes", "s1"};

    auto r = reg.call("execute_shell", {{"command", "echo should-not-run"}}, ctx);
    CHECK(r.error);
    CHECK(r.text.find("FUNES_ALLOW_SHELL") != std::string::npos);

    fs::remove_all(ws);
    return 0;
}

int test_enabled_execution() {
    setenv("FUNES_ALLOW_SHELL", "1", 1);
    fs::path ws = fs::temp_directory_path() / "funes_test_shell_ws_enabled";
    fs::remove_all(ws);
    fs::create_directories(ws);

    ToolRegistry reg;
    register_shell_tool(reg, ws.string());
    ToolContext ctx{"funes", "s1"};

    auto missing = reg.call("execute_shell", {}, ctx);
    CHECK(missing.error);

    auto ok = reg.call("execute_shell", {{"command", "echo hello-from-shell"}}, ctx);
    CHECK(!ok.error);
    CHECK(ok.text.find("exit_code: 0") != std::string::npos);
    CHECK(ok.text.find("hello-from-shell") != std::string::npos);

    auto fails = reg.call("execute_shell", {{"command", "exit 3"}}, ctx);
    CHECK(fails.error);
    CHECK(fails.text.find("exit_code: 3") != std::string::npos);

    // cwd is the workspace: a file written there should be visible.
    auto write = reg.call("execute_shell", {{"command", "echo marker > seen.txt"}}, ctx);
    CHECK(!write.error);
    CHECK(fs::exists(ws / "seen.txt"));

    // Timeout: process outlives its budget and gets killed.
    auto timeout = reg.call("execute_shell",
        {{"command", "sleep 5"}, {"timeout_seconds", 1}}, ctx);
    CHECK(timeout.error);
    CHECK(timeout.text.find("TIMED OUT") != std::string::npos);

    fs::remove_all(ws);
    unsetenv("FUNES_ALLOW_SHELL");
    return 0;
}

int main() {
    int rc = 0;
    rc |= test_disabled_by_default();
    rc |= test_enabled_execution();
    if (rc == 0) std::cout << "test_shell_tool: all tests passed\n";
    return rc;
}
