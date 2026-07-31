// =============================================================================
// src/core/run_outcome.cpp — see run_outcome.h
// =============================================================================

#include "run_outcome.h"

namespace funes {

const char* const kFailureMarker = "FAILED — ";

bool is_run_failure(const std::string& answer) {
    return answer.rfind(kFailureMarker, 0) == 0;
}

namespace {
// "after web_search" / "" — bailout messages name the last tool when there is
// one, because that is the first thing worth looking at in the journal.
std::string after(const std::string& tool) {
    return tool.empty() ? "" : " The last tool to run was " + tool + ".";
}
} // namespace

std::string empty_answer_failure(const std::string& last_tool_name) {
    return std::string(kFailureMarker) +
           "this run did not produce an answer. The model returned empty "
           "content, and a second call with tools disabled returned empty "
           "content too." + after(last_tool_name) +
           " Nothing was concluded from the tool results — do not treat this "
           "as a result; run it again.";
}

std::string loop_failure(const std::string& tool, int times) {
    return std::string(kFailureMarker) +
           "this run did not produce an answer. It called " + tool + " " +
           std::to_string(times) + " times without making progress, so the "
           "loop was stopped. Whatever that tool returned was never turned "
           "into a conclusion.";
}

std::string max_steps_failure(int steps, const std::string& last_tool_name) {
    return std::string(kFailureMarker) +
           "this run did not produce an answer. It used all " +
           std::to_string(steps) + " of its allowed steps and stopped without "
           "concluding anything." + after(last_tool_name) +
           " Either the task needs more steps or it needs breaking up.";
}

} // namespace funes
