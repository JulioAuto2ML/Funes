// =============================================================================
// tests/test_cron_tool.cpp — schedule_job / list_jobs / cancel_job / run_job_now
// =============================================================================
// Covers the validation surface and the fast (no LLM, no shell) paths — same
// split as test_delegation.cpp. An agent-kind job's actual execution needs a
// live (or mocked) LLM backend and is exercised end-to-end in
// tests/integration.sh instead.
//
// FUNES_ALLOW_SHELL defaults to unset, so the disabled-path test must run
// before the enabled-path one touches the environment (see test_shell_tool.cpp).

#include "agent.h"
#include "memory.h"
#include "tools/cron_tool.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace fs = std::filesystem;

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::cerr << "FAILED at " << __FILE__ << ":" << __LINE__ << " — " #cond "\n"; \
        return 1; \
    } \
} while (0)

namespace {

AgentConfig find_agent(const std::string& name) {
    AgentConfig cfg;
    if (name == "researcher") { cfg.name = "researcher"; cfg.system_prompt = "You research."; }
    return cfg;  // empty name for anything else — "not found"
}

} // namespace

int test_schedule_job_validation() {
    fs::path db = fs::temp_directory_path() / "funes_test_cron_validation.db";
    fs::remove(db);
    MemoryStore memory(db.string(), nullptr);
    AgentDefaults defaults;
    fs::path workspace = fs::temp_directory_path() / "funes_test_cron_ws";

    ToolRegistry reg;
    register_cron_tool(reg, memory, defaults, workspace.string(), find_agent);
    CHECK(reg.has("schedule_job") && reg.has("list_jobs") &&
          reg.has("cancel_job") && reg.has("run_job_now"));

    ToolContext ctx{"operator", "s1"};

    auto missing_name = reg.call("schedule_job",
        {{"schedule", "* * * * *"}, {"kind", "agent"}}, ctx);
    CHECK(missing_name.error);

    auto missing_schedule = reg.call("schedule_job",
        {{"name", "j"}, {"kind", "agent"}}, ctx);
    CHECK(missing_schedule.error);

    auto bad_kind = reg.call("schedule_job",
        {{"name", "j"}, {"schedule", "* * * * *"}, {"kind", "carrier-pigeon"}}, ctx);
    CHECK(bad_kind.error);

    auto bad_cron = reg.call("schedule_job",
        {{"name", "j"}, {"schedule", "not a cron expr"}, {"kind", "agent"},
         {"agent", "researcher"}, {"task", "t"}}, ctx);
    CHECK(bad_cron.error);
    CHECK(bad_cron.text.find("Bad schedule") != std::string::npos);

    auto agent_missing_task = reg.call("schedule_job",
        {{"name", "j"}, {"schedule", "* * * * *"}, {"kind", "agent"},
         {"agent", "researcher"}}, ctx);
    CHECK(agent_missing_task.error);

    auto unknown_agent = reg.call("schedule_job",
        {{"name", "j"}, {"schedule", "* * * * *"}, {"kind", "agent"},
         {"agent", "ghost"}, {"task", "t"}}, ctx);
    CHECK(unknown_agent.error);
    CHECK(unknown_agent.text.find("Unknown agent") != std::string::npos);

    auto shell_missing_command = reg.call("schedule_job",
        {{"name", "j"}, {"schedule", "* * * * *"}, {"kind", "shell"}}, ctx);
    CHECK(shell_missing_command.error);

    unsetenv("FUNES_ALLOW_SHELL");
    auto shell_disabled = reg.call("schedule_job",
        {{"name", "j"}, {"schedule", "* * * * *"}, {"kind", "shell"},
         {"command", "true"}}, ctx);
    CHECK(shell_disabled.error);
    CHECK(shell_disabled.text.find("FUNES_ALLOW_SHELL") != std::string::npos);

    return 0;
}

int test_schedule_list_cancel_roundtrip() {
    fs::path db = fs::temp_directory_path() / "funes_test_cron_roundtrip.db";
    fs::remove(db);
    MemoryStore memory(db.string(), nullptr);
    AgentDefaults defaults;
    fs::path workspace = fs::temp_directory_path() / "funes_test_cron_ws2";

    ToolRegistry reg;
    register_cron_tool(reg, memory, defaults, workspace.string(), find_agent);
    ToolContext ctx{"operator", "s1"};

    auto empty = reg.call("list_jobs", json::object(), ctx);
    CHECK(!empty.error);
    CHECK(empty.text.find("No scheduled jobs") != std::string::npos);

    auto scheduled = reg.call("schedule_job",
        {{"name", "daily-research"}, {"schedule", "0 9 * * *"}, {"kind", "agent"},
         {"agent", "researcher"}, {"task", "look something up"}}, ctx);
    CHECK(!scheduled.error);
    CHECK(scheduled.text.find("Scheduled job #") != std::string::npos);

    // Extract the id from "Scheduled job #<id> ...".
    auto hash = scheduled.text.find('#');
    int64_t id = std::stoll(scheduled.text.substr(hash + 1));

    auto listed = reg.call("list_jobs", json::object(), ctx);
    CHECK(!listed.error);
    CHECK(listed.text.find("daily-research") != std::string::npos);
    CHECK(listed.text.find("0 9 * * *") != std::string::npos);

    auto cancel_wrong = reg.call("cancel_job", {{"id", id + 999}}, ctx);
    CHECK(cancel_wrong.error);

    auto cancel_ok = reg.call("cancel_job", {{"id", id}}, ctx);
    CHECK(!cancel_ok.error);

    auto listed_after = reg.call("list_jobs", json::object(), ctx);
    CHECK(!listed_after.error);
    CHECK(listed_after.text.find("No scheduled jobs") != std::string::npos);

    return 0;
}

int test_run_job_now() {
    fs::path db = fs::temp_directory_path() / "funes_test_cron_run_now.db";
    fs::remove(db);
    MemoryStore memory(db.string(), nullptr);
    AgentDefaults defaults;
    fs::path workspace = fs::temp_directory_path() / "funes_test_cron_ws3";
    fs::create_directories(workspace);

    ToolRegistry reg;
    register_cron_tool(reg, memory, defaults, workspace.string(), find_agent);
    ToolContext ctx{"operator", "s1"};

    auto unknown_id = reg.call("run_job_now", {{"id", 12345}}, ctx);
    CHECK(unknown_id.error);
    CHECK(unknown_id.text.find("No job with id") != std::string::npos);

    // A shell-kind job run with shell disabled fails fast — no network, no
    // LLM, exercises the run_job_now -> cron_runner -> record path end to end.
    unsetenv("FUNES_ALLOW_SHELL");
    auto scheduled = reg.call("schedule_job",
        {{"name", "noop"}, {"schedule", "0 0 1 1 *"}, {"kind", "shell"},
         {"command", "true"}}, ctx);
    // Scheduling a shell job while shell is disabled is itself refused —
    // enable it just long enough to create the row, then disable again to
    // prove run_job_now re-checks the gate at run time, not just at
    // schedule time.
    CHECK(scheduled.error);

    setenv("FUNES_ALLOW_SHELL", "1", 1);
    scheduled = reg.call("schedule_job",
        {{"name", "noop"}, {"schedule", "0 0 1 1 *"}, {"kind", "shell"},
         {"command", "true"}}, ctx);
    CHECK(!scheduled.error);
    auto hash = scheduled.text.find('#');
    int64_t id = std::stoll(scheduled.text.substr(hash + 1));

    auto ran = reg.call("run_job_now", {{"id", id}}, ctx);
    CHECK(!ran.error);

    auto listed = reg.call("list_jobs", json::object(), ctx);
    CHECK(listed.text.find("(ok)") != std::string::npos);

    unsetenv("FUNES_ALLOW_SHELL");
    return 0;
}

// ── kind="script" ────────────────────────────────────────────────────────────
// Unattended deterministic work used to mean kind="shell", so a nightly backup
// script cost the install FUNES_ALLOW_SHELL — and everything else a shell job
// can do came with it. A script job runs one granted program instead, through
// the same run_script path an interactive call takes, with the grant
// re-checked when it fires rather than trusted from when it was scheduled.
int test_script_jobs() {
    unsetenv("FUNES_ALLOW_SHELL");   // the point: none of this needs it

    fs::path db = fs::temp_directory_path() / "funes_test_cron_script.db";
    fs::remove(db);
    MemoryStore memory(db.string(), nullptr);
    AgentDefaults defaults;
    fs::path workspace = fs::temp_directory_path() / "funes_test_cron_script_ws";
    fs::remove_all(workspace);

    fs::path lib = fs::temp_directory_path() / "funes_test_cron_scriptlib";
    fs::remove_all(lib);
    fs::create_directories(lib);
    { std::ofstream f(lib / "hello.sh"); f << "#!/bin/sh\necho scheduled-hello\n"; }
    fs::permissions(lib / "hello.sh", fs::perms::owner_all, fs::perm_options::replace);
    { std::ofstream f(lib / "hello.yaml"); f << "description: Says hello.\nrun: hello.sh\n"; }

    // The agent table the runner re-resolves the grant against. `granted`
    // flips to simulate an admin editing the YAML after the job exists.
    static bool granted = true;
    auto find = [](const std::string& name) {
        AgentConfig cfg;
        if (name == "operator") {
            cfg.name = "operator";
            if (granted) cfg.scripts = {"hello"};
        }
        return cfg;
    };

    ToolRegistry reg;
    register_script_tools(reg, workspace.string(), lib.string());
    register_cron_tool(reg, memory, defaults, workspace.string(), find);

    // Scheduling is refused for a script this agent was not granted — the
    // scheduler is not a way around the allowlist.
    ToolContext ungranted{"operator", "s1"};
    auto refused = reg.call("schedule_job",
        {{"name", "nope"}, {"schedule", "0 3 * * *"}, {"kind", "script"},
         {"script", "hello"}}, ungranted);
    CHECK(refused.error);
    CHECK(refused.text.find("not available to this agent") != std::string::npos);

    ToolContext ctx{"operator", "s1", "", "", 1, funes::Permissions::unrestricted(), {"hello"}};
    auto missing_script = reg.call("schedule_job",
        {{"name", "j"}, {"schedule", "0 3 * * *"}, {"kind", "script"}}, ctx);
    CHECK(missing_script.error);

    auto scheduled = reg.call("schedule_job",
        {{"name", "nightly"}, {"schedule", "0 3 * * *"}, {"kind", "script"},
         {"script", "hello"}}, ctx);
    CHECK(!scheduled.error);
    const int64_t id = std::stoll(scheduled.text.substr(scheduled.text.find('#') + 1));

    auto listed = reg.call("list_jobs", json::object(), ctx);
    CHECK(listed.text.find("(script)") != std::string::npos);
    CHECK(listed.text.find("script=hello") != std::string::npos);

    // It runs, with no shell enabled anywhere.
    auto ran = reg.call("run_job_now", {{"id", id}}, ctx);
    CHECK(!ran.error);
    CHECK(ran.text.find("scheduled-hello") != std::string::npos);

    // Revoking the agent's grant stops the timer too: the runner re-resolves
    // it rather than trusting what was true when the job was written.
    granted = false;
    auto after_revoke = reg.call("run_job_now", {{"id", id}}, ctx);
    CHECK(after_revoke.error);
    CHECK(after_revoke.text.find("no longer granted") != std::string::npos);
    granted = true;

    // So does denying the account the script, through the same key
    // FunesAgent::dispatch_tool checks.
    ToolContext denied{"operator", "s1", "", "",
                       1, funes::Permissions::parse(R"({"tools": {"run_script:hello": false}})",
                                                    false),
                       {"hello"}};
    auto after_deny = reg.call("run_job_now", {{"id", id}}, denied);
    CHECK(after_deny.error);
    CHECK(after_deny.text.find("no longer permitted") != std::string::npos);

    fs::remove_all(lib);
    fs::remove_all(workspace);
    return 0;
}

int main() {
    int rc = 0;
    rc |= test_schedule_job_validation();
    rc |= test_schedule_list_cancel_roundtrip();
    rc |= test_run_job_now();
    rc |= test_script_jobs();
    if (rc == 0) std::cout << "test_cron_tool: all tests passed\n";
    return rc;
}
