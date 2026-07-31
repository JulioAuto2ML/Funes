#pragma once
// =============================================================================
// src/core/run_outcome.h — how the agent loop reports that it has no answer
// =============================================================================
// run_loop can reach its end without the model ever producing one: an empty
// completion, the same tool called in a circle, max_steps exhausted. It used
// to fill those gaps with the last tool result, which reads as content and is
// indistinguishable from work — a raw web-search dump once travelled three
// levels up a delegation chain and was served to the user as a newsletter.
//
// So a run that has no answer says so, and says it in a shape a *caller* can
// detect: every message here starts with kFailureMarker, and delegation.cpp
// turns that into a tool error rather than passing it up as content. The
// builders take a tool *name* and never its text, so there is no parameter
// through which tool output could re-enter an answer.

#include <string>

namespace funes {

// The prefix every giving-up message carries. Also used by contract_failure
// (completion_contract.h) and schema_failure (answer_schema.h), which arrived
// at the same convention independently.
extern const char* const kFailureMarker;

// True if `answer` is one of this framework's failure messages. Deliberately
// a prefix test, not a search: a model reporting "the build FAILED — …" in a
// legitimate answer must not be mistaken for a failed run.
bool is_run_failure(const std::string& answer);

// The model returned no content, and the retry with tools disabled produced
// none either. `last_tool_name` may be empty if nothing ran.
std::string empty_answer_failure(const std::string& last_tool_name);

// The loop detector tripped: `tool` was called `times` times without progress.
std::string loop_failure(const std::string& tool, int times);

// max_steps was exhausted before the model produced a final answer.
std::string max_steps_failure(int steps, const std::string& last_tool_name);

} // namespace funes
