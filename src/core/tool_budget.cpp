// =============================================================================
// src/core/tool_budget.cpp — see tool_budget.h
// =============================================================================

#include "tool_budget.h"
#include "script_library.h"

namespace funes {

std::vector<std::string> call_keys(const std::string& tool, const nlohmann::json& args) {
    std::vector<std::string> keys{tool};
    if (tool != "run_script" || !args.is_object() || !args.contains("name")
        || !args["name"].is_string())
        return keys;
    // Sanitized, and only when it survives unchanged: the qualified key must
    // be the same string a YAML author would write. A name the library would
    // reject anyway ("../secret") stays unqualified and is refused downstream
    // — it must not become a second key nobody can write a limit for.
    const std::string name = args["name"].get<std::string>();
    if (!name.empty() && safe_script_name(name) == name)
        keys.push_back(tool + ":" + name);
    return keys;
}

bool over_budget(const ToolLimits& limits, const std::string& tool,
                 int calls_including_this) {
    auto it = limits.find(tool);
    if (it == limits.end() || it->second < 0) return false;
    return calls_including_this > it->second;
}

std::string budget_message(const std::string& key, int limit) {
    // A qualified key ("run_script:backup_workspace") reads as one thing here
    // on purpose: what is used up is that script, not run_script as a whole,
    // and a model told "run_script is used up" stops trying the other two it
    // was granted.
    return "This agent's budget for " + key + " is " + std::to_string(limit) +
           " call(s) per run, and it is used up. This call and any further "
           + key + " call will not run. Answer now from what you already "
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
