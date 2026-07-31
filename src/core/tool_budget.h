#pragma once
// =============================================================================
// src/core/tool_budget.h — per-tool call ceilings
// =============================================================================
// The ceiling to require_tools' floor (core/completion_contract.h): that says
// a tool call *must* happen before an answer is accepted, this says it may not
// happen more than N times in one run.
//
// It exists because a research agent will happily search forever. On
// 2026-07-31 `researcher` issued 20 distinct web_search calls without ever
// synthesizing, twice in one pipeline run, and `newsletter-publisher` did the
// same when it was handed a task with no research in it — half an hour of GPU
// for nothing sent. The loop detector eventually stopped them, but stopping a
// run is not the same as getting an answer out of it.
//
// So going over budget is deliberately *recoverable*: the call is refused with
// an error the model can read, telling it to conclude from what it already
// has. The run continues and usually produces something. Compare the loop
// detector, which kills the run outright — that stays as the backstop.
//
// The refusal alone is only a request, though, and a model is free to ignore
// it: on 2026-07-31 the 9B re-issued the refused web_search seven times in a
// row, each refusal costing a step, until max_steps ended `researcher` with no
// answer and the whole newsletter pipeline collapsed behind it. So the agent
// loop also withholds tools (tool_choice "none") on the completion right after
// a refusal — see force_no_tools in agent.cpp. Being told to conclude and
// having nothing else available are different things; only the second worked.

#include <map>
#include <string>

namespace funes {

// tool name → maximum calls per run. Declared in YAML as `tool_limits:`.
using ToolLimits = std::map<std::string, int>;

// True if this call must be refused. `calls_including_this` counts the call
// being considered, so a limit of 5 permits calls 1-5 and refuses the 6th.
//
// A tool with no entry is unlimited. A limit of 0 forbids the tool outright
// (useful for taking one away from a single run). A negative limit is treated
// as no limit: it is a config typo, and silently disabling a tool is a worse
// failure than ignoring the line.
bool over_budget(const ToolLimits& limits, const std::string& tool,
                 int calls_including_this);

// What the model is told in place of the tool result. Must read as final —
// a model that takes it for a transient error retries and burns its steps.
std::string budget_message(const std::string& tool, int limit);

// Injected as a user turn on any completion where tools are withheld — after a
// refusal, and on the last step. Dropping the schema is necessary but not
// sufficient: on 2026-07-31 `ai-newsletter` was handed a tool-free final turn
// and wrote a <tool_call> blob anyway, twice, because its own transcript was
// full of them and nothing said the situation had changed. Taking the option
// away silently only works on a model that notices.
std::string withheld_notice();

} // namespace funes
