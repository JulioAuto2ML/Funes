// =============================================================================
// src/core/tools/issue.h — turning a selection into an issue
// =============================================================================
// The other half of harvest.h. The model has read a numbered pool and comes
// back with numbers, some post text, and — for each item — a quote from the
// page it is writing about. This file turns that into the issue record the
// publisher renders and sends, and refuses to when it doesn't hold up.
//
// Two checks do the real work, and both are here rather than in a prompt
// because a prompt is a request and these are guarantees:
//
//   Resolution. An item names a candidate by id; the URL comes from the pool,
//   never from the model. An id that isn't in the pool is an error, not a
//   silently dropped item. This is what makes "the wrong link" unreachable
//   rather than merely unlikely.
//
//   Evidence. The model must quote a verbatim phrase from the candidate's page
//   supporting the claim its post makes, and the quote is checked against the
//   text harvest actually fetched. A post about an OpenAI breach cannot produce
//   a supporting quote from a Stripe checkout page — the failure that shipped
//   on 2026-07-31 becomes a deterministic reject.
//
// A failure is one concrete violation naming one item, in the style of
// validate_answer: a list of five complaints makes a small model rewrite
// everything at once and reliably break what was already right.

#pragma once
#include "json.hpp"
#include <string>
#include <vector>

namespace funes::issue {

// Case-folded, whitespace-collapsed, with typographic quotes and dashes mapped
// to their ASCII forms. A model retyping a phrase out of an excerpt renormalizes
// punctuation without meaning to, and rejecting it for that would be rejecting
// it for being a language model.
std::string normalize_for_match(const std::string& s);

// A quote shorter than this proves nothing: "the" occurs on every page.
constexpr size_t kMinEvidenceChars = 24;

// Empty = the page supports the quote. Otherwise a reason written to be read by
// the model that has to fix it.
//
// One elision is allowed — "the model said X … and Y" matches a page where X
// and Y are far apart, in that order. Two would let a model assemble a sentence
// the page never made.
std::string check_evidence(const std::string& quote, const std::string& page_text);

struct Selection {
    int         candidate_id = 0;
    std::string emoji;
    std::string text;
    std::string evidence;
};

// Posts go to X and LinkedIn, so the limit is X's.
constexpr size_t kMaxPostChars = 280;

// Resolves `selection` against a pool record (harvest::pool_record) and builds
// the issue. Empty return = `out` is the issue, ready to publish. Otherwise one
// violation and `out` is untouched.
std::string build_issue(const nlohmann::json& pool,
                        const std::vector<Selection>& selection,
                        nlohmann::json& out);

// The link label: the candidate's own headline, trimmed at a word boundary.
// Derived rather than asked for — it is the article's title, and the pool
// already holds it.
std::string headline_from_title(const std::string& title, size_t max_chars = 60);

} // namespace funes::issue

class ToolRegistry;

// publish_issue. `workspace_dir` holds the pool, the issues and the run records;
// `publishing_dir` is the repo's publishing/ (see publishing/README.md), which
// does the rendering, the link re-check and the send; `publications_dir` holds
// the configs that say how many items an issue needs, what its subject line is,
// and which artifacts and channels it has.
void register_publish_issue_tool(ToolRegistry& reg,
                                 const std::string& workspace_dir,
                                 const std::string& publishing_dir,
                                 const std::string& publications_dir);
