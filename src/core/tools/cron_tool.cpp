// =============================================================================
// src/core/tools/cron_tool.cpp — schedule_job / list_jobs / cancel_job / run_job_now
// =============================================================================

#include "cron_tool.h"
#include "../agent.h"
#include "../cron_runner.h"
#include "../cron_schedule.h"
#include "../funes_config.h"
#include <ctime>
#include <sstream>

namespace {

std::string format_epoch(int64_t t) {
    if (t <= 0) return "never";
    std::time_t tt = static_cast<std::time_t>(t);
    struct tm lt;
    localtime_r(&tt, &lt);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &lt);
    return buf;
}

ToolResult schedule_job_handler(MemoryStore& memory,
                                const std::function<AgentConfig(const std::string&)>& find_agent,
                                const json& args, const ToolContext&) {
    if (!args.contains("name") || !args["name"].is_string() || args["name"].get<std::string>().empty())
        return {"Missing 'name' argument", true};
    if (!args.contains("schedule") || !args["schedule"].is_string()
        || args["schedule"].get<std::string>().empty())
        return {"Missing 'schedule' argument", true};
    if (!args.contains("kind") || !args["kind"].is_string())
        return {"Missing 'kind' argument (must be 'agent' or 'shell')", true};

    const std::string name     = args["name"].get<std::string>();
    const std::string schedule = args["schedule"].get<std::string>();
    const std::string kind     = args["kind"].get<std::string>();
    if (kind != "agent" && kind != "shell")
        return {"'kind' must be 'agent' or 'shell', got '" + kind + "'", true};

    funes::cron::CronExpr expr;
    try {
        expr = funes::cron::parse_cron_expr(schedule);
    } catch (const std::exception& e) {
        return {std::string("Bad schedule: ") + e.what(), true};
    }

    MemoryStore::CronJob job;
    job.name     = name;
    job.kind     = kind;
    job.schedule = schedule;

    if (kind == "agent") {
        const std::string agent = args.value("agent", "");
        const std::string task  = args.value("task", "");
        if (agent.empty()) return {"kind='agent' requires 'agent'", true};
        if (task.empty())  return {"kind='agent' requires 'task'", true};
        if (find_agent(agent).name.empty())
            return {"Unknown agent '" + agent + "'", true};
        job.agent = agent;
        job.task  = task;
    } else {
        const std::string command = args.value("command", "");
        if (command.empty()) return {"kind='shell' requires 'command'", true};
        if (!funes::shell_allowed())
            return {"Shell execution is disabled. Set FUNES_ALLOW_SHELL=1 to enable scheduled "
                    "shell jobs — they then run with the Funes process's own permissions.", true};
        job.command = command;
    }

    job.next_run_at = static_cast<int64_t>(funes::cron::next_run_after(expr, time(nullptr)));
    int64_t id = memory.create_cron_job(job);

    return {"Scheduled job #" + std::to_string(id) + " '" + name + "' (" + kind +
            "), schedule '" + schedule + "', next run " + format_epoch(job.next_run_at)};
}

ToolResult list_jobs_handler(MemoryStore& memory, const json&, const ToolContext&) {
    auto jobs = memory.list_cron_jobs();
    if (jobs.empty()) return {"No scheduled jobs."};

    std::ostringstream out;
    for (const auto& j : jobs) {
        out << "#" << j.id << " '" << j.name << "' (" << j.kind << ") — schedule '"
            << j.schedule << "'";
        if (j.kind == "agent") out << ", agent=" << j.agent;
        out << (j.running ? ", running now" : "") << "\n"
            << "  next run: " << format_epoch(j.next_run_at) << "\n"
            << "  last run: " << format_epoch(j.last_run_at);
        if (!j.last_status.empty()) {
            out << " (" << j.last_status << ")";
            if (!j.last_output.empty()) out << " — " << j.last_output;
        }
        out << "\n";
    }
    return {out.str()};
}

ToolResult cancel_job_handler(MemoryStore& memory, const json& args, const ToolContext&) {
    if (!args.contains("id") || !args["id"].is_number_integer())
        return {"Missing 'id' argument", true};
    int64_t id = args["id"].get<int64_t>();
    if (!memory.delete_cron_job(id))
        return {"No job with id " + std::to_string(id), true};
    return {"Cancelled job #" + std::to_string(id)};
}

ToolResult run_job_now_handler(MemoryStore& memory, ToolRegistry& tools,
                               const AgentDefaults& defaults, const std::string& workspace_dir,
                               const std::function<AgentConfig(const std::string&)>& find_agent,
                               const json& args, const ToolContext&) {
    if (!args.contains("id") || !args["id"].is_number_integer())
        return {"Missing 'id' argument", true};
    int64_t id = args["id"].get<int64_t>();

    auto [ok, output] = funes::cron::run_cron_job_now(memory, tools, defaults, workspace_dir,
                                                       find_agent, id);
    return {(ok ? "Job #" + std::to_string(id) + " ran successfully:\n" + output
                : "Job #" + std::to_string(id) + " failed:\n" + output),
            !ok};
}

} // namespace

void register_cron_tool(ToolRegistry& reg, MemoryStore& memory, const AgentDefaults& defaults,
                        const std::string& workspace_dir,
                        std::function<AgentConfig(const std::string&)> find_agent) {
    reg.add({
        "schedule_job",
        "Schedule a recurring job using a standard 5-field cron expression "
        "(minute hour day-of-month month day-of-week — e.g. '0 9 * * *' for 9am daily, "
        "'*/15 * * * *' for every 15 minutes). Two kinds: 'agent' runs a task through a "
        "named agent (for anything needing judgment or generation, like curating a "
        "newsletter); 'shell' runs a command directly with no LLM involved, for "
        "deterministic scripts — requires FUNES_ALLOW_SHELL=1. Returns the job's id.",
        {
            {"type", "object"},
            {"properties", {
                {"name",     {{"type", "string"}, {"description", "A short label for the job"}}},
                {"schedule", {{"type", "string"},
                             {"description", "5-field cron expression, e.g. '0 9 * * *'"}}},
                {"kind",     {{"type", "string"}, {"enum", json::array({"agent", "shell"})},
                             {"description", "'agent' or 'shell'"}}},
                {"agent",    {{"type", "string"},
                             {"description", "kind='agent': the target agent's name"}}},
                {"task",     {{"type", "string"},
                             {"description", "kind='agent': the task to give it, each run"}}},
                {"command",  {{"type", "string"},
                             {"description", "kind='shell': the command to run, each run"}}}
            }},
            {"required", json::array({"name", "schedule", "kind"})}
        },
        [&memory, find_agent](const json& args, const ToolContext& ctx) {
            return schedule_job_handler(memory, find_agent, args, ctx);
        }
    });

    reg.add({
        "list_jobs",
        "List every scheduled job: id, kind, schedule, next run time, and the "
        "status/output of its last run.",
        json(nullptr),
        [&memory](const json& args, const ToolContext& ctx) {
            return list_jobs_handler(memory, args, ctx);
        }
    });

    reg.add({
        "cancel_job",
        "Permanently remove a scheduled job by id (see list_jobs).",
        {
            {"type", "object"},
            {"properties", {{"id", {{"type", "integer"}}}}},
            {"required", json::array({"id"})}
        },
        [&memory](const json& args, const ToolContext& ctx) {
            return cancel_job_handler(memory, args, ctx);
        }
    });

    reg.add({
        "run_job_now",
        "Run a scheduled job immediately, out of band from its schedule, and record the "
        "outcome exactly as a normal firing would (see list_jobs). Useful for testing a "
        "job definition without waiting on its schedule. Blocks until the job finishes.",
        {
            {"type", "object"},
            {"properties", {{"id", {{"type", "integer"}}}}},
            {"required", json::array({"id"})}
        },
        [&reg, &memory, defaults, workspace_dir, find_agent](const json& args, const ToolContext& ctx) {
            return run_job_now_handler(memory, reg, defaults, workspace_dir, find_agent, args, ctx);
        }
    });
}
