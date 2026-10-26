#pragma once
// =============================================================================
// src/core/tool_log.h — one stderr line per tool call
// =============================================================================
// The journal used to show a delegated run as two lines — `[delegate] funes →
// home ... task=...` and the final `OK: <answer>` — and nothing in between.
// On 2026-10-21 `home` answered that schedule_job was "not available", and
// there was no way to tell from the log whether it had called schedule_job and
// got an error, spent its tool_limits budget (which takes the tool out of the
// schema), or simply hit the last-step withholding. Each of those has a
// different fix, so the log has to say which one happened.
//
// Pure formatting, so the line can be tested without an LLM; agent.cpp does
// the writing. One line per call, built whole and written with one `<<` so
// concurrent runs (cron thread, two chats) don't interleave mid-line.

#include "json.hpp"
#include <string>

namespace funes {

enum class ToolCallOutcome {
    Ok,
    Error,          // the tool ran (or was unknown) and reported an error
    BudgetRefused,  // tool_limits spent — never dispatched
    PermissionDenied,  // the caller's permissions refuse a call key — never dispatched
    RepeatRefused,  // identical to earlier calls this run (tool_budget.h) — never dispatched
};

// Compact JSON of a call's arguments, at most `max_bytes` (UTF-8 safe, "…"
// appended when cut). Any object key that names a credential — password,
// token, secret, api_key, authorization, cookie, credential, private_key, at
// any depth, case-insensitive — has its value replaced by "***" before the
// dump, so a truncation boundary can never reveal half of one. This catches
// what a key says; a secret pasted into free text (a shell command) is not
// something a summary can recognise, which is why it is short.
std::string summarize_tool_args(const nlohmann::json& args, size_t max_bytes = 120);

// `detail` is the tool's error text for Error, or what refused the call for
// the two refusals (e.g. "run_script limit 3"); ignored for Ok. It is cut to
// `max_detail_bytes` and folded onto one line. `key` is the most specific
// call key (`run_script:tv_art`, not `run_script`) — the one a budget,
// contract or permission entry would name.
//
//   [tool] home (depth 1) schedule_job args={"cron":"45 6-23 * * *",...} → error: ...
std::string format_tool_call_line(const std::string& agent, int depth,
                                  const std::string& key,
                                  const std::string& args_summary,
                                  ToolCallOutcome outcome,
                                  const std::string& detail = "",
                                  size_t max_detail_bytes = 160);

} // namespace funes
