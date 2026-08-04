// =============================================================================
// src/core/completion_contract.h — "you are not done yet" for the agent loop
// =============================================================================
// FunesAgent::run_loop used to treat *any* assistant message without tool calls
// as the final answer. That gives a model a free exit from a multi-step task at
// every step boundary, and local models take it: they narrate the last tool
// result as if it were the deliverable and stop. Prompt wording can discourage
// it, but the exit stays open, so the failure just moves to the next boundary.
//
// A completion contract closes it. An agent declares the tools that must have
// SUCCEEDED before a plain-text answer counts as final (AgentConfig::
// require_tools). Until then, an attempt to answer is answered with a nudge and
// the loop keeps going; if the model won't comply within its nudge budget, the
// run ends in an explicit failure rather than a plausible-sounding lie.
//
// This is deliberately per-agent config and not newsletter-specific: any agent
// whose value is in the side effects of its last tool call wants it.

#pragma once
#include <set>
#include <string>
#include <vector>

namespace funes {

struct CompletionContract {
    // Tool names, in the order the agent's prompt asks for them.
    std::vector<std::string> required;

    bool active() const { return !required.empty(); }

    // `required` entries absent from `satisfied`, in declaration order.
    // Duplicates in `required` are reported once. `satisfied` is built by
    // agent.cpp's run_loop as "the MOST RECENT call to this tool name
    // succeeded" — a later failure erases an earlier success (see the
    // insert/erase pair around ToolResult.error there) — not "succeeded at
    // least once ever". That distinction matters for an agent that calls the
    // same required tool more than once for different purposes, e.g.
    // ai-newsletter's delegate_to_agent (once to research, once to publish):
    // without the erase, the first call's success would permanently satisfy
    // the contract regardless of whether the second one failed.
    std::vector<std::string> missing(const std::set<std::string>& satisfied) const;

    // How many times the model may try to finish early before the run is failed.
    // Scaled to the contract: each outstanding call is worth one reminder, plus
    // slack for a model that stalls twice on the same one.
    int nudge_budget() const { return static_cast<int>(required.size()) + 2; }
};

// Injected as a user turn when the model answers with `missing` outstanding.
// Phrased as a correction of the immediate move, not a restatement of the task
// — the task is already in the system prompt, and repeating it there invites
// the model to summarize the plan again instead of acting on it.
std::string contract_nudge(const std::vector<std::string>& missing);

// Final answer when the nudge budget is spent. Must read as a failure to a
// human *and* to a delegating agent, which sees only this string: the whole
// point is that "reported sent, never sent" becomes impossible to produce.
std::string contract_failure(const std::vector<std::string>& missing,
                             const std::string& model_answer);

} // namespace funes
