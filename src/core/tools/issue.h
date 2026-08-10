// =============================================================================
// src/core/tools/issue.h — turning a selection into an issue
// =============================================================================
// The other half of harvest.h. The model has read a numbered pool and comes
// back with numbers and some post text. This file turns that into the issue
// record the publisher renders and sends, and refuses to when it doesn't
// hold up.
//
// Two checks do the real work, and both are here rather than in a prompt
// because a prompt is a request and these are guarantees:
//
//   Resolution. An item names a candidate by id; the URL comes from the pool,
//   never from the model. An id that isn't in the pool is an error, not a
//   silently dropped item. This is what makes "the wrong link" unreachable
//   rather than merely unlikely.
//
//   Grounding. The system extracts a sentence from the candidate's page that
//   shares enough content words with the post text to prove the post is about
//   that page. A post about an OpenAI breach linked to a Stripe checkout page
//   will share zero content words and be rejected deterministically — no model
//   involvement in the check at all.
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
// to their ASCII forms.
std::string normalize_for_match(const std::string& s);

// Minimum content-word overlap between a post and a page sentence for the post
// to be considered grounded in that page.
constexpr int kMinOverlap = 3;

// Deterministic grounding: finds the sentence in `page_text` that best supports
// `post_text` by content-word overlap. Returns {sentence, ""} on success, or
// {"", reason} when the post cannot be grounded in the page.
std::pair<std::string, std::string> extract_evidence(
    const std::string& post_text, const std::string& page_text);

struct Selection {
    int         candidate_id = 0;
    std::string emoji;
    std::string text;
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
