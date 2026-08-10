// =============================================================================
// src/core/cron_runner.cpp — cron_runner implementation
// =============================================================================

#include "cron_runner.h"
#include "agent.h"
#include "cron_schedule.h"
#include "funes_config.h"
#include "run_outcome.h"
#include "text_utils.h"
#include "tools/process_runner.h"
#include <chrono>
#include <ctime>
#include <iostream>
#include <thread>
#include <tuple>

namespace funes::cron {

namespace {

// Job outputs are read by a human via list_jobs, not fed back into an LLM
// context — bounded generously compared to result_store's kInlineLimit, but
// still bounded, since a runaway script's stdout must not grow the DB row
// without limit.
constexpr size_t kOutputPreviewBytes  = 4000;
constexpr size_t kMaxShellOutputBytes = 16 * 1024;

void bound_preview(std::string& s) {
    if (s.size() > kOutputPreviewBytes) {
        funes::truncate_utf8_safe(s, kOutputPreviewBytes);
        s += "…";
    }
}

// Runs an agent-kind job in-process — the same call delegate_to_agent makes
// (src/core/tools/delegation.cpp), no HTTP hop. persist=true (unlike
// delegate_to_agent's false) because there is no orchestrator here to relay
// the answer to: the job's own session, "cron-<id>", is the record.
//
// Hyphen, not the colon a "cron:<id>" reads more naturally with: session ids
// reach the HTTP API's /api/history and /api/chat routes (FunesApi::
// valid_session, src/server/api.cpp), whose regex is [A-Za-z0-9_-]{1,64} —
// a colon would make the job's own session unreadable through the API that
// list_jobs points a human at.
std::pair<bool, std::string> run_agent_job(const MemoryStore::CronJob& job,
                                           ToolRegistry& tools, MemoryStore& memory,
                                           const AgentDefaults& defaults,
                                           const FindAgentFn& find_agent) {
    AgentConfig target = find_agent(job.agent);
    if (target.name.empty())
        return {false, "Unknown agent '" + job.agent + "'"};

    try {
        FunesAgent sub(target, tools, memory, defaults);
        // Each run gets its own session so tool budgets don't carry over
        // from a previous run of the same job.
        const auto now = std::chrono::system_clock::now();
        const auto epoch = std::chrono::duration_cast<std::chrono::seconds>(
            now.time_since_epoch()).count();
        const std::string session = "cron-" + std::to_string(job.id) +
                                    "-" + std::to_string(epoch);
        std::string out = sub.run(job.task, session,
                                  nullptr, {}, /*persist=*/true);
        const bool ok = !funes::is_run_failure(out);
        bound_preview(out);
        return {ok, out};
    } catch (const std::exception& e) {
        return {false, std::string("Agent run failed: ") + e.what()};
    }
}

// Runs a shell-kind job directly, no LLM involved — for deterministic
// scripts, the same job the crontab lines in publishing/README.md do today.
std::pair<bool, std::string> run_shell_job(const MemoryStore::CronJob& job,
                                           const std::string& workspace_dir) {
    if (!funes::shell_allowed())
        return {false, "Shell execution is disabled (set FUNES_ALLOW_SHELL=1 to enable it)"};

    const int timeout = funes::env_int("FUNES_CRON_SHELL_TIMEOUT", 120);
    funes::proc::Result r = funes::proc::run_shell_command(
        job.command, workspace_dir, timeout, kMaxShellOutputBytes);

    std::string out = r.output;
    if (r.timed_out) out += "\n[timed out after " + std::to_string(timeout) + "s]";
    bound_preview(out);
    return {r.exit_code == 0 && !r.timed_out, out};
}

// Runs `job`, computes its next occurrence, and records both — the shared
// tail end of a poll-triggered firing and an explicit run_job_now call.
std::pair<bool, std::string> execute_and_record(const MemoryStore::CronJob& job,
                                                ToolRegistry& tools, MemoryStore& memory,
                                                const AgentDefaults& defaults,
                                                const std::string& workspace_dir,
                                                const FindAgentFn& find_agent) {
    bool ok = false;
    std::string output;
    try {
        if (job.kind == "agent")
            std::tie(ok, output) = run_agent_job(job, tools, memory, defaults, find_agent);
        else
            std::tie(ok, output) = run_shell_job(job, workspace_dir);
    } catch (const std::exception& e) {
        ok = false;
        output = std::string("cron job crashed: ") + e.what();
    }

    // The schedule was already validated by schedule_job — this branch only
    // matters if a row was hand-edited in the DB. Push it a day out rather
    // than re-attempting (and re-failing) every poll tick.
    time_t next = time(nullptr) + 24 * 3600;
    try {
        next = funes::cron::next_run_after(funes::cron::parse_cron_expr(job.schedule),
                                           time(nullptr));
    } catch (const std::exception& e) {
        output += "\n[failed to compute next run: " + std::string(e.what()) + "]";
    }

    memory.record_cron_job_run(job.id, ok, output, static_cast<int64_t>(next));
    memory.mark_cron_job_running(job.id, false);
    return {ok, output};
}

} // namespace

void start_cron_runner(MemoryStore& memory, ToolRegistry& tools,
                       const AgentDefaults& defaults, const std::string& workspace_dir,
                       FindAgentFn find_agent, int poll_seconds) {
    std::thread([&memory, &tools, defaults, workspace_dir, find_agent, poll_seconds] {
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(poll_seconds));
            try {
                for (const auto& job : memory.due_cron_jobs(time(nullptr))) {
                    // Marked running synchronously, on the poll thread, before
                    // the job's own thread even starts — so the next tick
                    // (however soon) never sees it as due twice.
                    memory.mark_cron_job_running(job.id, true);
                    std::thread([&memory, &tools, defaults, workspace_dir, find_agent, job] {
                        execute_and_record(job, tools, memory, defaults, workspace_dir, find_agent);
                    }).detach();
                }
            } catch (const std::exception& e) {
                std::cerr << "[funes] cron poll failed: " << e.what() << "\n";
            }
        }
    }).detach();
}

std::pair<bool, std::string> run_cron_job_now(MemoryStore& memory, ToolRegistry& tools,
                                              const AgentDefaults& defaults,
                                              const std::string& workspace_dir,
                                              const FindAgentFn& find_agent, int64_t job_id) {
    auto job = memory.get_cron_job(job_id);
    if (!job) return {false, "No job with id " + std::to_string(job_id)};

    memory.mark_cron_job_running(job_id, true);
    return execute_and_record(*job, tools, memory, defaults, workspace_dir, find_agent);
}

} // namespace funes::cron
