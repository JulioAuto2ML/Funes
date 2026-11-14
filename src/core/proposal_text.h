// =============================================================================
// src/core/proposal_text.h — what a proposal does, in the code's words (7.23)
// =============================================================================
// A proposal (proposals.h) used to reach the person only through the model's
// paraphrase. On 2026-11-14 the curator, asked to retry one LinkedIn post,
// proposed publish_issue — re-sending the whole newsletter — and described it
// as "the post is ready"; the person approved it, and only publish_issue's
// already-sent guard stopped a second mailing. So what a call does is written
// here, from the tool and its arguments, and it is what the person reads:
// appended to the reply of the run that made the proposal, and repeated in
// the answer to «aprobar N». The model's own words come before it, never
// instead of it.
// =============================================================================

#pragma once
#include "json.hpp"
#include "proposals.h"
#include <string>
#include <vector>

namespace funes {

// One clause: "enviar el newsletter ai-pulse-es del 2026-11-14 a todos sus
// suscriptores". Tools without a sentence here get their name and a compact,
// credential-masked dump of the arguments — plain, but never a wrong story.
std::string describe_call(const std::string& tool, const nlohmann::json& args, bool es);

// The block appended to a reply for the proposals its run created, "" for
// none: one line per proposal with the agent, describe_call and how to answer.
std::string proposals_footer(const std::vector<Proposal>& made, bool es);

} // namespace funes
