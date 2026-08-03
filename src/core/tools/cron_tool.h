// =============================================================================
// src/core/tools/cron_tool.h — schedule_job / list_jobs / cancel_job / run_job_now
// =============================================================================
// The agent-facing surface of Funes' in-process scheduler (see
// core/cron_schedule.h for the expression syntax and core/cron_runner.h for
// what actually fires a due job). Only `operator` gets these tools
// (agents/operator.yaml) — scheduling is an ops capability, same as
// read_file/write_file/execute_shell.
// =============================================================================

#pragma once
#include "../agent_config.h"
#include "../memory.h"
#include "../tools.h"
#include <functional>
#include <string>

// Forward-declared rather than pulling in all of agent.h (see cron_runner.h).
struct AgentDefaults;

// find_agent looks up a target persona by name for kind="agent" jobs — an
// empty-name AgentConfig means "not found", validated at schedule time
// (schedule_job) as well as at run time (the scheduler itself), the same
// two-point check delegate_to_agent does.
void register_cron_tool(ToolRegistry& reg, MemoryStore& memory, const AgentDefaults& defaults,
                        const std::string& workspace_dir,
                        std::function<AgentConfig(const std::string&)> find_agent);
