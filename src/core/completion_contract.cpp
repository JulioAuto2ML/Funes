// =============================================================================
// src/core/completion_contract.cpp
// =============================================================================

#include "completion_contract.h"

namespace funes {

std::vector<std::string> CompletionContract::missing(
    const std::set<std::string>& satisfied) const {
    std::vector<std::string> out;
    std::set<std::string> seen;
    for (const auto& tool : required) {
        if (satisfied.count(tool)) continue;
        if (!seen.insert(tool).second) continue;   // named twice in the prompt
        out.push_back(tool);
    }
    return out;
}

// Joins as "a, b and c" — the nudge is read by a model, and a bare
// comma-separated tail reads as one more item rather than the last one.
static std::string join_tools(const std::vector<std::string>& tools) {
    std::string s;
    for (size_t i = 0; i < tools.size(); ++i) {
        if (i > 0) s += (i + 1 == tools.size()) ? " and " : ", ";
        s += "`" + tools[i] + "`";
    }
    return s;
}

std::string contract_nudge(const std::vector<std::string>& missing) {
    return "Stop. That was a text answer, but you have not called " +
           join_tools(missing) +
           " yet in this conversation, and the task is not complete without "
           "them. Do not apologize, re-plan, or describe what you are about "
           "to do. Emit the next tool call now — starting with " +
           join_tools({missing.front()}) + ".";
}

std::string contract_failure(const std::vector<std::string>& missing,
                             const std::string& model_answer) {
    std::string s = "FAILED — this run did not complete. The following required "
                    "tool calls never happened: " + join_tools(missing) +
                    ". Any side effect they were responsible for (a file "
                    "written, a message sent) did NOT occur.";
    if (!model_answer.empty())
        s += " The unverified claim made instead was: \"" + model_answer + "\"";
    return s;
}

} // namespace funes
