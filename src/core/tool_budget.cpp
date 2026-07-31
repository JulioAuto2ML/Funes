// =============================================================================
// src/core/tool_budget.cpp — see tool_budget.h
// =============================================================================

#include "tool_budget.h"

namespace funes {

bool over_budget(const ToolLimits& limits, const std::string& tool,
                 int calls_including_this) {
    auto it = limits.find(tool);
    if (it == limits.end() || it->second < 0) return false;
    return calls_including_this > it->second;
}

std::string budget_message(const std::string& tool, int limit) {
    return "This agent's budget for " + tool + " is " + std::to_string(limit) +
           " call(s) per run, and it is used up. This call and any further "
           + tool + " call will not run. Answer now from what you already "
           "have — if it is genuinely not enough, say what is missing and "
           "what you did find, rather than trying again.";
}

std::string withheld_notice() {
    return "No tools are available for this turn. None will run, and a tool "
           "call written out as text will be discarded rather than executed — "
           "so writing one costs you the turn and produces nothing. Give the "
           "final answer now, using only what is already in this conversation. "
           "If what you have is thin, say what you found and what is missing; "
           "that is still an answer, and it is the only kind available here.";
}

} // namespace funes
