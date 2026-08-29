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

// Past this, an over-long post is not a post that needs trimming — it is the
// model having misunderstood the field (pasting the article, or the whole
// issue, into one item). Trimming that produces a stub with a link, so it is
// still rejected outright.
constexpr size_t kMaxTrimmablePostChars = kMaxPostChars * 2;

// Characters, not bytes — the post limit is a character limit, and an accented
// word must not count double against it. Exposed because shorten_post's
// contract is stated in these units.
size_t utf8_length(const std::string& s);

// Cuts `text` to at most `max_chars`, at a sentence boundary if there is a
// usable one and a word boundary otherwise (with an ellipsis). Exposed for
// testing, and because build_issue both *suggests* its output to the model and
// — on a second attempt — applies it.
std::string shorten_post(const std::string& text, size_t max_chars = kMaxPostChars);

// Resolves `selection` against a pool record (harvest::pool_record) and builds
// the issue. Empty return = `out` is the issue, ready to publish. Otherwise the
// violations and `out` is untouched.
//
// Two classes of problem, handled differently on purpose:
//
//   Contract violations — an unknown id, a duplicate story, an empty or
//     over-long post, a missing emoji — reject the whole issue. The model got
//     the format wrong and can fix it.
//
//   Ungroundable items — the post text is supported by no page in the pool,
//     not even after trying every unused candidate — are DROPPED, listed in
//     `dropped`, and the rest of the issue ships. Rejecting everything for one
//     bad item meant a model that could not fix it (because the code had
//     already proved no candidate matched) retried with identical arguments
//     until the loop detector killed the run: three attempts, no newsletter.
//
// `min_items` is the floor: if too few survive the drops, the issue is
// rejected after all and the reasons are in the returned string. `dropped` may
// be null if the caller does not care, but publish_issue wants it for the run
// record and for telling the model what it lost.
// `trim_over_length` is what the caller sets once it has already rejected this
// publication+date for a post that was too long. The first rejection hands the
// model a ready-made shorter version to copy; if it comes back over the limit
// anyway, the second call trims deterministically and publishes.
//
// That split is not timidity, it is what the 2026-08-29 failure showed. Told
// its post was "329 characters; the limit is 280", the model resubmitted the
// same text byte-for-byte and the loop detector killed the run — no newsletter.
// In the very same run it had corrected eight wrong ids without a single
// mistake, because for those the tool named the right value instead of only
// naming the problem. Supplying the answer is what works; asking for prose
// arithmetic is not.
std::string build_issue(const nlohmann::json& pool,
                        const std::vector<Selection>& selection,
                        nlohmann::json& out,
                        int min_items = 1,
                        std::vector<std::string>* dropped = nullptr,
                        bool trim_over_length = false);

// Which pool publish_issue resolves ids against.
//
// Split out and filesystem-free so it can be tested: on 2026-08-26 the "no
// date given" path silently fell back to the newest pool on disk. Harvest had
// failed that morning, so the newest was the previous day's — and today's post
// text was grounded against yesterday's pages. It only surfaced because the
// overlap check rejected almost everything; had a few posts matched by
// coincidence, the issue would have shipped yesterday's URLs under today's
// headlines. That is the one failure this whole pipeline exists to prevent.
//
// `requested` empty means "today's issue": today's pool must exist, and no
// other date will do. A non-empty `requested` is a deliberate republish of an
// older day and is honoured if that pool exists.
struct PoolChoice {
    std::string date;    // empty when `error` is set
    std::string error;
};
PoolChoice resolve_pool_date(const std::vector<std::string>& available_dates,
                             const std::string& requested,
                             const std::string& today);

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
