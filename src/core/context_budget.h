// =============================================================================
// src/core/context_budget.h — keep one run's transcript inside the window
// =============================================================================
//
// context_compressor folds *earlier conversations* into a summary before a run
// starts. Nothing bounded what a run accumulates while it runs: every tool
// result and every call's arguments stay in the transcript until the run ends.
// On 2026-10-30 curator's Spanish issue died at step ~26 with "request (32849
// tokens) exceeds the available context size (32768 tokens)" — three harvest
// pools and eight full publish_issue argument sets, none of them still needed.
//
// trim_to_fit runs before every completion in FunesAgent::run_loop. It is
// deterministic (no model call — the window is already full) and shrinks the
// oldest material first, in this order, stopping as soon as the estimate fits:
//
//   1. images on old messages     (~800 tokens each, already looked at)
//   2. old tool results           (head kept; a stored result's id is in the
//                                  head, so read_result can still reach it)
//   3. old tool calls' arguments  (replaced by a stub object: the call keeps
//                                  its id and name, so the transcript stays a
//                                  well-formed call → result sequence)
//   4. old assistant prose
//
// Steps 2 and 3 first skip the newest result and the newest call of each tool
// and take those last: curator's latest harvest pool is older than the pages
// it read afterwards, and it is the pool the issue is published from.
//
// Never touched: system messages, user messages (the task and any nudge), and
// everything from the latest assistant message on — the step the model is in
// the middle of. If those alone don't fit, `fits` is false and the request is
// sent anyway: the server's refusal is then an honest failure, not something
// this layer can fix by deleting the task.
// =============================================================================

#pragma once
#include "llm_client.h"
#include <string>
#include <vector>

namespace funes {

struct TrimOutcome {
    int  before = 0;           // estimated tokens (messages + overhead) on entry
    int  after  = 0;           // ...and on exit
    bool fits   = true;        // after <= budget
    int  images        = 0;    // images dropped
    int  tool_results  = 0;    // tool messages shortened
    int  tool_calls    = 0;    // tool-call argument sets replaced
    int  assistant_texts = 0;  // assistant prose shortened
    bool trimmed() const { return images + tool_results + tool_calls + assistant_texts > 0; }
};

// Shrinks `history` in place until estimate_tokens(history) + overhead_tokens
// <= budget_tokens, or there is nothing left it may shrink.
TrimOutcome trim_to_fit(std::vector<ChatMessage>& history, int budget_tokens,
                        int overhead_tokens = 0);

// True for a backend's "the prompt is larger than the context window" refusal
// (llama.cpp, OpenAI-compatible, Anthropic), as opposed to any other error.
bool is_context_overflow(const std::string& error_text);

// The prompt size the server reported in such a refusal (llama.cpp's
// n_prompt_tokens, or the first "N tokens" figure), 0 when it gave none.
int reported_prompt_tokens(const std::string& error_text);

}  // namespace funes
