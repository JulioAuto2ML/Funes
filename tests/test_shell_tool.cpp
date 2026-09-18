// =============================================================================
// tests/test_shell_tool.cpp — execute_shell
// =============================================================================
// FUNES_ALLOW_SHELL defaults to unset, so the disabled-path test must run
// first and confirm the tool refuses to run anything before we opt in for
// the rest of the process's lifetime.

#include "tools.h"
#include "tools/process_runner.h"
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

    // cwd is the caller's workspace, which is now <root>/<user_id>.
    auto write = reg.call("execute_shell", {{"command", "echo marker > seen.txt"}}, ctx);
    CHECK(!write.error);
    CHECK(fs::exists(ws / "1" / "seen.txt"));

    // Timeout: process outlives its budget and gets killed.
    auto timeout = reg.call("execute_shell",
        {{"command", "sleep 5"}, {"timeout_seconds", 1}}, ctx);
    CHECK(timeout.error);
    CHECK(timeout.text.find("TIMED OUT") != std::string::npos);

    fs::remove_all(ws);
    unsetenv("FUNES_ALLOW_SHELL");
    return 0;
}

// The child's environment is an allowlist, not this process's block. Before
// 5.0.1 `env` printed FUNES_SERVICE_TOKEN and every key from funes.local into
// the transcript; a script or MCP server got the same. Regression for that,
// and for the one escape hatch (FUNES_CHILD_ENV) and the manifest-style
// extra_env override, both through proc::child_environment directly.
int test_child_environment_is_an_allowlist() {
    setenv("FUNES_ALLOW_SHELL", "1", 1);
    setenv("FUNES_SERVICE_TOKEN", "super-secret-token", 1);
    setenv("SOME_IMAP_PASSWORD", "hunter2", 1);
    setenv("MY_TOOL_KEY", "pass-me-through", 1);
    unsetenv("FUNES_CHILD_ENV");

    fs::path ws = fs::temp_directory_path() / "funes_test_shell_ws_env";
    fs::remove_all(ws);
    fs::create_directories(ws);
    ToolRegistry reg;
    register_shell_tool(reg, ws.string());
    ToolContext ctx{"funes", "s1"};

    auto r = reg.call("execute_shell", {{"command", "env"}}, ctx);
    CHECK(!r.error);
    CHECK(r.text.find("super-secret-token") == std::string::npos);
    CHECK(r.text.find("FUNES_SERVICE_TOKEN") == std::string::npos);
    CHECK(r.text.find("FUNES_ALLOW_SHELL") == std::string::npos);
    CHECK(r.text.find("hunter2") == std::string::npos);
    CHECK(r.text.find("MY_TOOL_KEY") == std::string::npos);
    CHECK(r.text.find("PATH=") != std::string::npos);   // still a usable process

    // The operator can name what passes.
    setenv("FUNES_CHILD_ENV", "MY_TOOL_KEY, OTHER", 1);
    auto env = funes::proc::child_environment();
    bool saw_key = false, saw_token = false;
    for (const auto& e : env) {
        if (e == "MY_TOOL_KEY=pass-me-through") saw_key = true;
        if (e.rfind("FUNES_SERVICE_TOKEN=", 0) == 0) saw_token = true;
    }
    CHECK(saw_key);
    CHECK(!saw_token);
    unsetenv("FUNES_CHILD_ENV");

    // extra_env (a script manifest's env:) is added and overrides a passed one.
    setenv("TZ", "UTC", 1);
    auto with_extra = funes::proc::child_environment({{"TZ", "Etc/GMT+3"}, {"API_KEY", "k"}});
    int tz_count = 0; bool tz_override = false, api = false;
    for (const auto& e : with_extra) {
        if (e.rfind("TZ=", 0) == 0) { ++tz_count; tz_override = (e == "TZ=Etc/GMT+3"); }
        if (e == "API_KEY=k") api = true;
    }
    CHECK(tz_count == 1);
    CHECK(tz_override);
    CHECK(api);

    fs::remove_all(ws);
    unsetenv("FUNES_ALLOW_SHELL");
    unsetenv("FUNES_SERVICE_TOKEN");
    unsetenv("SOME_IMAP_PASSWORD");
    unsetenv("MY_TOOL_KEY");
    return 0;
}

int main() {
    int rc = 0;
    rc |= test_disabled_by_default();
    rc |= test_enabled_execution();
    rc |= test_child_environment_is_an_allowlist();
    if (rc == 0) std::cout << "test_shell_tool: all tests passed\n";
    return rc;
}
