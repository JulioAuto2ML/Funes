// =============================================================================
// src/core/cron_runner.h — background scheduler for cron_jobs
// =============================================================================
// Polls MemoryStore::due_cron_jobs on a timer and runs each due job — an
// agent task via FunesAgent::run (the same in-process call delegate_to_agent
// already makes, src/core/tools/delegation.cpp) or a shell command via
// funes::proc::run_shell_command (src/core/tools/process_runner.h). Same
// detached-background-thread shape as the memory-consolidation loop in
// src/server/main.cpp.
//
// A failed run is recorded, not retried: list_jobs is how a human (or the
// operator agent) notices and re-runs it with run_job_now — the same "check
// the record, don't trust the model" pattern publish_issue's run records use.
// =============================================================================

#pragma once
#include "agent_config.h"
#include "memory.h"
#include "tools.h"
#include "permissions.h"
#include <cstdint>
#include <functional>
#include <string>
#include <utility>

// Forward-declared to avoid pulling all of agent.h's dependents into every
// translation unit that only wants to start the runner (src/server/main.cpp).
struct AgentDefaults;

namespace funes::cron {

using FindAgentFn = std::function<AgentConfig(const std::string&)>;

// Resolves a job owner's permissions at fire time, not at schedule time: an
// admin revoking someone's shell access should take effect on their existing
// jobs too. Left unset (or returning unrestricted) in contexts with no user
// table, e.g. tests.
using FindPermissionsFn = std::function<funes::Permissions(int64_t user_id)>;

// Starts the poll loop on a detached background thread and returns
// immediately. `memory`, `tools`, and `find_agent`'s captured state must
// outlive the process — true for everything main() constructs before
// calling this.
void start_cron_runner(MemoryStore& memory, ToolRegistry& tools,
                       const AgentDefaults& defaults, const std::string& workspace_dir,
                       FindAgentFn find_agent, FindPermissionsFn find_permissions,
                       int poll_seconds);

// Runs one job immediately, out of band from the poll loop, and records the
// outcome exactly as a scheduled firing would — for the run_job_now tool
// (src/core/tools/cron_tool.cpp), so a job definition can be tested without
// waiting on its schedule. Blocks the caller until the job finishes (an
// agent-kind job runs its full tool loop synchronously, the same way
// delegate_to_agent runs a sub-agent). Returns {false, reason} if `job_id`
// doesn't exist.
std::pair<bool, std::string> run_cron_job_now(MemoryStore& memory, ToolRegistry& tools,
                                              const AgentDefaults& defaults,
                                              const std::string& workspace_dir,
                                              const FindAgentFn& find_agent,
                                              const FindPermissionsFn& find_permissions,
                                              int64_t job_id);

} // namespace funes::cron
