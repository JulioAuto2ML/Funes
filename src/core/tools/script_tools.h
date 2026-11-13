// =============================================================================
// src/core/tools/script_tools.h — what run_script would refuse, asked early
// =============================================================================
// The effects gate (agent.cpp) stores a call as a proposal for the person to
// approve. A call run_script would refuse anyway — a script not granted to
// the agent, not installed, admin-only, or with bad arguments — must not
// become one: on 2026-11-13 sysadmin invented `disk_usage`, the gate proposed
// it, the person approved it, and only then did it fail. The gate asks this
// first and hands the refusal back to the model, which can still correct it.
#pragma once
#include "../tools.h"
#include "json.hpp"
#include <string>

namespace funes {

// "" when run_script would accept the call; otherwise its error text.
std::string run_script_refusal(const std::string& scripts_dir, const nlohmann::json& args,
                               const ToolContext& ctx);

} // namespace funes
