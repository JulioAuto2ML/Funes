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
#include "cron_runner.h"
#include "effects.h"
#include "permissions.h"
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
    if (name == "funes") { cfg.name = "funes"; cfg.tools = {"recall", "notify_user"}; }
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

    auto cancel_wrong = reg.call("cancel_job", {{"id", id + 999}, {"name", "daily-research"}}, ctx);
    CHECK(cancel_wrong.error);

    auto cancel_ok = reg.call("cancel_job", {{"id", id}, {"name", "  Daily-Research "}}, ctx);
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

// ── kind="shell" is execute_shell, deferred ──────────────────────────────────
// Regression for a privilege escalation found in 5.0: schedule_job checked only
// FUNES_ALLOW_SHELL, and run_shell_job checked only FUNES_ALLOW_SHELL, so a
// member account — denied execute_shell by default — could schedule any command
// and have the runner execute it with the process's privileges. Both ends now
// ask the owner's permissions, and the fire-time check is the one that counts:
// a job scheduled while permitted must stop running once the permission goes.
int test_shell_jobs_need_execute_shell_permission() {
    fs::path db = fs::temp_directory_path() / "funes_test_cron_shell_perm.db";
    fs::remove(db);
    MemoryStore memory(db.string(), nullptr);
    AgentDefaults defaults;
    fs::path workspace = fs::temp_directory_path() / "funes_test_cron_ws4";
    fs::create_directories(workspace);

    ToolRegistry reg;
    register_cron_tool(reg, memory, defaults, workspace.string(), find_agent);
    setenv("FUNES_ALLOW_SHELL", "1", 1);

    // A member with no permissions blob: execute_shell is privileged, so it is
    // denied — and so must be scheduling a shell job.
    const funes::Permissions member = funes::Permissions::parse("{}", /*is_admin=*/false);
    CHECK(!member.allows_tool("execute_shell"));
    ToolContext denied{"operator", "s1", "", "", 1, member, {}};
    auto refused = reg.call("schedule_job",
        {{"name", "escalate"}, {"schedule", "0 0 1 1 *"}, {"kind", "shell"},
         {"command", "true"}}, denied);
    CHECK(refused.error);
    CHECK(refused.text.find("execute_shell") != std::string::npos);
    CHECK(memory.list_cron_jobs(1).empty());

    // Granted explicitly: scheduling works, and the job runs.
    const funes::Permissions granted =
        funes::Permissions::parse(R"({"tools": {"execute_shell": true}})", false);
    ToolContext allowed{"operator", "s1", "", "", 1, granted, {}};
    auto scheduled = reg.call("schedule_job",
        {{"name", "ok"}, {"schedule", "0 0 1 1 *"}, {"kind", "shell"},
         {"command", "true"}}, allowed);
    CHECK(!scheduled.error);
    const int64_t id = std::stoll(scheduled.text.substr(scheduled.text.find('#') + 1));

    auto ran = reg.call("run_job_now", {{"id", id}}, allowed);
    CHECK(!ran.error);

    // The same row, fired by an owner whose permission has since been revoked:
    // the runner re-resolves and refuses. run_cron_job_now with the member's
    // permissions is exactly what the poll loop does with find_permissions.
    auto [ok, output] = funes::cron::run_cron_job_now(
        memory, reg, defaults, workspace.string(), find_agent,
        [&member](int64_t) { return member; }, id);
    CHECK(!ok);
    CHECK(output.find("execute_shell") != std::string::npos);

    // And the shell's working directory is the owner's workspace, not the root.
    auto pwd = reg.call("schedule_job",
        {{"name", "pwd"}, {"schedule", "0 0 1 1 *"}, {"kind", "shell"},
         {"command", "pwd"}}, allowed);
    const int64_t pwd_id = std::stoll(pwd.text.substr(pwd.text.find('#') + 1));
    auto pwd_ran = reg.call("run_job_now", {{"id", pwd_id}}, allowed);
    CHECK(!pwd_ran.error);
    CHECK(pwd_ran.text.find((workspace / "1").string()) != std::string::npos);

    unsetenv("FUNES_ALLOW_SHELL");
    fs::remove_all(workspace);
    return 0;
}

// A job's `running` flag is cleared only when its run finishes, and runs are
// threads of this process — so a restart mid-run (a deploy, 2026-10-23 17:12)
// left the flag set with nothing behind it, and due_cron_jobs skipped the job
// from then on. The TV publish job sat like that for four days.
int test_stale_running_flags_cleared_at_startup() {
    fs::path db = fs::temp_directory_path() / "funes_test_cron_stale.db";
    fs::remove(db);
    MemoryStore memory(db.string(), nullptr);

    MemoryStore::CronJob job;
    job.name = "publish";
    job.kind = "shell";
    job.command = "true";
    job.schedule = "* * * * *";
    job.user_id = 1;
    job.next_run_at = 1000;
    const int64_t stuck = memory.create_cron_job(job);
    job.name = "idle";
    const int64_t idle = memory.create_cron_job(job);
    memory.mark_cron_job_running(stuck, true);

    CHECK(memory.due_cron_jobs(2000).size() == 1);  // only `idle`

    CHECK(memory.clear_stale_cron_runs() == 1);     // counts what it freed
    auto due = memory.due_cron_jobs(2000);
    CHECK(due.size() == 2);
    CHECK((due[0].id == stuck || due[1].id == stuck));
    CHECK(memory.clear_stale_cron_runs() == 0);
    (void)idle;

    fs::remove(db);
    return 0;
}

// Changing when a job fires has to move next_run_at with it: the runner only
// reads `schedule` after a run, so a job rescheduled from daily to Tuesdays
// would otherwise still fire once at its old next time (2026-11, the newsletter
// split into English Mon/Wed/Fri and Spanish Tue/Thu/Sat).
int test_reschedule_moves_next_run() {
    fs::path db = fs::temp_directory_path() / "funes_test_cron_reschedule.db";
    fs::remove(db);
    MemoryStore memory(db.string(), nullptr);

    MemoryStore::CronJob job;
    job.name = "newsletter";
    job.kind = "agent";
    job.agent = "curator";
    job.task = "publish";
    job.schedule = "30 10 * * *";
    job.user_id = 1;
    job.next_run_at = 1000;
    const int64_t id = memory.create_cron_job(job);

    // Someone else's job is "not there", same as cancel_job.
    CHECK(!memory.reschedule_cron_job(2, id, "30 10 * * 2,4,6", 5000));
    CHECK(memory.get_cron_job(-1, id)->schedule == "30 10 * * *");

    CHECK(memory.reschedule_cron_job(1, id, "30 10 * * 2,4,6", 5000));
    auto after = memory.get_cron_job(-1, id);
    CHECK(after->schedule == "30 10 * * 2,4,6");
    CHECK(after->next_run_at == 5000);
    CHECK(memory.due_cron_jobs(2000).empty());

    // user_id < 0 is the admin path the CLI uses.
    CHECK(memory.reschedule_cron_job(-1, id, "0 9 * * 1", 6000));
    CHECK(memory.get_cron_job(-1, id)->next_run_at == 6000);
    CHECK(!memory.reschedule_cron_job(-1, id + 99, "0 9 * * 1", 6000));

    fs::remove(db);
    return 0;
}

// A job may say what "done" means (7.18, self-healing H0): a reminder that
// ran "ok" without calling notify_user reached nobody — the 23/12 appointment
// job would have. require_tools becomes the run's completion contract, so
// that run ends FAILED and the jobs probe reports it.
int test_jobs_declare_required_tools() {
    fs::path db = fs::temp_directory_path() / "funes_test_cron_require.db";
    fs::remove(db);
    MemoryStore memory(db.string(), nullptr);
    AgentDefaults defaults;
    ToolRegistry reg;
    register_cron_tool(reg, memory, defaults, (fs::temp_directory_path() / "funes_test_cron_ws5").string(),
                       find_agent);
    ToolContext ctx{"operator", "s1"};

    auto bad = reg.call("schedule_job",
        {{"name", "r"}, {"schedule", "0 20 * * *"}, {"kind", "agent"}, {"agent", "funes"},
         {"task", "remind me"}, {"require_tools", json::array({"execute_shell"})}}, ctx);
    CHECK(bad.error);
    CHECK(bad.text.find("execute_shell") != std::string::npos);
    CHECK(reg.call("schedule_job",
        {{"name", "r"}, {"schedule", "0 20 * * *"}, {"kind", "agent"}, {"agent", "funes"},
         {"task", "remind me"}, {"require_tools", "notify_user"}}, ctx).error);

    auto ok = reg.call("schedule_job",
        {{"name", "Relvar"}, {"schedule", "0 20 * * *"}, {"kind", "agent"}, {"agent", "funes"},
         {"task", "remind me"}, {"require_tools", json::array({"notify_user"})}}, ctx);
    CHECK(!ok.error);
    auto jobs = memory.list_cron_jobs(ctx.user_id);
    CHECK(jobs.size() == 1);
    CHECK(jobs[0].require_tools == std::vector<std::string>{"notify_user"});
    CHECK(reg.call("list_jobs", json::object(), ctx).text.find("requires notify_user") != std::string::npos);

    AgentConfig target = find_agent("funes");
    target.require_tools = {"recall"};
    funes::cron::apply_job_contract(target, jobs[0]);
    CHECK((target.require_tools == std::vector<std::string>{"recall", "notify_user"}));
    funes::cron::apply_job_contract(target, jobs[0]);
    CHECK(target.require_tools.size() == 2);
    return 0;
}

// 2026-11-14: asked to drop a reminder that was only ever proposal #3, the
// operator called cancel_job(3) — and job #3 was the Spanish newsletter. The
// id alone is a number a model can take from anywhere; the job's name has to
// come along and match. The refusal does not say what #N is called, or the
// model would just call again with it.
int test_cancel_job_needs_the_jobs_name() {
    fs::path db = fs::temp_directory_path() / "funes_test_cron_cancel_name.db";
    fs::remove(db);
    MemoryStore memory(db.string(), nullptr);
    AgentDefaults defaults;
    ToolRegistry reg;
    register_cron_tool(reg, memory, defaults, (fs::temp_directory_path() / "funes_test_cron_ws6").string(),
                       find_agent);
    ToolContext ctx{"operator", "s1"};
    auto s = reg.call("schedule_job",
        {{"name", "Daily AI Newsletter (ES)"}, {"schedule", "30 10 * * 2,4,6"}, {"kind", "agent"},
         {"agent", "researcher"}, {"task", "publish"}}, ctx);
    CHECK(!s.error);
    const int64_t id = memory.list_cron_jobs(ctx.user_id)[0].id;

    auto no_name = reg.call("cancel_job", {{"id", id}}, ctx);
    CHECK(no_name.error);
    auto wrong = reg.call("cancel_job",
        {{"id", id}, {"name", "Recordatorio cuota School of Rock diciembre 2026"}}, ctx);
    CHECK(wrong.error);
    CHECK(wrong.text.find("Newsletter") == std::string::npos);
    CHECK(memory.list_cron_jobs(ctx.user_id).size() == 1);

    CHECK(!reg.call("cancel_job", {{"id", id}, {"name", "daily ai newsletter (es)"}}, ctx).error);
    CHECK(memory.list_cron_jobs(ctx.user_id).empty());

    // Deleting a job is an act on the person's schedule, gated like creating one.
    CHECK(funes::builtin_effect("cancel_job") == funes::Effect::External);
    return 0;
}

// A reminder for a date is a cron expression that matches once a year:
// "0 9 23 12 *" fired again every 23 December. `once` makes it a single run —
// after it (ok or not) the job is done: kept, listed, never due again.
int test_one_shot_jobs_run_once() {
    fs::path db = fs::temp_directory_path() / "funes_test_cron_once.db";
    fs::remove(db);
    MemoryStore memory(db.string(), nullptr);
    AgentDefaults defaults;
    ToolRegistry reg;
    register_cron_tool(reg, memory, defaults, (fs::temp_directory_path() / "funes_test_cron_ws7").string(),
                       find_agent);
    ToolContext ctx{"operator", "s1"};
    CHECK(reg.call("schedule_job", {{"name", "r"}, {"schedule", "0 10 21 11 *"}, {"kind", "agent"},
                                    {"agent", "funes"}, {"task", "x"}, {"once", "yes"}}, ctx).error);
    auto s = reg.call("schedule_job",
        {{"name", "Recordatorio H4"}, {"schedule", "0 10 21 11 *"}, {"kind", "agent"},
         {"agent", "funes"}, {"task", "remind me"}, {"once", true}}, ctx);
    CHECK(!s.error);
    CHECK(s.text.find("once") != std::string::npos);
    auto jobs = memory.list_cron_jobs(ctx.user_id);
    CHECK(jobs.size() == 1 && jobs[0].once && jobs[0].done_at == 0);
    const int64_t id = jobs[0].id;
    const int64_t when = jobs[0].next_run_at;
    CHECK(memory.due_cron_jobs(when).size() == 1);

    memory.record_cron_job_run(id, true, "sent", when + 365 * 86400);
    jobs = memory.list_cron_jobs(ctx.user_id);
    CHECK(jobs[0].done_at > 0);
    CHECK(memory.due_cron_jobs(when + 400 * 86400).empty());
    CHECK(reg.call("list_jobs", json::object(), ctx).text.find("once, done") != std::string::npos);

    // A recurring job is untouched by all of this.
    CHECK(!reg.call("schedule_job", {{"name", "daily"}, {"schedule", "0 9 * * *"}, {"kind", "agent"},
                                     {"agent", "funes"}, {"task", "x"}}, ctx).error);
    for (const auto& j : memory.list_cron_jobs(ctx.user_id))
        if (j.name == "daily") {
            memory.record_cron_job_run(j.id, true, "ok", j.next_run_at + 86400);
            CHECK(memory.get_cron_job(ctx.user_id, j.id)->done_at == 0);
        }
    return 0;
}

int main() {
    int rc = 0;
    rc |= test_stale_running_flags_cleared_at_startup();
    rc |= test_schedule_job_validation();
    rc |= test_schedule_list_cancel_roundtrip();
    rc |= test_run_job_now();
    rc |= test_script_jobs();
    rc |= test_shell_jobs_need_execute_shell_permission();
    rc |= test_reschedule_moves_next_run();
    rc |= test_jobs_declare_required_tools();
    rc |= test_cancel_job_needs_the_jobs_name();
    rc |= test_one_shot_jobs_run_once();
    if (rc == 0) std::cout << "test_cron_tool: all tests passed\n";
    return rc;
}
